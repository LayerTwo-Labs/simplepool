// Stats queries against the simplepool SQLite schema.
//
// All hashrate estimates use:  H/s ≈ sum(difficulty) * 2^32 / windowSec
//
// The handle passed in is the wrapper from lib/db.js — it may not yet be
// connected (proxy hasn't started). In that case we return empty/zero
// shapes so the templates render gracefully.

const TWO_32 = 4294967296;

const EMPTY_OVERVIEW = {
    accepted: 0,
    rejected: 0,
    blocks: 0,
    workers_active: 0,
    hashrate: 0,
    window_sec: 86400,
    db_ready: false,
};

function db(handle) {
    return handle.get();
}

/* Hashrate over an arbitrary window. Uses the standard estimator
 * H/s ≈ sum(difficulty) * 2^32 / windowSec. */
function hashrateOver(d, nowSec, windowSec) {
    const row = d.prepare(
        'SELECT COALESCE(SUM(difficulty),0) AS sum_diff FROM shares WHERE ts >= ?'
    ).get(nowSec - windowSec);
    return (row.sum_diff * TWO_32) / windowSec;
}

export function overview(handle, windowSec = 86400) {
    const d = db(handle);
    if (!d) return { ...EMPTY_OVERVIEW, window_sec: windowSec };
    const nowSec = Math.floor(Date.now() / 1000);
    const since = nowSec - windowSec;

    const acc = d.prepare(
        'SELECT COUNT(*) AS n, COALESCE(SUM(difficulty),0) AS sum_diff, COALESCE(MAX(difficulty),0) AS best FROM shares WHERE ts >= ?'
    ).get(since);
    const rej = d.prepare('SELECT COUNT(*) AS n FROM rejects WHERE ts >= ?').get(since);
    const blk = d.prepare('SELECT COUNT(*) AS n FROM blocks_found WHERE ts >= ?').get(since);
    const wk = d.prepare(
        'SELECT COUNT(DISTINCT worker_id) AS n FROM shares WHERE ts >= ?'
    ).get(since);
    const lifetime = d.prepare(
        'SELECT COUNT(*) AS shares, COALESCE(MAX(difficulty),0) AS best, MIN(ts) AS oldest_ts FROM shares'
    ).get();
    const last = d.prepare('SELECT MAX(ts) AS ts FROM shares').get();
    const totalRej = d.prepare('SELECT COUNT(*) AS n FROM rejects').get();
    const totalBlk = d.prepare('SELECT COUNT(*) AS n FROM blocks_found').get();

    const total24h = acc.n + rej.n;
    const rejectRate24h = total24h > 0 ? (rej.n / total24h) * 100 : 0;

    return {
        accepted: acc.n,
        rejected: rej.n,
        blocks: blk.n,
        workers_active: wk.n,
        hashrate: (acc.sum_diff * TWO_32) / windowSec,   // 24h estimate
        hashrate_1h: hashrateOver(d, nowSec, 3600),
        hashrate_5m: hashrateOver(d, nowSec, 300),
        best_share_24h: acc.best,
        best_share_lifetime: lifetime.best,
        reject_rate_pct: rejectRate24h,
        shares_lifetime: lifetime.shares,
        rejects_lifetime: totalRej.n,
        blocks_lifetime: totalBlk.n,
        oldest_share_ts: lifetime.oldest_ts,
        last_share_ts: last.ts,
        window_sec: windowSec,
        db_ready: true,
    };
}

