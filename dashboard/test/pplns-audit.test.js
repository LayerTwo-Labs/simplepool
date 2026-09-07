/* The worker page's audit, under PPLNS.
 *
 * The audit answers "why is this number what it is?", and it was written for
 * PPS: it re-derives a balance by summing shares.credited_sats, because PPS
 * prices a share the moment it arrives and stores that price on the row.
 *
 * PPLNS prices a share in hindsight, out of a block actually found. Every
 * share therefore carries credited_sats = 0, the sum came to zero against a
 * real balance, and the page rendered
 *
 *     ⚠ Off by 4,950,000,000 sats (49.50000000 BTC)
 *     ... Ask the operator to confirm the rate history.
 *
 * to every miner on a PPLNS pool — telling them the pool's books were short by
 * their entire balance, on the one page whose whole purpose is being
 * checkable. Not a display nit: a false accusation against the operator.
 *
 * The fix is not to silence it but to give PPLNS the derivation that actually
 * produced the number: for each matured, distributed block, the window it was
 * split across and this worker's proportion of it — recomputed from the raw
 * shares and blocks_found rows rather than read back from what the payout path
 * wrote.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { worker, fmtHashrate } from '../lib/stats.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');
const NOW    = Math.floor(Date.now() / 1000);
const handleFor = db => ({ get: () => db });

const THUNDER_ADDR = '2sYBNmMJMMZHi6xasMcCPgNiYJ1z';

/* A pool in `mode` with the given workers, and optionally one matured block
 * distributed across whatever shares precede it. */
