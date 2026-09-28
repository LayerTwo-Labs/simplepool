/* Submission and tracking.
 *
 * A submission goes to the pool's own bitcoind. The enforcer's template
 * mempool mirrors that node's mempool, so a tx the node accepts reaches the
 * templates the proxy mines with no further step. The node needs
 * `-acceptnonstdtxn` for it to take non-standard txs; BIP300 deposits are
 * standard on a drivechain-patched node already.
 *
 * Nothing can be taken back out of a node's mempool, so the fee rule is
 * applied BEFORE anything is broadcast: `testmempoolaccept` reports the fee
 * and size without broadcasting, and only a tx that passes both the node and
 * the rule is sent. Once sent it is relayed like any other.
 *
 * After that, every poll reads the template the proxy is mining and asks the
 * node where each open tx stands, and moves the row accordingly.
 */

import { RpcError } from './rpc.js';
import { OPEN_STATUSES } from './store.js';
import { feeRate, feeSnapshot } from './fees.js';

const now = () => Math.floor(Date.now() / 1000);
const btcToSats = (btc) => Math.round(btc * 1e8);

/* A serialized tx is at most a block, 4M wu, i.e. 4MB; nothing near that is
 * useful, and a POST body has to be bounded somewhere. 1MB of tx. */
export const MAX_TX_HEX_LEN = 2 * 1_000_000;

/* testmempoolaccept's reasons for a tx the node already has */
const ALREADY_KNOWN = ['txn-already-in-mempool', 'txn-already-known'];

