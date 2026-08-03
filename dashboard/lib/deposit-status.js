/* Where is a deposit, actually?
 *
 * "Did the deposit go through" has four different answers, and the operator
 * needs to know which one is currently true — a deposit can be irreversibly
 * confirmed on the mainchain and still be worth nothing on Thunder:
 *
 *   1. broadcast   the enforcer handed the tx to bitcoind
 *   2. confirmed   it is in a mainchain block (with N confirmations)
 *   3. ctip        the sidechain's Ctip points at it, so BIP300 has
 *                  recognised it as a deposit rather than an ordinary spend
 *   4. credited    Thunder has processed it into a spendable UTXO the
 *                  payout worker can draw from
 *
 * Stage 4 is the one that surprises people. Thunder only ingests a deposit
 * when its own chain advances, so if nobody is BMM-mining the sidechain the
 * funds sit at stage 3 indefinitely — confirmed, irreversible, and unusable.
 * Reporting "confirmed" alone would be actively misleading there.
 *
 * Everything here is read-only and best-effort: any probe may fail without
 * taking the page down with it, because a status panel that 500s when the
 * enforcer hiccups is worse than one that says "unknown".
 */

import { enforcerRpc } from './enforcer.js';

const TXID_RE = /^[0-9a-fA-F]{64}$/;

/* Thunder credits the reserve in aggregate, not per deposit, so stage 4 can
 * only be answered for the reserve as a whole. Kept separate from the
 * per-deposit stages so the view never implies we matched a specific tx. */
async function thunderReserve(thunderRpcUrl) {
    if (!thunderRpcUrl) return { ok: false, error: 'THUNDER_RPC_URL not configured' };
    const ctl = new AbortController();
    const t   = setTimeout(() => ctl.abort(), 10_000);
    try {
        const r = await fetch(thunderRpcUrl, {
            method:  'POST',
            headers: { 'content-type': 'application/json' },
            body:    JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'balance', params: [] }),
            signal:  ctl.signal,
        });
        const j = await r.json();
        if (j.error) throw new Error(j.error.message || 'thunder error');
        return {
            ok:        true,
            total:     Number(j.result?.total_sats     ?? 0),
            available: Number(j.result?.available_sats ?? 0),
        };
    } catch (e) {
        return { ok: false, error: e.message };
    } finally {
        clearTimeout(t);
    }
}

/* Resolve every recorded deposit against the enforcer's own view of the
 * chain. `deposits` is the rows from the deposits table.
 *
 * Returns { rows, tipHeight, ctipTxid, reserve, errors } — `rows` in the same
 * order it was given, each annotated with a `status` object. Never throws. */
