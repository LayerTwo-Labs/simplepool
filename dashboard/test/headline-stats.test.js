/* The "Pool at a glance" card on the overview.
 *
 * Most tiles restate numbers computed elsewhere; what is new and worth
 * pinning down is:
 *
 *   - Network hashrate measured from block arrivals in the templates table,
 *     which has to survive a reorg, a gap while the proxy was down, and a
 *     pool that has only been up for part of the window.
 *   - The next retarget height, which regtest never reaches.
 *   - Active users and coins mined, which must use the same keys and the
 *     same "confirmed only" rule as the rest of the page.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { all as fmtAll } from '../lib/fmt.js';
import * as stats from '../lib/stats.js';
import { openDb } from '../lib/db.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');
const TWO_32 = 4294967296;
const DIFF   = 1e10;

function makeDb(seed) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-headline-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    seed(db, Math.floor(Date.now() / 1000));
    db.close();
    return openDb(file);
}

/* One template row per (ts, height), as the proxy writes them. */
function addTemplates(db, rows) {
    const ins = db.prepare(`
        INSERT INTO templates (ts, height, prev_hash, bits, network_difficulty,
                               coinbase_value_sats, tx_count, tx_fees_sats, source,
                               cb_spendable, cb_op_returns, longpoll, rate_sats_per_diff)
        VALUES (?, ?, '00', '1d00ffff', ?, 0, 0, 0, 'enforcer', 0, 0, 0, 0)`);
    for (const [ts, height, diff = DIFF] of rows) ins.run(ts, height, diff);
}

test('network hashrate is the work of the blocks that arrived over the span', () => {
    /* A block every 10 min for 10 h, plus a second template at each height
     * (a tx refresh) that must not count as a block. */
    const db = makeDb((d, now) => {
        const rows = [];
        const start = now - 36000;
        for (let i = 0; i <= 60; i++) {
            rows.push([start + i * 600, 1000 + i]);
            rows.push([start + i * 600 + 30, 1000 + i]);
        }
        addTemplates(d, rows);
    });
    const net = stats.networkHashrate(db);
    assert.equal(net.blocks, 60);
    /* Span is from the first row to now, not the nominal 24h. */
    assert.ok(Math.abs(net.span_sec - 36000) <= 2);
    const expected = (60 * DIFF * TWO_32) / net.span_sec;
    assert.ok(Math.abs(net.hashrate - expected) / expected < 1e-9);
});

test('a reorg is not counted twice, and a gap counts every skipped height', () => {
    const db = makeDb((d, now) => {
        const t = now - 7200;
        addTemplates(d, [
            [t,        100],
            [t + 600,  101],
            [t + 900,  101],   // competing tip at the same height
            [t + 1200, 102],
            [t + 1300, 101],   // reorg back...
            [t + 1400, 102],   // ...and forward again: not a new block
            [t + 5400, 107],   // proxy was down for 5 blocks
            [t + 6000, 108],
        ]);
    });
    assert.equal(stats.networkHashrate(db).blocks, 8);
});

test('too few blocks is no measurement, not a wild one', () => {
    const db = makeDb((d, now) => addTemplates(d, [[now - 1200, 10], [now - 600, 11], [now - 1, 12]]));
    assert.equal(stats.networkHashrate(db), null);
});

test('fmtBtc labels amounts with the chain\'s coin', () => {
    assert.equal(stats.fmtBtc(1148800000000), '11488 BTC');
    assert.equal(stats.fmtBtc(1148800000000, 'pECX'), '11488 pECX');
    assert.equal(stats.fmtBtc(null, 'pECX'), '—');
});

test('next retarget is the next multiple of 2016, never on regtest', () => {
    assert.deepEqual(stats.nextRetarget(973730, 'forknet'), { height: 975744, blocks_away: 2014 });
    /* On a boundary the retarget has just happened; the next one is 2016 on. */
    assert.deepEqual(stats.nextRetarget(975744, 'main'), { height: 977760, blocks_away: 2016 });
    assert.equal(stats.nextRetarget(973730, 'regtest'), null);
    assert.equal(stats.nextRetarget(null, 'main'), null);
});

