/* Submission and tracking.
 *
 * The enforcer's block template server holds the txs; this keeps the record
 * of them. A submission is handed straight to the enforcer, which has the
 * node check it for consensus validity and then keeps it in the template
 * mempool, never relaying it. After that, every poll asks the enforcer where
 * each open tx stands and reads the template the proxy is mining, and moves
 * the row accordingly.
 *
 * The enforcer forgets a tx when it leaves its mempool, and forgets all of
 * them when it restarts or a block is disconnected. This side remembers, so
 * that is where resubmission lives.
 */

import { RpcError } from './rpc.js';
import { OPEN_STATUSES } from './store.js';
import { feeRate, feeSnapshot } from './fees.js';

const now = () => Math.floor(Date.now() / 1000);

/* A serialized tx is at most the 1M wu the enforcer accepts, i.e. 1MB. */
export const MAX_TX_HEX_LEN = 2 * 1_000_000;

export class Slipstream {
    constructor({ store, enforcer, bitcoind = null, pool, cfg, log }) {
        this.store = store;
        this.enforcer = enforcer;
        this.bitcoind = bitcoind;
        this.pool = pool;
        this.cfg = cfg;
        this.log = log;
        this.template = null;
        this.templateAt = null;
        this.enforcerError = null;
    }

    fees() {
        return {
            ...feeSnapshot(this.template, this.cfg.minFeeRate),
            updated_at: this.templateAt,
        };
    }

    async refreshTemplate() {
        try {
            this.template = await this.enforcer.getBlockTemplate();
            this.templateAt = now();
            this.enforcerError = null;
        } catch (e) {
            this.enforcerError = e.message;
            throw e;
        }
        return this.template;
    }

    /* Hand one tx to the enforcer and record the outcome. Returns
     * `{ httpStatus, body }`. Every call is logged as a submission, whatever
     * becomes of it. */
    async submit(rawHex, submitter) {
        const hex = typeof rawHex === 'string' ? rawHex.trim() : '';
        const record = (fields) => this.store.logSubmission({ submitter, rawHex: hex, ...fields });
        const refuse = (httpStatus, reason, extra = {}) => {
            record({ txid: extra.txid ?? null, accepted: false, rejectReason: reason });
            return { httpStatus, body: { accepted: false, reject_reason: reason, ...extra } };
        };

        if (hex.length === 0 || hex.length % 2 !== 0 || !/^[0-9a-fA-F]+$/.test(hex)) {
            return refuse(400, 'invalid-hex');
        }
        if (hex.length > MAX_TX_HEX_LEN) return refuse(400, 'tx-size');

        let resp;
        try {
            resp = await this.enforcer.submit(hex);
        } catch (e) {
            if (e instanceof RpcError) {
                if (e.code === -22) return refuse(400, 'tx-decode-failed');
                if (e.code === -32601) return refuse(503, 'slipstream-disabled');
                if (e.code === -10) return refuse(503, 'enforcer-syncing');
            }
            this.log.error(`submitslipstreamtx failed: ${e.message}`);
            return refuse(502, 'enforcer-unavailable');
        }
        const { txid } = resp;
        if (!resp.accepted) {
            return refuse(200, resp.reject_reason ?? 'rejected', { txid });
        }

        const existing = this.store.get(txid);
        if (existing && !['dropped', 'expired'].includes(existing.status)) {
            record({ txid, accepted: true });
            return { httpStatus: 200, body: { accepted: true, already_tracked: true, ...summary(existing) } };
        }

        if (!this.template) {
            try { await this.refreshTemplate(); } catch { /* the floor still applies */ }
        }
        const fees = this.fees();
        const rate = feeRate(resp.fee_sat, resp.vsize);
        if (rate < fees.required_rate) {
            // Already in the enforcer's mempool by now: take it back out, or
            // it is mined for less than was asked.
            try {
                await this.enforcer.remove(txid);
            } catch (e) {
                this.log.error(`removeslipstreamtx ${txid} after a low fee failed: ${e.message}`);
            }
            return refuse(200, 'fee-rate-too-low', {
                txid, fee_rate: rate, required_fee_rate: fees.required_rate,
            });
        }

        const row = {
            txid,
            wtxid: resp.wtxid,
            raw_hex: hex,
            vsize: resp.vsize,
            weight: resp.weight,
            fee_sats: resp.fee_sat,
            fee_rate: rate,
            required_fee_rate: fees.required_rate,
            submitted_height: this.template?.height ?? null,
            submitter: submitter ?? null,
        };
        if (existing) {
            // Dropped or expired before, and valid again now
            this.store.touch(txid, {
                raw_hex: hex, fee_sats: row.fee_sats, fee_rate: rate,
                required_fee_rate: row.required_fee_rate, submitted_at: now(),
                submitted_height: row.submitted_height,
            });
            this.store.setStatus(txid, 'pending', { detail: { resubmitted_by: 'submitter' } });
        } else {
            this.store.insertAccepted(row);
        }
        record({ txid, accepted: true });
        this.log.info(`accepted ${txid} at ${rate} sat/vB (required ${fees.required_rate})`);
        return { httpStatus: 200, body: { accepted: true, ...summary(this.store.get(txid)) } };
    }

