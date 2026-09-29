/* The slipstream service, as the dashboard shows it.
 *
 * Read through the service's own API rather than its database: the service
 * owns slipstream.db, and a second reader of its schema is a second place to
 * keep in step with it. Every call degrades to { ok: false, error } so a
 * service that is down costs the page one card, not the page.
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
