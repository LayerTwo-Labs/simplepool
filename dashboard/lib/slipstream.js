/* The slipstream service, as the dashboard shows it.
 *
 * Read through the service's own API rather than its database: the service
 * owns slipstream.db, and a second reader of its schema is a second place to
 * keep in step with it. Every call degrades to { ok: false, error } so a
 * service that is down costs the page one card, not the page.
 *
 * Withdrawing goes to the enforcer, which holds the tx, not to the service,
 * which has no authenticated surface to take it from. The service sees the
 * tx leave on its next poll and records it as dropped (withdrawn).
 */

const TIMEOUT_MS = 3000;

async function getJson(url) {
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), TIMEOUT_MS);
    try {
        const res = await fetch(url, { signal: ctrl.signal });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return await res.json();
    } finally {
        clearTimeout(t);
    }
}

/* Fees and recent txs. `statuses` narrows the list, e.g. to what is still
 * open for the admin page. */
export async function fetchSlipstream(apiUrl, { limit = 50, statuses = null } = {}) {
    if (!apiUrl) return { configured: false, ok: false, fees: null, txs: [] };
    const base = apiUrl.replace(/\/+$/, '');
    try {
        const lists = statuses
            ? statuses.map(s => getJson(`${base}/api/txs?status=${encodeURIComponent(s)}&limit=${limit}`))
            : [getJson(`${base}/api/txs?limit=${limit}`)];
        const [fees, ...pages] = await Promise.all([getJson(`${base}/api/fees`), ...lists]);
        const txs = pages.flatMap(p => p.txs ?? [])
            .sort((a, b) => b.submitted_at - a.submitted_at);
        return { configured: true, ok: true, fees, txs };
    } catch (e) {
        return { configured: true, ok: false, fees: null, txs: [], error: e.message };
    }
}

/* { ok, msg, detail } like every admin action. Never throws. */
export async function withdrawSlipstreamTx({ enforcerGbtUrl, txid }) {
    if (!enforcerGbtUrl) {
        return { ok: false, msg: 'withdraw unavailable', detail: 'ENFORCER_GBT_URL is not set' };
    }
    if (!/^[0-9a-fA-F]{64}$/.test(txid || '')) {
        return { ok: false, msg: 'withdraw refused', detail: 'txid must be 64 hex chars' };
    }
    const ctrl = new AbortController();
    const t = setTimeout(() => ctrl.abort(), TIMEOUT_MS);
    try {
        const res = await fetch(enforcerGbtUrl, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'removeslipstreamtx',
                                   params: [txid.toLowerCase()] }),
            signal: ctrl.signal,
        });
        const body = await res.json();
        if (body.error) {
            return { ok: false, msg: 'withdraw failed',
                     detail: `${body.error.code} ${body.error.message}` };
        }
        const removed = body.result ?? [];
        return removed.length > 0
            ? { ok: true, msg: `withdrew ${removed.length} tx(s)`, detail: removed.join(', ') }
            : { ok: false, msg: 'nothing withdrawn',
                detail: 'the enforcer is not holding that tx (already mined or gone), '
                      + 'or the node holds its own copy and it stays in the mempool' };
    } catch (e) {
        return { ok: false, msg: 'withdraw failed', detail: e.message };
    } finally {
        clearTimeout(t);
    }
}