    /* One pass over every tx still in play. Errors reaching the enforcer end
     * the pass early; the next one picks up where it stood. */
    async tick() {
        const template = await this.refreshTemplate();
        const inTemplate = new Set((template.transactions ?? []).map(tx => tx.txid));
        for (const row of this.store.byStatus(OPEN_STATUSES)) {
            await this._followOpen(row, template, inTemplate);
        }
        for (const row of this.store.byStatus(['mined'])) {
            await this._followMined(row, template);
        }
    }

    async _followOpen(row, template, inTemplate) {
        const status = await this.enforcer.status(row.txid);
        switch (status.status) {
        case 'pending': {
            if (row.submitted_height !== null
                && template.height - row.submitted_height >= this.cfg.expiryBlocks) {
                await this.enforcer.remove(row.txid);
                this.store.setStatus(row.txid, 'expired', {
                    detail: { submitted_height: row.submitted_height, height: template.height },
                });
                this.log.info(`expired ${row.txid} after ${this.cfg.expiryBlocks} blocks`);
                return;
            }
            const ts = now();
            if (inTemplate.has(row.txid)) {
                this.store.setStatus(row.txid, 'in_template', {
                    fields: row.first_in_template_at === null ? { first_in_template_at: ts } : {},
                });
                this.store.touch(row.txid, { last_in_template_at: ts });
            } else {
                this.store.setStatus(row.txid, 'pending');
            }
            return;
        }
        case 'removed':
            switch (status.reason) {
            case 'mined':
                return this._markMined(row, status.block_hash, template);
            case 'reorged':
                return this._resubmit(row, 'reorged');
            default:
                // conflict_mined, parent_removed, rejected_by_enforcer, or a
                // withdrawal that was not ours
                this.store.setStatus(row.txid, 'dropped', {
                    reason: status.reason,
                    detail: { block_hash: status.block_hash, spent_by: status.spent_by, parent: status.parent },
                });
                this.log.info(`dropped ${row.txid}: ${status.reason}`);
                return;
            }
        case 'unknown': {
            // The enforcer restarted and lost it. It may have been mined
            // while it was down.
            const block = this.bitcoind ? await this.bitcoind.txBlock(row.txid) : null;
            if (block) return this._markMined(row, block, template);
            return this._resubmit(row, 'enforcer-forgot');
        }
        default:
            this.log.warn(`getslipstreamtx ${row.txid}: unexpected status ${JSON.stringify(status)}`);
        }
    }

    async _markMined(row, blockHash, template) {
        let height = null;
        let confirmations = null;
        if (this.bitcoind) {
            const header = await this.bitcoind.blockConfirmations(blockHash);
            if (header) ({ height, confirmations } = header);
        } else if (blockHash === template.previousblockhash) {
            height = template.height - 1;
            confirmations = 1;
        }
        this.store.setStatus(row.txid, 'mined', {
            fields: {
                mined_block_hash: blockHash,
                mined_height: height,
                mined_at: now(),
                mined_by_pool: boolOrNull(this.pool.foundBlock(blockHash)),
                confirmations,
            },
            detail: { block_hash: blockHash, height },
        });
        this.log.info(`mined ${row.txid} in ${blockHash}`);
    }

    async _followMined(row, template) {
        let confirmations;
        if (this.bitcoind) {
            const header = await this.bitcoind.blockConfirmations(row.mined_block_hash);
            if (!header || header.confirmations < 0) {
                // Its block left the main chain. Mined again elsewhere, or
                // waiting to be.
                const block = await this.bitcoind.txBlock(row.txid);
                if (block && block !== row.mined_block_hash) {
                    return this._markMined(row, block, template);
                }
                return this._resubmit(row, 'orphaned');
            }
            confirmations = header.confirmations;
        } else if (row.mined_height !== null) {
            // Without the node an orphan cannot be seen, only depth
            confirmations = template.height - row.mined_height;
        } else {
            return;
        }
        if (confirmations >= this.cfg.confirmations) {
            this.store.setStatus(row.txid, 'confirmed', {
                fields: { confirmations, confirmed_at: now() },
            });
            this.log.info(`confirmed ${row.txid} (${confirmations} deep)`);
        } else {
            this.store.touch(row.txid, { confirmations });
        }
    }

    async _resubmit(row, why) {
        let resp;
        try {
            resp = await this.enforcer.submit(row.raw_hex);
        } catch (e) {
            this.log.warn(`resubmitting ${row.txid} (${why}) failed: ${e.message}`);
            return;
        }
        if (resp.accepted) {
            this.store.touch(row.txid, { resubmissions: row.resubmissions + 1 });
            this.store.setStatus(row.txid, 'pending', {
                fields: { mined_block_hash: null, mined_height: null, mined_at: null,
                          mined_by_pool: null, confirmations: null },
                detail: { resubmitted_after: why },
            });
            this.log.info(`resubmitted ${row.txid} after ${why}`);
        } else {
            this.store.setStatus(row.txid, 'dropped', {
                reason: resp.reject_reason ?? 'rejected',
                detail: { resubmitted_after: why },
            });
            this.log.info(`dropped ${row.txid}: resubmission after ${why} refused (${resp.reject_reason})`);
        }
    }
}

const boolOrNull = (v) => (v === null || v === undefined ? null : v ? 1 : 0);

/* A row as the API shows it: everything but the raw tx. */
export function summary(row) {
    if (!row) return null;
    const { raw_hex: _raw, ...rest } = row;
    return rest;
}