function makeDb({ mode = 'pplns-thunder', fee_bps = 100, workers = [], block = null } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-pplns-audit-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO pool_meta
                  (id, network, network_source, coinbase_tag, operator_address,
                   pool_btc_address, pool_mode, fee_bps, rate_source,
                   rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                   network_difficulty, block_value_sats, credited_from,
                   listeners, updated_at)
                VALUES (1, 'regtest', 'node', '/sp/', 'bcrt1qop', 'bcrt1qpool',
                        ?, ?, 'derived', 0, 0, 0, 1.0, 5000000000, 1, NULL, ?)`)
      .run(mode, fee_bps, NOW);

    const insWorker = db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                                  VALUES (?, ?, ?, ?, ?)`);
    /* PPS stores a price on every share. PPLNS stores none — that is the whole
     * difference, and writing zeros here is what makes these fixtures honest. */
    const insShare = db.prepare(`INSERT INTO shares
        (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used)
        VALUES (?, ?, ?, 0, NULL, 0, 0)`);

    let ts = NOW - 1000;
    for (const w of workers) {
        insWorker.run(w.id, w.name, NOW - 2000, NOW, w.address || THUNDER_ADDR);
    }
    /* Shares are inserted in the order given, because the window walks
     * BACKWARDS from the block's own share by row id. Order is the point. */
    for (const w of workers) {
        for (let i = 0; i < (w.shares || 0); ++i) insShare.run(w.id, ts++, w.difficulty);
    }

    if (block) {
        db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used)
                    VALUES (?, ?, 0, 1, ?, 0, 0)`).run(block.finder_id, ts++, block.hash);
        db.prepare(`INSERT INTO blocks_found
            (ts, height, hash, finder_id, finder_address, reward_sats, fee_sats,
             status, confirmations, pplns_window_diff, pplns_distributed)
            VALUES (?, ?, ?, ?, 'bcrt1qpool', ?, ?, 'confirmed', 150, ?, 1)`)
          .run(NOW - 50, block.height, block.hash, block.finder_id,
               block.reward_sats, block.fee_sats, block.window_diff);
    }
    for (const w of workers) {
        if (w.accrued != null) {
            db.prepare(`INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
                        VALUES (?, ?, 0, ?)`).run(w.id, w.accrued, NOW);
        }
    }
    return db;
}

const render = (db, name) => {
    const w = worker(handleFor(db), name);
    return ejs.renderFile(path.join(VIEWS, 'worker.ejs'),
        { ...fmt.all, ...w, name, fmtHashrate, pool: null, health: null },
        { views: [VIEWS] });
};

/* One block, one miner, the whole window: 5,000,000,000 gross at 100 bps
 * leaves 4,950,000,000, and alice holds all of it. */
const SOLE_MINER = {
    workers: [{ id: 1, name: 'alice', difficulty: 5.0, shares: 10, accrued: 4950000000 }],
    block: { height: 800100, hash: 'blk_one', finder_id: 1,
             reward_sats: 4950000000, fee_sats: 50000000, window_diff: 50.0 },
};

test('a pplns pool is not told its books are short by the whole balance', async () => {
    const html = await render(makeDb(SOLE_MINER), 'alice');
    assert.doesNotMatch(html, /⚠ Off by/,
        'the page must not report a discrepancy that does not exist');
    assert.doesNotMatch(html, /Ask the operator to confirm the rate history/,
        'and must not send the miner to challenge the operator over it');
});

test('the balance is re-derived from the window that produced it', async () => {
    const db = makeDb(SOLE_MINER);
    const { pplns_audit, pps_audit } = worker(handleFor(db), 'alice');
    assert.equal(pplns_audit.block_count, 1);
    /* gross 5e9, fee 1% -> payable 4.95e9; alice holds 50 of 50 difficulty. */
    assert.equal(pplns_audit.blocks[0].payable, 4950000000);
    assert.equal(pplns_audit.blocks[0].my_diff, 50);
    assert.equal(pplns_audit.blocks[0].win_diff, 50);
    assert.equal(pplns_audit.credited_total, 4950000000);
    assert.equal(pplns_audit.credited_total, pps_audit.accrued,
        'the recomputation must reproduce the stored balance exactly');
    assert.equal(pps_audit.matches, true);
});

test('two miners split a block in proportion to the work they did', async () => {
    /* alice 30, bob 20, of a 50-difficulty window: 60/40 of 4,950,000,000. */
    const db = makeDb({
        workers: [
            { id: 1, name: 'alice', difficulty: 5.0, shares: 6, accrued: 2970000000 },
            { id: 2, name: 'bob',   difficulty: 5.0, shares: 4, accrued: 1980000000 },
        ],
        block: { height: 800200, hash: 'blk_two', finder_id: 2,
                 reward_sats: 4950000000, fee_sats: 50000000, window_diff: 50.0 },
    });
    const a = worker(handleFor(db), 'alice').pplns_audit;
    const b = worker(handleFor(db), 'bob').pplns_audit;
    assert.equal(a.blocks[0].my_diff, 30);
    assert.equal(b.blocks[0].my_diff, 20);
    assert.equal(a.credited_total, 2970000000);
    assert.equal(b.credited_total, 1980000000);
    /* The finder gets no premium: bob found it and is paid for his work, not
     * for the finding. */
    assert.ok(a.credited_total > b.credited_total,
        'the miner with more work in the window is paid more, finder or not');
    assert.equal(a.credited_total + b.credited_total, 4950000000,
        'and between them they get the whole payable amount');
});

test('work done before the window is worth nothing', async () => {
    /* carol mined 1000 difficulty, all of it outside a 50-difficulty window.
     * This is the case a naive "sum every share" query gets wrong, and the
     * reason the audit has to mirror the distributor's walk rather than
     * approximate it. */
    const db = makeDb({
        workers: [
            { id: 3, name: 'carol', difficulty: 25.0, shares: 40, accrued: 0 },
            { id: 1, name: 'alice', difficulty: 5.0,  shares: 10, accrued: 4950000000 },
        ],
        block: { height: 800300, hash: 'blk_three', finder_id: 1,
                 reward_sats: 4950000000, fee_sats: 50000000, window_diff: 50.0 },
    });
    const carol = worker(handleFor(db), 'carol').pplns_audit;
    assert.equal(carol.blocks[0].my_diff, 0, 'carol is outside the window');
    assert.equal(carol.credited_total, 0, 'and is paid nothing, however much she mined');
    const alice = worker(handleFor(db), 'alice').pplns_audit;
    assert.equal(alice.credited_total, 4950000000);
});

test('a pps-classic pool still gets the per-share audit', async () => {
    /* The PPS path must be untouched: it re-derives from credited_sats, which
     * is exactly right for a mode that prices a share on arrival. */
    const db = makeDb({ mode: 'pps-classic',
                        workers: [{ id: 1, name: 'alice', difficulty: 5.0, shares: 4, accrued: 0 }] });
    db.prepare('UPDATE shares SET credited_sats = 100, rate_used = 20').run();
    db.prepare('UPDATE pps_credits SET accrued_sats = 400 WHERE worker_id = 1').run();
    const { pplns_audit, pps_audit } = worker(handleFor(db), 'alice');
    assert.equal(pplns_audit, null, 'no pplns audit on a pps pool');
    assert.equal(pps_audit.accrued_computed, 400);
    assert.equal(pps_audit.matches, true);
    const html = await render(db, 'alice');
    assert.match(html, /Σ FLOOR\(diff × rate\)/, 'the PPS derivation is still shown');
});

test('a pplns pool that has found nothing yet says so, rather than nothing', async () => {
    const db = makeDb({
        workers: [{ id: 1, name: 'alice', difficulty: 5.0, shares: 10, accrued: 0 }],
    });
    const { pplns_audit } = worker(handleFor(db), 'alice');
    assert.equal(pplns_audit.block_count, 0);
    const html = await render(db, 'alice');
    assert.match(html, /No matured block has been distributed yet/);
});
