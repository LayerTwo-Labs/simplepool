/* Formatting helpers shared across views. server.js drops them onto
 * res.locals so every EJS render sees them without a per-view
 * <% const fmt... %> preamble. */

export const fmtN = (n) =>
    new Intl.NumberFormat('en-US').format(n || 0);

export const fmtF = (n, d = 4) =>
    (typeof n === 'number' ? n.toFixed(d) : '—');

export const fmtTs = (t) =>
    t ? new Date(t * 1000).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : '—';

export const ago = (t) => {
    if (!t) return '—';
    const s = Math.max(0, Math.floor(Date.now() / 1000) - t);
    if (s < 60)    return s + 's ago';
    if (s < 3600)  return Math.floor(s / 60)    + 'm ago';
    if (s < 86400) return Math.floor(s / 3600)  + 'h ago';
    return Math.floor(s / 86400) + 'd ago';
};

export const fmtSats = (sats) => {
    if (!sats) return '0 sats';
    const btc = sats / 1e8;
    if (btc >= 0.01) return fmtN(sats) + ' sats (' + btc.toFixed(8) + ' BTC)';
    return fmtN(sats) + ' sats';
};

/* Adaptive-precision percent — keeps small miners visible instead of
 * rendering them as 0.00%. Used by the public overview. */
export const fmtPct = (p) => {
    if (p == null || !isFinite(p)) return '—';
    if (p === 0) return '0%';
    const abs = Math.abs(p);
    if (abs >= 1)     return p.toFixed(2)  + '%';
    if (abs >= 0.01)  return p.toFixed(3)  + '%';
    if (abs >= 0.0001) return p.toFixed(4) + '%';
    return p.toPrecision(2) + '%';
};

/* Convenience bundle for res.locals middleware. */
export const all = { fmtN, fmtF, fmtTs, ago, fmtSats, fmtPct };