export class Slipstream {
    constructor({ store, bitcoind, enforcer, pool, cfg, log }) {
        this.store = store;
        this.bitcoind = bitcoind;
        this.enforcer = enforcer;
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

    /* Check one tx, and broadcast it if it passes. Returns
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

        let check;
        try {
            check = await this.bitcoind.testAccept(hex);
        } catch (e) {
            if (e instanceof RpcError && e.code === -22) return refuse(400, 'tx-decode-failed');
            this.log.error(`testmempoolaccept failed: ${e.message}`);
            return refuse(502, 'node-unavailable');
        }
        const { txid } = check;

        const existing = this.store.get(txid);
        if (existing && !['dropped'].includes(existing.status)) {
            record({ txid, accepted: true });
            return { httpStatus: 200, body: { accepted: true, already_tracked: true, ...summary(existing) } };
        }

        // Fee and size: from the check, or from the mempool for a tx the node
        // already has, which is then taken on and followed like any other.
        let feeSats;
        let vsize;
        let alreadyInMempool = false;
        if (check.allowed) {
            feeSats = btcToSats(check.fees.base);
            vsize = check.vsize;
        } else if (ALREADY_KNOWN.includes(check['reject-reason'])) {
            const entry = await this.bitcoind.mempoolEntry(txid);
            if (!entry) return refuse(200, 'already-confirmed', { txid });
            feeSats = btcToSats(entry.fees.base);
            vsize = entry.vsize;
            alreadyInMempool = true;
        } else {
            return refuse(200, check['reject-reason'] ?? 'rejected', { txid });
        }

        if (!this.template) {
            try { await this.refreshTemplate(); } catch { /* the floor still applies */ }
        }
        const fees = this.fees();
        const rate = feeRate(feeSats, vsize);
        if (rate < fees.required_rate) {
            return refuse(200, 'fee-rate-too-low', {
                txid, fee_rate: rate, required_fee_rate: fees.required_rate,
            });
        }

        if (!alreadyInMempool) {
            try {
                await this.bitcoind.send(hex);
            } catch (e) {
                // Lost a race with something the check did not see, e.g. a
                // conflicting tx arriving in between.
                if (e instanceof RpcError) return refuse(200, e.rpcMessage, { txid });
                this.log.error(`sendrawtransaction ${txid} failed: ${e.message}`);
                return refuse(502, 'node-unavailable', { txid });
            }
        }
        const entry = await this.bitcoind.mempoolEntry(txid);
        const row = {
            txid,
            wtxid: check.wtxid,
            raw_hex: hex,
            vsize,
            weight: entry?.weight ?? vsize * 4,
            fee_sats: feeSats,
            fee_rate: rate,
            required_fee_rate: fees.required_rate,
            submitted_height: this.template?.height ?? null,
            submitter: submitter ?? null,
        };
        if (existing) {
            // Dropped before, and valid again now
            this.store.touch(txid, {
                raw_hex: hex, fee_sats: feeSats, fee_rate: rate,
                required_fee_rate: row.required_fee_rate, submitted_at: now(),
                submitted_height: row.submitted_height,
            });
            this.store.setStatus(txid, 'pending', { detail: { resubmitted_by: 'submitter' } });
        } else {
            this.store.insertAccepted(row);
        }
        record({ txid, accepted: true });
        this.log.info(`accepted ${txid} at ${rate} sat/vB (required ${fees.required_rate})`
                      + (alreadyInMempool ? ', already in the mempool' : ''));
        return { httpStatus: 200, body: { accepted: true, ...summary(this.store.get(txid)) } };
    }

    /* One pass over every tx still in play. An unreachable enforcer ends the
     * pass before anything moves; the next one picks up where it stood. */
    async tick() {
        const template = await this.refreshTemplate();
        const inTemplate = new Set((template.transactions ?? []).map(tx => tx.txid));
        for (const row of this.store.byStatus(OPEN_STATUSES)) {
            await this._followOpen(row, inTemplate);
        }
        for (const row of this.store.byStatus(['mined'])) {
            await this._followMined(row);
        }
    }

    async _followOpen(row, inTemplate) {
        if (await this.bitcoind.mempoolEntry(row.txid)) {
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
        const block = await this.bitcoind.txBlock(row.txid);
        if (block) return this._markMined(row, block);
        // Out of the mempool and not mined: evicted, expired, replaced, or its
        // input spent by a block. Sending it again tells which.
        return this._rebroadcast(row, 'left-mempool');
    }

    async _markMined(row, blockHash) {
        const header = await this.bitcoind.blockConfirmations(blockHash);
        this.store.setStatus(row.txid, 'mined', {
            fields: {
                mined_block_hash: blockHash,
                mined_height: header?.height ?? null,
                mined_at: now(),
                mined_by_pool: boolOrNull(this.pool.foundBlock(blockHash)),
                confirmations: header?.confirmations ?? null,
            },
            detail: { block_hash: blockHash, height: header?.height ?? null },
        });
        this.log.info(`mined ${row.txid} in ${blockHash}`);
    }

    async _followMined(row) {
        const header = await this.bitcoind.blockConfirmations(row.mined_block_hash);
        if (!header || header.confirmations < 0) {
            // Its block left the main chain. The node puts a disconnected
            // block's txs back in its mempool when it can, so it may already
            // be waiting again, or mined in the block that replaced it.
            const block = await this.bitcoind.txBlock(row.txid);
            if (block && block !== row.mined_block_hash) return this._markMined(row, block);
            if (await this.bitcoind.mempoolEntry(row.txid)) {
                return this._backToPending(row, 'orphaned');
            }
            return this._rebroadcast(row, 'orphaned');
        }
        if (header.confirmations >= this.cfg.confirmations) {
            this.store.setStatus(row.txid, 'confirmed', {
                fields: { confirmations: header.confirmations, confirmed_at: now() },
            });
            this.log.info(`confirmed ${row.txid} (${header.confirmations} deep)`);
        } else {
            this.store.touch(row.txid, { confirmations: header.confirmations });
        }
    }

    _backToPending(row, why) {
        this.store.setStatus(row.txid, 'pending', {
            fields: { mined_block_hash: null, mined_height: null, mined_at: null,
                      mined_by_pool: null, confirmations: null },
            detail: { after: why },
        });
        this.log.info(`${row.txid} back in the mempool after ${why}`);
    }

    async _rebroadcast(row, why) {
        try {
            await this.bitcoind.send(row.raw_hex);
        } catch (e) {
            if (!(e instanceof RpcError)) {
                this.log.warn(`rebroadcasting ${row.txid} (${why}) failed: ${e.message}`);
                return;
            }
            // -27: "Transaction already in block chain" -- mined, and the
            // lookup above raced the block. The next poll finds its block.
            if (e.code === -27) return;
            this.store.setStatus(row.txid, 'dropped', {
                reason: e.rpcMessage,
                detail: { after: why },
            });
            this.log.info(`dropped ${row.txid}: the node refused it again after ${why} (${e.rpcMessage})`);
            return;
        }
        this.store.touch(row.txid, { resubmissions: row.resubmissions + 1 });
        this.store.event(row.txid, 'rebroadcast', { after: why });
        this._backToPending(row, why);
    }
}

const boolOrNull = (v) => (v === null || v === undefined ? null : v ? 1 : 0);

/* A row as the API shows it: everything but the raw tx. */
export function summary(row) {
    if (!row) return null;
    const { raw_hex: _raw, ...rest } = row;
    return rest;
}
