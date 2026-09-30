/* Post bodies, as Telegram HTML. Pure: every function takes the dashboard's
 * JSON and returns a string, so the tests pin the wording without a network.
 *
 * The number formatters are copies of the dashboard's (lib/stats.js,
 * lib/fmt.js), not imports: this service ships as its own image and reads
 * the dashboard over HTTP only. */

const HASH_UNITS = ['H/s', 'KH/s', 'MH/s', 'GH/s', 'TH/s', 'PH/s', 'EH/s'];

export function fmtHashrate(hps) {
    if (!hps || !isFinite(hps) || hps <= 0) return '0 H/s';
    let i = 0;
    let v = hps;
    while (v >= 1000 && i < HASH_UNITS.length - 1) {
        v /= 1000;
        i++;
    }
    return `${v.toFixed(2)} ${HASH_UNITS[i]}`;
}

export function fmtBtc(sats) {
    if (sats == null) return '—';
    const v = Number(sats) / 1e8;
    if (!isFinite(v)) return '—';
    return v.toFixed(8).replace(/0+$/, '').replace(/\.$/, '') + ' BTC';
}

export const fmtN = (n) => new Intl.NumberFormat('en-US').format(n || 0);

/* Share difficulty: large and only its magnitude matters. */
export function fmtDiff(d) {
    if (!d || !isFinite(d)) return '0';
    const units = ['', 'K', 'M', 'G', 'T', 'P', 'E'];
    let i = 0;
    let v = d;
    while (v >= 1000 && i < units.length - 1) {
        v /= 1000;
        i++;
    }
    return i === 0 ? v.toFixed(0) : `${v.toFixed(2)}${units[i]}`;
}

export function fmtAgo(sec) {
    if (sec == null || !isFinite(sec)) return '—';
    if (sec < 60)    return `${Math.floor(sec)}s`;
    if (sec < 3600)  return `${Math.floor(sec / 60)}m`;
    if (sec < 86400) return `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`;
    return `${Math.floor(sec / 86400)}d ${Math.floor((sec % 86400) / 3600)}h`;
}

export function esc(s) {
    return String(s ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

function link(publicUrl, path, label) {
    return publicUrl ? `<a href="${esc(publicUrl + path)}">${esc(label)}</a>` : null;
}

function lines(...xs) {
    return xs.filter((x) => x != null && x !== false).join('\n');
}

/* The core stat block shared by the digest and the pinned live message. */
function statLines(p) {
    return [
        `Hashrate: <b>${fmtHashrate(p.hashrate_1h)}</b> (1h) · ${fmtHashrate(p.hashrate)} (24h)`,
        `Active workers (24h): <b>${fmtN(p.workers_active)}</b>`,
        `Shares (24h): ${fmtN(p.accepted)} accepted · ${(p.reject_rate_pct ?? 0).toFixed(2)}% rejected`,
        `Best share (24h): ${fmtDiff(p.best_share_24h)}`,
    ];
}

export function digest({ status, blocks, poolName, publicUrl, nowSec }) {
    const p = status.pool;
    const since = nowSec - 86400;
    const found = (blocks ?? []).filter((b) => b.ts >= since && b.status !== 'rejected');
    const blockLine = found.length === 0
        ? 'Blocks (24h): none'
        : `Blocks (24h): <b>${found.length}</b> — ` +
          found.map((b) => `#${fmtN(b.height)}${b.status === 'orphaned' ? ' (orphaned)' : ''}`).join(', ');
    const fee = p.fee_bps != null ? ` · fee ${(p.fee_bps / 100).toFixed(2)}%` : '';
    const mode = p.mode ? `${esc(p.mode)}${fee}` : null;
    return lines(
        `📊 <b>${esc(poolName)} — daily report</b>`,
        new Date(nowSec * 1000).toISOString().slice(0, 10),
        '',
        ...statLines(p),
        blockLine,
        `Blocks (all time): ${fmtN(p.blocks_lifetime)}`,
        mode && `Mode: ${mode}`,
        status.health && !status.health.ok && status.health.status !== 'checking'
            ? '⚠️ A ledger health check is failing — see the dashboard.'
            : null,
        link(publicUrl, '/', 'Open the dashboard'),
    );
}

export function live({ status, poolName, publicUrl, nowSec, flowing = true }) {
    const p = status.pool;
    const lastShare = p.last_share_ts ? fmtAgo(nowSec - p.last_share_ts) + ' ago' : 'never';
    return lines(
        `${flowing ? '🟢' : '🔴'} <b>${esc(poolName)} — live</b>`,
        '',
        ...statLines(p),
        `Last share: ${lastShare}`,
        `Blocks (all time): ${fmtN(p.blocks_lifetime)}`,
        '',
        `<i>Updated ${new Date(nowSec * 1000).toISOString().slice(11, 16)} UTC</i>`,
        link(publicUrl, '/', 'Open the dashboard'),
    );
}

export function blockFound({ block, poolName, publicUrl }) {
    const reward = block.reward_sats != null ? ` · reward ${fmtBtc(block.reward_sats)}` : '';
    return lines(
        `⛏ <b>${esc(poolName)} found a block!</b>`,
        '',
        `Height <b>${fmtN(block.height)}</b>${reward}`,
        block.finder ? `Found by <code>${esc(block.finder)}</code>` : null,
        `<code>${esc(block.hash)}</code>`,
        block.status === 'pending' ? '<i>Waiting for confirmations.</i>' : null,
        link(publicUrl, '/blocks', 'All blocks'),
    );
}

export function blockLost({ block, poolName }) {
    const what = block.status === 'orphaned'
        ? 'was orphaned: another block won at that height'
        : 'was rejected by the network';
    return lines(
        `⚠️ <b>${esc(poolName)}</b>: block <b>${fmtN(block.height)}</b> ${what}. It will not pay out.`,
        `<code>${esc(block.hash)}</code>`,
    );
}

export function sharesStopped({ poolName, lastShareTs, nowSec }) {
    const since = lastShareTs ? ` for ${fmtAgo(nowSec - lastShareTs)}` : '';
    return `🔴 <b>${esc(poolName)}</b>: no shares accepted${since}. The pool may be down; we're looking into it.`;
}

export function sharesResumed({ poolName, status }) {
    return `🟢 <b>${esc(poolName)}</b>: shares are coming in again ` +
           `(${fmtHashrate(status.pool.hashrate_5m)} over the last 5m).`;
}

export function healthFailing({ poolName, health, publicUrl }) {
    const labels = (health.failing ?? []).map((c) => `• ${esc(c.label ?? c.id)}`);
    return lines(
        `⚠️ <b>${esc(poolName)}</b>: a ledger health check is failing.`,
        ...labels,
        link(publicUrl, '/health', 'Details'),
    );
}

export function healthRecovered({ poolName }) {
    return `✅ <b>${esc(poolName)}</b>: all ledger health checks pass again.`;
}
