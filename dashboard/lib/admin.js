// PPS pool operator queries + Thunder reserve balance probe.
//
// All read-only against the same shares.db handle the public dashboard
// uses. Thunder is queried over its JSON-RPC HTTP endpoint (default
// http://127.0.0.1:6009) with a short timeout — we never block a
// request on a slow / down Thunder node.

function unwrap(handle) {
    if (typeof handle?.get === 'function') return handle.get();
    return handle;
}

/* Pool-wide totals across pps_credits. `owed` is the outstanding sat
 * balance the payout worker is responsible for draining. */
export function poolTotals(handle) {
    const db = unwrap(handle);
    if (!db) return { accrued: 0, paid: 0, owed: 0, workers: 0 };
    const r = db.prepare(`
        SELECT COALESCE(SUM(accrued_sats), 0) AS accrued,
               COALESCE(SUM(paid_sats),    0) AS paid,
               COUNT(*)                       AS workers
        FROM   pps_credits
    `).get();
    return {
        accrued: Number(r.accrued),
        paid:    Number(r.paid),
        owed:    Number(r.accrued) - Number(r.paid),
        workers: Number(r.workers),
    };
}

/* Per-worker balances, sorted by outstanding first. `thunder_address`
 * is what the payout worker will target. */
export function perWorkerBalances(handle) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT  w.id                          AS worker_id,
                w.name                        AS worker_name,
                w.payout_address              AS thunder_address,
                c.accrued_sats                AS accrued,
                c.paid_sats                   AS paid,
                (c.accrued_sats - c.paid_sats) AS owed,
                c.last_updated                AS last_updated
        FROM    pps_credits c
        JOIN    workers     w ON w.id = c.worker_id
        ORDER BY owed DESC
    `).all().map(r => ({
        ...r,
        accrued:      Number(r.accrued),
        paid:         Number(r.paid),
        owed:         Number(r.owed),
        last_updated: Number(r.last_updated),
    }));
}

/* Current in-flight payouts. A row with an empty txid means the
 * broadcast may or may not have gone out (worker crashed between
 * INSERT and RPC); a row with a txid means the broadcast succeeded
 * but the finalize transaction crashed. Either state requires
 * operator attention — see payout/README.md for the reconciliation
 * runbook. */
export function inFlight(handle) {
    const db = unwrap(handle);
    if (!db) return [];
    return db.prepare(`
        SELECT  p.id, p.worker_id, w.name AS worker_name,
                w.payout_address AS thunder_address,
                p.sats, p.txid, p.started_at
        FROM    payouts_in_flight p
        JOIN    workers           w ON w.id = p.worker_id
        ORDER BY p.started_at ASC
    `).all().map(r => ({
        ...r,
        sats: Number(r.sats),
        started_at: Number(r.started_at),
    }));
}

/* Minimal Thunder JSON-RPC client — we only need `balance()`. Short
 * timeout so the admin page renders promptly even when Thunder is
 * unreachable. */
export async function thunderBalance(rpcUrl, timeoutMs = 1500) {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), timeoutMs);
    try {
        const res = await fetch(rpcUrl, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'balance', params: [] }),
            signal: ctrl.signal,
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const body = await res.json();
        if (body.error) throw new Error(`${body.error.code} ${body.error.message}`);
        return { ok: true, available_sats: Number(body.result?.available_sats ?? 0),
                 total_sats: Number(body.result?.total_sats ?? 0) };
    } catch (e) {
        return { ok: false, error: e.message, available_sats: 0, total_sats: 0 };
    } finally {
        clearTimeout(t);
    }
}
