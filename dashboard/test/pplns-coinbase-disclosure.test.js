/* What a pplns-coinbase pool tells the people it costs.
 *
 * This mode pays a claim below the payout floor nothing from that block: its
 * share goes to the other miners in the window (never the operator) and the
 * miner is owed a turn in the payout queue, so being small costs frequency
 * rather than money. That is defensible as a stated rule and indefensible as
 * a discovery, and the whole case for the policy rests on the miner being
 * able to see it BEFORE pointing a rig at the pool. The operator's log is the
 * one place they cannot look, so these tests treat the disclosure as part of
 * the feature rather than as decoration. The header of this file said the
 * old rule -- forfeited to the operator -- for a while after the tests below
 * started asserting the page must not say that.
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

import { poolMeta, fmtHashrate, templates as statsTemplates } from '../lib/stats.js';
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

test('a skipped claim is described as shared out, never as the operator\'s', async () => {
    const html = await about(makeDb());
    /* The exact claim a miner has to come away with, and it is the opposite of
     * what this test asserted before #76: what a block cannot pay goes to the
     * OTHER MINERS, and the operator still takes only its fee. */
    assert.match(html, /shared out\s*among the miners/is);
    assert.match(html, /takes only its fee/i);
    /* And that the cost is frequency, not amount — the sentence a small miner
     * needs in order to decide whether to point a rig here. */
    assert.match(html, /less often/i);
    assert.match(html, /first in the queue/i);
    /* The page must NOT tell miners their share goes to the operator, which
     * is what it used to say and is now simply false. */
    assert.doesNotMatch(html, /amount goes to the operator/i);
    assert.doesNotMatch(html, /forfeit/i);
});

test('the queue is described as an order, not a balance', async () => {
    /* The property that makes it defensible: no money is held. A miner who
     * reads this must not come away believing the pool owes them a payout
     * they could one day claim. */
    const html = await about(makeDb());
    assert.match(html, /no balance to withdraw/i);
    assert.match(html, /nobody would be short a payment/i);
});

test('a proxy that never published a floor claims none', async () => {
    /* An older proxy stores NULL here. Rendering the default 546 anyway would
     * be stating a policy on that operator's behalf, which is worse than
     * staying quiet: the operator may be running a build that has no floor. */
    const db = makeDb();
    db.prepare('UPDATE pool_meta SET pplns_payout_floor_sats = NULL').run();
    const html = await about(db);
    assert.doesNotMatch(html, /may not pay everyone/i);
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
    assert.match(html, /may not pay everyone/i);
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

/* The fourth place. templates.ejs answered a zero rate with "only pps-classic
 * prices a share on arrival" -- true of pps-classic, and no answer at all to
 * the pplns operator who is looking at their own pool and wondering what is
 * n/a and why. Same bug as the three above, one page later. */
function withTemplate(db, { rate = 0, height = 997058 } = {}) {
    db.prepare(`INSERT INTO templates
                  (ts, height, prev_hash, bits, network_difficulty,
                   coinbase_value_sats, tx_count, tx_fees_sats, source,
                   cb_spendable, cb_op_returns, longpoll, rate_sats_per_diff,
                   last_seen, polls)
                VALUES (@ts, @height, @prev, '1900ffff', 4294967296,
                        313374735, 1158, 874735, 'enforcer', 1, 8, 1, @rate,
                        @ts, 62)`)
      .run({ ts: Math.floor(Date.now() / 1000), height, rate,
             prev: '00'.repeat(32) });
    return db;
}

const templatesPage = db => render('templates.ejs', {
    templates: statsTemplates(db), pool: poolMeta(db),
    health: { ok: true, checks: [] },
    stratumUrl: 'stratum+tcp://x:3334', sidechainId: 9,
});

test('the templates page names the mode it is in, not pps-classic', async () => {
    for (const mode of ['pplns-coinbase', 'pplns-btc', 'pplns-thunder', 'solo']) {
        const html = await templatesPage(withTemplate(makeDb({ mode })));
        assert.doesNotMatch(html, /only pps-classic prices a share on arrival/,
                            `${mode} was told about a mode it is not in`);
        assert.match(html, new RegExp(mode), `${mode} should name itself`);
        /* And the label must stop claiming a PPS rate this pool has none of. */
        assert.doesNotMatch(html, /<label>PPS rate<\/label>/,
                            `${mode} has no PPS rate to label`);
    }
});

test('a pplns pool is told where the price does come from', async () => {
    const html = await templatesPage(withTemplate(makeDb()));
    assert.match(html, /priced when a block is found/i);
    assert.match(html, /own coinbase/i);
});

test('pps-classic still shows its rate, and its zero means something else', async () => {
    const priced = await templatesPage(
        withTemplate(makeDb({ mode: 'pps-classic' }), { rate: 1036.8368 }));
    assert.match(priced, /<label>PPS rate<\/label>/);
    assert.match(priced, /1036\.8368 sats\/diff/);

    /* Zero under pps-classic is not "this mode has no rate" -- it is accrual
     * that has stopped, which is the one case where the operator must not be
     * reassured by an "n/a". */
    const gated = await templatesPage(
        withTemplate(makeDb({ mode: 'pps-classic' }), { rate: 0 }));
    assert.match(gated, /nothing is accruing/i);
    assert.doesNotMatch(gated, /n\/a/);
});

test('the history rate column disappears when no row was ever priced', async () => {
    const db = makeDb();
    withTemplate(db, { height: 1 });
    withTemplate(db, { height: 2 });
    const html = await templatesPage(db);
    assert.doesNotMatch(html, /<th>PPS rate<\/th>/,
                        'a dead column of em-dashes on a table that scrolls');

    /* But a pool that switched away from pps-classic keeps its priced
     * history legible -- the column follows the data, not the current mode. */
    const switched = makeDb();
    withTemplate(switched, { height: 1, rate: 1036.8368 });
    withTemplate(switched, { height: 2 });
    assert.match(await templatesPage(switched), /<th>PPS rate<\/th>/);
});
