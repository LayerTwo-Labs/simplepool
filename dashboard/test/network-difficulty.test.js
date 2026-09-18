/* Network difficulty on the node-tip card.
 *
 * The card answered "what height is the chain at" but never "how hard is it",
 * which is the figure that explains every other number on the page: what a
 * share is worth under PPS, how often this pool should expect a block, and
 * whether a listener's promised min_diff is above the chain at all.
 *
 * Two things are worth pinning down, and both have bitten this dashboard in
 * other forms already:
 *
 *   - The number is NOT in node_status. That table is the bitcoind tip poll
 *     and carries no target. It comes from pool_meta, which the proxy
 *     rewrites off each block template — the same row the PPS rate reads, so
 *     there is one difficulty here and not two that can disagree.
 *   - It has to survive the range chains actually run at. Mainnet is ~1.3e14
 *     and a forknet sits at 4.66e-10; the same %.2f that reads fine for one
 *     renders the other as "0.00", which is how src/config.c came to use %g
 *     for exactly this number.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { fmtDifficulty, all as fmtAll } from '../lib/fmt.js';
import * as stats from '../lib/stats.js';
import { openDb } from '../lib/db.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const MAINNET_DIFF = 126980000000000;
const FORKNET_DIFF = 4.66e-10;

/* A DB with a tip, and optionally a published difficulty. `diff = null`
 * writes no pool_meta row at all, which is what a DB looks like before the
 * proxy has ever built a job. */
function makeDb(diff = MAINNET_DIFF) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-netdiff-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    const now = Math.floor(Date.now() / 1000);
    db.prepare(`INSERT INTO node_status (id, tip_height, tip_hash, tip_observed_at, updated_at)
                VALUES (1, 914203, ?, ?, ?)`)
      .run('00'.repeat(32), now - 240, now - 5);
    if (diff !== null) {
        db.prepare(`INSERT INTO pool_meta (id, pool_mode, fee_bps, rate_source,
                                           network_difficulty, updated_at)
                    VALUES (1, 'pps-classic', 100, 'derived', ?, ?)`)
          .run(diff, now - 9);
    }
    db.close();
    return openDb(file);
}

test('a mainnet difficulty is readable, not fifteen digits', () => {
    assert.equal(fmtDifficulty(MAINNET_DIFF), '126.98 T');
    assert.equal(fmtDifficulty(3.5e6), '3.50 M');
    assert.equal(fmtDifficulty(111157.455), '111.16 k');
});

test('a forknet difficulty survives, where toFixed would read 0.00', () => {
    /* The whole reason this is not (n).toFixed(2). A chain ten orders of
     * magnitude under difficulty 1 is a chain simplepool is run against. */
    const out = fmtDifficulty(FORKNET_DIFF);
    assert.equal(out, '4.66e-10');
    assert.notEqual(out, '0.00');
    assert.ok(Number(out) > 0, 'still parses back to a positive number');
});

test('nothing published is a dash, never a difficulty of zero', () => {
    /* A chain whose difficulty is unknown must not render as a chain at
     * difficulty zero — that reads as a claim, and it is the opposite one. */
    for (const v of [0, null, undefined, NaN, -1, Infinity]) {
        assert.equal(fmtDifficulty(v), '—', `${v} should render as a dash`);
    }
});

test('nodeStatus carries the difficulty the proxy published', () => {
    const node = stats.nodeStatus(makeDb());
    assert.equal(node.tip_height, 914203);
    assert.equal(node.network_difficulty, MAINNET_DIFF);
    /* Its own timestamp, not node_status.updated_at: the tip poll and the
     * template feed are different sources and go stale independently. */
    assert.ok(node.network_difficulty_at > 0);
    assert.notEqual(node.network_difficulty_at, node.updated_at);
});

test('a DB with no published difficulty omits the field rather than faking one', () => {
    for (const diff of [null, 0]) {
        const node = stats.nodeStatus(makeDb(diff));
        assert.equal(node.tip_height, 914203, 'the tip itself still reports');
        assert.ok(!('network_difficulty' in node),
                  'absent, so the view can tell "unknown" from "zero"');
    }
});

test('the node-tip card shows the difficulty, and the exact value with it', async () => {
    const db = makeDb();
    const html = await ejs.renderFile(path.join(VIEWS, 'index.ejs'), {
        ...fmtAll, pool: null, stratumUrl: 'stratum+tcp://x:3334', sidechainId: 9,
        health: null, active: 'overview',
        ov:     stats.overview(db),
        lb:     stats.leaderboard(db),
        lbAddr: stats.leaderboardByAddress(db),
        blocks: stats.recentBlocks(db, 5),
        node:   stats.nodeStatus(db),
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
    }, { views: [VIEWS] });
    assert.match(html, /Network difficulty/);
    assert.match(html, /126\.98 T/);
    /* The rounded form is the readable one, not the authoritative one, so the
     * exact value has to be reachable from the same element. */
    assert.match(html, new RegExp(`Exactly ${MAINNET_DIFF}`));
});

test('an unpublished difficulty renders a dash on the card, not "0"', async () => {
    const db = makeDb(null);
    const html = await ejs.renderFile(path.join(VIEWS, 'index.ejs'), {
        ...fmtAll, pool: null, stratumUrl: 'stratum+tcp://x:3334', sidechainId: 9,
        health: null, active: 'overview',
        ov:     stats.overview(db),
        lb:     stats.leaderboard(db),
        lbAddr: stats.leaderboardByAddress(db),
        blocks: stats.recentBlocks(db, 5),
        node:   stats.nodeStatus(db),
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
    }, { views: [VIEWS] });
    assert.match(html, /Network difficulty/);
    assert.doesNotMatch(html, /Exactly /);
    assert.match(html, /The proxy publishes the difficulty with each block template/);
});