export function leaderboard(handle, windowSec = 86400, limit = 50) {
    const d = db(handle);
    if (!d) return [];
    const nowSec = Math.floor(Date.now() / 1000);
    const since = nowSec - windowSec;
    const since1h = nowSec - 3600;
    const since5m = nowSec - 300;

    // One pass for the 24h totals, then per-worker short-window sums.
    const rows = d.prepare(`
        SELECT w.id              AS id,
               w.name            AS name,
               w.payout_address  AS payout_address,
               COUNT(s.id)       AS shares,
               MAX(s.ts)         AS last_seen,
               COALESCE(SUM(s.difficulty), 0) AS sum_diff
          FROM shares s
          JOIN workers w ON w.id = s.worker_id
         WHERE s.ts >= ?
         GROUP BY w.id
         ORDER BY sum_diff DESC
         LIMIT ?
    `).all(since, limit);

    const sumShort = d.prepare(
        'SELECT COALESCE(SUM(difficulty),0) AS sd FROM shares WHERE worker_id = ? AND ts >= ?'
    );

    const totalDiff = rows.reduce((a, r) => a + r.sum_diff, 0);
    return rows.map(r => {
        const sd1h = sumShort.get(r.id, since1h).sd;
        const sd5m = sumShort.get(r.id, since5m).sd;
        return {
            name: r.name,
            payout_address: r.payout_address || null,
            shares: r.shares,
            last_seen: r.last_seen,
            hashrate_est: (r.sum_diff * TWO_32) / windowSec,
            hashrate_1h: (sd1h * TWO_32) / 3600,
            hashrate_5m: (sd5m * TWO_32) / 300,
            share_of_pool_pct: totalDiff > 0 ? (r.sum_diff / totalDiff) * 100 : 0,
        };
    });
}

/* Roll up the leaderboard by payout_address. Useful when one miner runs
 * several rigs under the same address (`bc1q….rig1`, `bc1q….rig2`, …):
 * each rig appears as its own row in `leaderboard()`, but here we collapse
 * them into one entry keyed by payout_address and list the rig labels. */
export function leaderboardByAddress(handle, windowSec = 86400, limit = 50) {
    const d = db(handle);
    if (!d) return [];
    const since = Math.floor(Date.now() / 1000) - windowSec;

    // Aggregate shares per (payout_address). Workers with no payout_address
    // recorded (legacy rows) fall back to their `name` as the key so they
    // still show up rather than collapsing into one nameless bucket.
    const rows = d.prepare(`
        SELECT COALESCE(NULLIF(w.payout_address, ''), w.name) AS addr,
               COUNT(s.id)                                    AS shares,
               MAX(s.ts)                                      AS last_seen,
               COALESCE(SUM(s.difficulty), 0)                 AS sum_diff,
               COUNT(DISTINCT w.id)                           AS rigs,
               GROUP_CONCAT(DISTINCT w.name)                  AS worker_names
          FROM shares s
          JOIN workers w ON w.id = s.worker_id
         WHERE s.ts >= ?
         GROUP BY addr
         ORDER BY sum_diff DESC
         LIMIT ?
    `).all(since, limit);

    const totalDiff = rows.reduce((a, r) => a + r.sum_diff, 0);
    return rows.map(r => {
        // Extract just the .label parts so we can show "rig1, rig2, rig3"
        // instead of repeating the address.
        const labels = (r.worker_names || '').split(',').map(n => {
            const dot = n.indexOf('.');
            return dot >= 0 ? n.slice(dot + 1) : '';
        }).filter(Boolean);
        return {
            address: r.addr,
            rigs: r.rigs,
            labels,
            shares: r.shares,
            last_seen: r.last_seen,
            hashrate_est: (r.sum_diff * TWO_32) / windowSec,
            share_of_pool_pct: totalDiff > 0 ? (r.sum_diff / totalDiff) * 100 : 0,
        };
    });
}