function seedPool(d, now) {
    const w = d.prepare('INSERT INTO workers (name, first_seen, last_seen, payout_address) VALUES (?, ?, ?, ?)');
    w.run('alice.rig1', now, now, 'addrA');
    w.run('alice.rig2', now, now, 'addrA');
    w.run('bob.rig1',   now, now, 'addrB');
    w.run('old.rig',    now - 900000, now - 900000, 'addrC');
    const s = d.prepare('INSERT INTO shares (ts, worker_id, difficulty) VALUES (?, ?, ?)');
    s.run(now - 60, 1, 1000); s.run(now - 60, 2, 1000); s.run(now - 60, 3, 1000);
    s.run(now - 900000, 4, 1000);
    /* bob.rig2 hashed this morning and stopped: active today, not hashing. */
    w.run('bob.rig2', now, now - 7200, 'addrB');
    s.run(now - 7200, 5, 1000);
    const b = d.prepare(`INSERT INTO blocks_found (ts, height, hash, reward_sats, fee_sats, status)
                         VALUES (?, ?, ?, ?, ?, ?)`);
    b.run(now - 100, 500, 'h1', 312500000, 3125000, 'confirmed');
    b.run(now - 200, 499, 'h2', 312500000, 3125000, 'confirmed');
    b.run(now - 300, 498, 'h3', 312500000, 3125000, 'pending');
    b.run(now - 400, 497, 'h4', 312500000, 3125000, 'orphaned');
}

test('active users are payout addresses, coins mined are confirmed and net of fee', () => {
    const ov = stats.overview(makeDb(seedPool));
    assert.equal(ov.users_active, 2, 'two addresses in 24h, the third is stale');
    assert.equal(ov.workers_active, 4);
    assert.equal(ov.workers_hashing, 3, 'only rigs with a share in the last 15 min');
    assert.equal(ov.mined_sats_lifetime, 625000000, 'pending and orphaned mined nothing');
});

test('the overview renders the card, and survives an empty pool', async () => {
    const render = (db, extra = {}) => ejs.renderFile(path.join(VIEWS, 'index.ejs'), {
        ...fmtAll, pool: null, stratumUrl: 'stratum+tcp://x:3334', sidechainId: 9,
        health: null, active: 'overview',
        ov:     stats.overview(db),
        lb:     stats.leaderboard(db),
        lbAddr: stats.leaderboardByAddress(db),
        blocks: stats.recentBlocks(db, 5),
        node:   stats.nodeStatus(db),
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
        ...extra,
    }, { views: [VIEWS] });

    const db = makeDb(seedPool);
    const html = await render(db, {
        net: { hashrate: 1.15e17, blocks: 850, span_sec: 86400, window_sec: 86400 },
        retarget: { height: 975744, blocks_away: 2014 },
    });
    for (const label of ['Network hashrate', 'Pool share (24h)', 'Network difficulty',
                         'Block height', 'Next retarget', 'Pool hashrate (1h)',
                         'Blocks mined (24h)', 'Coins mined', 'Active users', 'Fee']) {
        assert.ok(html.includes(label), `missing tile: ${label}`);
    }
    assert.match(html, /115\.00 PH\/s/);
    assert.match(html, /24h · from 850 block times/);
    assert.match(html, /2,014 blocks away/);
    assert.match(html, /6\.25 BTC/);
    assert.match(html, /3 workers hashing/);

    /* The unit follows COIN_SYMBOL, which server.js binds into fmtBtc. */
    const ecx = await render(db, { fmtBtc: (sats) => stats.fmtBtc(sats, 'pECX') });
    assert.match(ecx, /6\.25 pECX/);
    assert.doesNotMatch(ecx, /6\.25 BTC/);

    /* No published difficulty, no block arrivals: dashes, not zeros. */
    const empty = await render(makeDb(() => {}));
    assert.match(empty, /Pool at a glance/);
    assert.match(empty, /no data yet/);
});
