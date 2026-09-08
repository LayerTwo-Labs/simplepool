/* What a pplns-coinbase pool tells the people it costs.
 *
 * This mode forfeits a claim below the payout floor to the operator and never
 * settles it. That is defensible as a stated rule and indefensible as a
 * discovery, and the whole case for the policy rests on the miner being able
 * to see it BEFORE pointing a rig at the pool. The operator's log is the one
 * place they cannot look, so these tests treat the disclosure as part of the
 * feature rather than as decoration.
 *
 * They also pin the mislabels this mode exposed. The dashboard used to answer
 * "not pps-classic" with the word "solo" in three places, so every pplns pool
 * was told it was solo by the same pages whose header said otherwise.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import { poolMeta, fmtHashrate } from '../lib/stats.js';
import { health as runHealth } from '../lib/health.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const OPERATOR = 'bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080';

function makeDb({ mode = 'pplns-coinbase', floor = 546 } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-cbwin-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO pool_meta
                  (id, network, network_source, coinbase_tag, operator_address,
                   pool_btc_address, pool_mode, fee_bps, rate_source,
                   rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                   network_difficulty, block_value_sats, credited_from,
                   listeners, updated_at, pplns_payout_floor_sats)
                VALUES (1, 'regtest', 'node', '/sp/', @op, NULL, @mode, 100,
                        'derived', 0, 0, 100, 1, 5000000000, 1, NULL, 1, @floor)`)
      .run({ op: OPERATOR, mode, floor });
    return db;
}

const render = (view, locals) =>
    ejs.renderFile(path.join(VIEWS, view), { ...fmt.all, ...locals },
                   { views: [VIEWS] });

const about = db => render('partial/about-numbers.ejs',
                           { pool: poolMeta(db), stratumUrl: 'stratum+tcp://x:3334',
                             sidechainId: 9 });

test('the payout floor is stated to the miner, in sats', async () => {
    const html = await about(makeDb({ floor: 25000 }));
    assert.match(html, /25,000 sats/, 'the floor itself');
    /* Integers, not "25,000.00 sats" -- satoshis do not have decimals, and
     * the shared BTC formatter rendered the first version that way. */
    assert.doesNotMatch(html, /25,000\.00 sats/);
});

test('the floor is described as forfeited, never as carried', async () => {
    const html = await about(makeDb());
    /* The exact claim a miner has to come away with. Softening any of these
     * into "held" or "later" would describe the design we deliberately did
     * NOT build, and would be a false promise rather than a vague one. */
    assert.match(html, /not.{0,30}carried forward/is);
    assert.match(html, /goes to the operator/i);
    assert.match(html, /earn nothing/i);
});

test('a proxy that never published a floor claims none', async () => {
    /* An older proxy stores NULL here. Rendering the default 546 anyway would
     * be stating a policy on that operator's behalf, which is worse than
     * staying quiet: the operator may be running a build that has no floor. */
    const db = makeDb();
    db.prepare('UPDATE pool_meta SET pplns_payout_floor_sats = NULL').run();
    const html = await about(db);
    assert.doesNotMatch(html, /There is a minimum/i);
    assert.doesNotMatch(html, /546/);
    /* But the mode itself is still described -- silence about the floor must
     * not become silence about the mode. */
    assert.match(html, /pplns-coinbase/);
});

test('a zero floor is still a floor, and still disclosed', async () => {
    /* 0 means "pay anything the dust limit allows" -- a real policy, and
     * distinct from NULL. A `|| null` normalisation would collapse the two
     * and silently stop disclosing. */
    const html = await about(makeDb({ floor: 0 }));
    assert.match(html, /There is a minimum/i);
});

test('every mode gets its own guidance, and none is called solo', async () => {
    for (const mode of ['pplns-coinbase', 'pplns-btc', 'pplns-thunder']) {
        const html = await about(makeDb({ mode }));
        assert.match(html, new RegExp(mode),
                     `${mode} should name itself`);
        /* The bug: all three fell through to the unknown-mode branch. */
        assert.doesNotMatch(html, /has not published its mode yet/,
                            `${mode} should not read as unknown`);
    }
});

test('the pplns rails ask for the right username type', async () => {
    const thunder = await about(makeDb({ mode: 'pplns-thunder' }));
    assert.match(thunder, /your-thunder-address/);
    const btc = await about(makeDb({ mode: 'pplns-btc' }));
    assert.match(btc, /your-bitcoin-address/);
    const cb = await about(makeDb({ mode: 'pplns-coinbase' }));
    assert.match(cb, /your-bitcoin-address/);
});

test('solvency is not claimed for a pool that holds nothing', async () => {
    /* In pplns-coinbase blocks_found.reward_sats is what the block paid the
     * MINERS. Summing it as pool revenue reported a healthy 50 BTC margin for
     * a pool with no wallet -- a green light asserting custody that does not
     * exist. */
    const db = makeDb();
    db.prepare(`INSERT INTO blocks_found (ts, height, hash, reward_sats,
                                          fee_sats, status)
                VALUES (1, 11, 'aa', 4950000000, 50000000, 'confirmed')`).run();
    const margin = runHealth(db).checks.find(c => c.id === 'margin');
    assert.equal(margin.value, null, 'no margin figure for a custody-free pool');
    assert.match(margin.detail, /never holds the reward/);

    /* And the check still works where custody is real. */
    const pps = makeDb({ mode: 'pps-classic' });
    pps.prepare(`INSERT INTO blocks_found (ts, height, hash, reward_sats,
                                           fee_sats, status)
                 VALUES (1, 11, 'aa', 4950000000, 50000000, 'confirmed')`).run();
    assert.equal(runHealth(pps).checks.find(c => c.id === 'margin').value,
                 5000000000);
});

test('the accrual check names the mode it is actually in', async () => {
    for (const mode of ['solo', 'pplns-coinbase', 'pplns-btc', 'pplns-thunder']) {
        const c = runHealth(makeDb({ mode })).checks
                    .find(x => x.id === 'pps_difficulty');
        assert.match(c.detail, new RegExp(mode),
                     `${mode} should be named, not called solo`);
    }
});

test('a mode with no balance does not report one as owed', async () => {
    const html = await render('worker.ejs', {
        pool: poolMeta(makeDb()),
        health: { ok: true, checks: [] },
        worker: { name: 'w', payout_address: 'bcrt1q', first_seen: 1,
                  last_seen: 1, window_shares: 0, window_hashrate: 0 },
        name: 'w', shares: [], buckets: [], window_sec: 86400,
        pps_audit: null, pplns_audit: null, payouts: [], blocks: [],
        fmtHashrate,
        stratumUrl: 'stratum+tcp://x:3334', sidechainId: 9,
    });
    assert.doesNotMatch(html, /solo mode/,
                        'a pplns-coinbase worker page must not claim solo');
    assert.match(html, /paid in the coinbase/);
});