export function worker(handle, name, windowSec = 86400) {
    const d = db(handle);
    if (!d) return { worker: null, shares: [], buckets: [] };

    const w = d.prepare('SELECT * FROM workers WHERE name = ?').get(name);
    if (!w) return { worker: null, shares: [], buckets: [] };

    const since = Math.floor(Date.now() / 1000) - windowSec;

    const shares = d.prepare(`
        SELECT ts, difficulty, is_block, block_hash
          FROM shares
         WHERE worker_id = ?
         ORDER BY ts DESC
         LIMIT 200
    `).all(w.id);

    const sumRow = d.prepare(`
        SELECT COALESCE(SUM(difficulty),0) AS sum_diff,
               COUNT(*)                   AS n
          FROM shares
         WHERE worker_id = ? AND ts >= ?
    `).get(w.id, since);

    // 10-minute buckets across the window.
    const bucketSec = 600;
    const rawBuckets = d.prepare(`
        SELECT (ts / ?) * ? AS bucket,
               COALESCE(SUM(difficulty), 0) AS sum_diff,
               COUNT(*)                    AS n
          FROM shares
         WHERE worker_id = ? AND ts >= ?
         GROUP BY bucket
         ORDER BY bucket ASC
    `).all(bucketSec, bucketSec, w.id, since);

    // Densify so the sparkline has every bucket (zero-fill empty ones).
    const nowBucket = Math.floor(Math.floor(Date.now() / 1000) / bucketSec) * bucketSec;
    const startBucket = Math.floor(since / bucketSec) * bucketSec;
    const byTs = new Map(rawBuckets.map(b => [b.bucket, b]));
    const buckets = [];
    for (let t = startBucket; t <= nowBucket; t += bucketSec) {
        const b = byTs.get(t);
        const sd = b ? b.sum_diff : 0;
        buckets.push({
            ts: t,
            shares: b ? b.n : 0,
            hashrate: (sd * TWO_32) / bucketSec,
        });
    }

    return {
        worker: {
            name: w.name,
            payout_address: w.payout_address || null,
            first_seen: w.first_seen,
            last_seen: w.last_seen,
            window_shares: sumRow.n,
            window_hashrate: (sumRow.sum_diff * TWO_32) / windowSec,
        },
        shares,
        buckets,
        window_sec: windowSec,
    };
}

export function recentBlocks(handle, limit = 25) {
    const d = db(handle);
    if (!d) return [];
    return d.prepare(`
        SELECT b.ts,
               b.height,
               b.hash,
               b.finder_address,
               b.reward_sats,
               b.fee_sats,
               w.name AS finder
          FROM blocks_found b
          LEFT JOIN workers w ON w.id = b.finder_id
         ORDER BY b.ts DESC
         LIMIT ?
    `).all(limit);
}

/* Paginated full history. `beforeTs` is the exclusive upper bound used
 * for the "older" link; pass null/undefined for the first page. */
export function allBlocks(handle, { limit = 50, beforeTs = null } = {}) {
    const d = db(handle);
    if (!d) return { rows: [], next_before: null };
    const cap = Math.min(Math.max(Number(limit) || 50, 1), 200);
    let rows;
    if (beforeTs == null) {
        rows = d.prepare(`
            SELECT b.ts, b.height, b.hash, b.finder_address,
                   b.reward_sats, b.fee_sats, w.name AS finder
              FROM blocks_found b
              LEFT JOIN workers w ON w.id = b.finder_id
             ORDER BY b.ts DESC
             LIMIT ?
        `).all(cap);
    } else {
        rows = d.prepare(`
            SELECT b.ts, b.height, b.hash, b.finder_address,
                   b.reward_sats, b.fee_sats, w.name AS finder
              FROM blocks_found b
              LEFT JOIN workers w ON w.id = b.finder_id
             WHERE b.ts < ?
             ORDER BY b.ts DESC
             LIMIT ?
        `).all(Number(beforeTs), cap);
    }
    const next_before = rows.length === cap ? rows[rows.length - 1].ts : null;
    return { rows, next_before };
}

const SATS_PER_BTC = 100000000;
export function fmtBtc(sats) {
    if (sats == null) return '—';
    const v = Number(sats) / SATS_PER_BTC;
    if (!isFinite(v)) return '—';
    return v.toFixed(8).replace(/0+$/, '').replace(/\.$/, '') + ' BTC';
}

const UNITS = ['H/s', 'KH/s', 'MH/s', 'GH/s', 'TH/s', 'PH/s', 'EH/s'];
export function fmtHashrate(hps) {
    if (!hps || !isFinite(hps) || hps <= 0) return '0 H/s';
    let i = 0;
    let v = hps;
    while (v >= 1000 && i < UNITS.length - 1) {
        v /= 1000;
        i++;
    }
    return `${v.toFixed(2)} ${UNITS[i]}`;
}