export async function depositStatuses(deposits, {
    enforcerGrpcAddr, thunderRpcUrl, sidechainId,
} = {}) {
    const errors = [];

    /* One list call covers every deposit; per-txid lookups would be N round
     * trips for a page that renders on every 30s refresh. */
    const [listed, tip, ctip, reserve] = await Promise.all([
        enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.WalletService/ListSidechainDepositTransactions', {}, 15_000)
            .catch(e => { errors.push(`deposit list: ${e.message}`); return null; }),
        enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.ValidatorService/GetChainTip', {}, 10_000)
            .catch(e => { errors.push(`chain tip: ${e.message}`); return null; }),
        enforcerRpc(enforcerGrpcAddr,
            'cusf.mainchain.v1.ValidatorService/GetCtip',
            { sidechain_number: Number(sidechainId) }, 10_000)
            .catch(e => { errors.push(`ctip: ${e.message}`); return null; }),
        thunderReserve(thunderRpcUrl),
    ]);

    const tipHeight = Number(tip?.blockHeaderInfo?.height ?? tip?.block_header_info?.height ?? 0);
    const ctipTxid  = ctip?.ctip?.txid?.hex ?? null;

    /* txid -> enforcer record. Field names arrive in either camelCase or
     * snake_case depending on enforcer build, so read both. */
    const byTxid = new Map();
    for (const row of listed?.transactions || []) {
        const tx = row?.tx || row?.transaction;
        const id = tx?.txid?.hex ?? (typeof tx?.txid === 'string' ? tx.txid : null);
        if (id) byTxid.set(id.toLowerCase(), { row, tx });
    }

    const rows = (deposits || []).map(d => {
        const txid = String(d.btc_txid || '').toLowerCase();
        const known = TXID_RE.test(txid) ? byTxid.get(txid) : null;

        /* A row whose txid was never filled in is a deposit that failed
         * before the enforcer returned one — the attempt log has the error. */
        if (!TXID_RE.test(txid)) {
            return { ...d, status: {
                stage: 'no-txid', label: 'no txid recorded', ok: false,
                detail: 'The deposit failed before a txid existed — see the broadcast attempts below.',
            } };
        }

        const conf   = known?.tx?.confirmationInfo || known?.tx?.confirmation_info || null;
        const height = Number(conf?.height ?? 0);
        const confirmations = (height > 0 && tipHeight > 0)
            ? Math.max(0, tipHeight - height + 1) : 0;
        const isCtip = !!(ctipTxid && ctipTxid.toLowerCase() === txid);

        if (!known) {
            /* Recorded locally but the enforcer has never heard of it. Not
             * proof of failure — a pruned or re-created wallet loses this
             * history — so say what we know, not what we guess. */
            return { ...d, status: {
                stage: 'unknown', label: 'not in enforcer wallet', ok: false,
                confirmations: 0, height: 0, isCtip,
                detail: 'The enforcer has no record of this txid. It may predate the current wallet.',
            } };
        }
        if (!conf) {
            return { ...d, status: {
                stage: 'broadcast', label: 'in mempool, unconfirmed', ok: false,
                confirmations: 0, height: 0, isCtip,
                detail: 'Broadcast but not yet mined into a mainchain block.',
            } };
        }
        if (!isCtip) {
            /* Confirmed, but the Ctip has moved past it. That is the normal
             * steady state once a LATER deposit supersedes this one — the
             * Ctip only ever names the most recent. Not an error. */
            return { ...d, status: {
                stage: 'confirmed', label: `confirmed (${confirmations} conf)`, ok: true,
                confirmations, height, isCtip: false,
                blockHash: conf?.blockHash?.hex ?? conf?.block_hash?.hex ?? null,
                detail: 'Confirmed on the mainchain. The Ctip has since moved to a later deposit.',
            } };
        }
        return { ...d, status: {
            stage: 'ctip', label: `confirmed (${confirmations} conf) · current Ctip`, ok: true,
            confirmations, height, isCtip: true,
            blockHash: conf?.blockHash?.hex ?? conf?.block_hash?.hex ?? null,
            detail: 'Confirmed and recognised by BIP300 — this tx is the sidechain\'s current Ctip.',
        } };
    });

    return { rows, tipHeight, ctipTxid, reserve, errors };
}

/* One-line summary for the flash message after an explicit re-check.
 * Deliberately calls out the confirmed-but-not-credited case, because that
 * is the state an operator is most likely to misread as "done". */
export function summarise({ rows, reserve }) {
    const n = rows.length;
    if (n === 0) return 'No deposits recorded yet.';
    const settled = rows.filter(r => r.status.ok).length;
    const pending = n - settled;
    const parts = [`${settled}/${n} confirmed on the mainchain`];
    if (pending > 0) parts.push(`${pending} not confirmed`);
    if (reserve?.ok) {
        parts.push(`Thunder reserve ${reserve.total} sats`);
        if (settled > 0 && reserve.total === 0) {
            parts.push('— confirmed on the mainchain but NOT yet credited on Thunder ' +
                       '(the sidechain must advance a block to ingest it)');
        }
    } else if (reserve?.error) {
        parts.push(`Thunder unreachable: ${reserve.error}`);
    }
    return parts.join(', ');
}
