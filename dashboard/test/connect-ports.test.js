/* The "which port" block on the connect card.
 *
 * A stratum URL says nothing about the difficulty behind it, and pointing a
 * rented fleet at the home-miner port is not a subtle failure: one connection
 * carrying a whole fleet at difficulty 1 is hundreds of thousands of submits
 * a second, the pool limits it, and the marketplace cancels the order for
 * work the pool appears to be rejecting. So the ports are listed with what
 * each is for.
 *
 * The second property here is the one the operator-facing health check
 * already reports and the miner never saw: a port holding a floor ABOVE the
 * network difficulty makes its miners discard blocks they solved. That is
 * the miner's loss, so it is disclosed to the miner.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import fs from 'node:fs';
import os from 'node:os';
import ejs from 'ejs';
import Database from 'better-sqlite3';

import * as fmt from '../lib/fmt.js';
import { poolMeta } from '../lib/stats.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VIEWS  = path.resolve(__dirname, '../views');
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const URL_   = 'stratum+tcp://pool.example.org:3334';

const HOME   = { port: 3334, label: null, min_diff: 1, initial_diff: 1,
                 promised_min_diff: 0 };
const RENTED = { port: 3335, label: 'braiins', min_diff: 65536,
                 initial_diff: 65536, promised_min_diff: 65536 };

const ports = (pool, stratumUrl = URL_) =>
    ejs.renderFile(path.join(VIEWS, 'partial/connect-ports.ejs'),
                   { ...fmt.all, pool, stratumUrl }, { views: [VIEWS] });

const pool = (listeners, extra = {}) => ({
    pool_mode: 'pplns-coinbase', fee_bps: 100, network: 'signet',
    network_difficulty: 1e12, listeners, ...extra,
});

test('each port is printed as a dialable URL with who it is for', async () => {
    const html = await ports(pool([HOME, RENTED]));
    assert.match(html, /stratum\+tcp:\/\/pool\.example\.org:3334\s+individual miners/);
    assert.match(html, /stratum\+tcp:\/\/pool\.example\.org:3335\s+rented or aggregated hashrate \(braiins\)/);
    assert.match(html, /65,536/);
});

test('a pool with one port explains nothing — there is no choice to make', async () => {
    assert.equal((await ports(pool([HOME]))).trim(), '');
    assert.equal((await ports(pool(null))).trim(), '');
    assert.equal((await ports(null)).trim(), '');
});

test('the difficulty of a port is not a claim about what it pays', async () => {
    /* The fear this answers: "the big-difficulty port must pay less per
     * share". It pays the same per unit of difficulty, and a miner who does
     * not know that will avoid the port they belong on. */
    const html = await ports(pool([HOME, RENTED]));
    assert.match(html, /does not change what you earn/);
    assert.match(html, /credited by its difficulty/);
});

test('solo does not answer that with a credit it does not pay', async () => {
    /* Same reassurance, different reason: in solo a share is credited
     * nothing at all, so "work is credited by its difficulty" would be the
     * card promising a payment this mode never makes. */
    const html = await ports(pool([HOME, RENTED], { pool_mode: 'solo' }));
    assert.match(html, /does not change what you earn/);
    assert.doesNotMatch(html, /credited by its difficulty/);
    assert.match(html, /only a block\s+pays/);
});

test('a floor held above the network difficulty is disclosed, with its size', async () => {
    /* 500000 held over a chain at 1200: ~416 of every 417 blocks solved on
     * that port are filtered out by the miner before the pool sees them. */
    const nice = { port: 3336, label: 'nicehash', min_diff: 500000,
                   initial_diff: 500000, promised_min_diff: 500000 };
    const html = await ports(pool([HOME, nice], { network_difficulty: 1200 }));
    assert.match(html, /A held floor costs blocks/);
    assert.match(html, /3336/);
    assert.match(html, /416 of every\s+417/);
    assert.match(html, /1,200/);
});

test('the worst floor is the one reported, not the first', async () => {
    const nice = { port: 3336, label: 'nicehash', min_diff: 500000,
                   initial_diff: 500000, promised_min_diff: 500000 };
    const html = await ports(pool([HOME, RENTED, nice], { network_difficulty: 1200 }));
    assert.match(html, /Port <strong>3336<\/strong>/);
});

test('a floor the chain is already above costs nothing and says so', async () => {
    const html = await ports(pool([HOME, RENTED], { network_difficulty: 1e12 }));
    assert.doesNotMatch(html, /A held floor costs blocks/);
    assert.match(html, /not in that position right now/);
});

test('a pool where no port holds a floor gets neither paragraph', async () => {
    /* initial_diff high, promised_min_diff 0: configured, not promised. The
     * network difficulty clamps it, so no block is lost and there is nothing
     * to disclose. */
    const configured = { port: 3335, label: 'big', min_diff: 1,
                         initial_diff: 65536, promised_min_diff: 0 };
    const html = await ports(pool([HOME, configured], { network_difficulty: 1200 }));
    assert.doesNotMatch(html, /A held floor costs blocks/);
    assert.doesNotMatch(html, /not in that position right now/);
    /* Still listed, and still on the rented side of the list. */
    assert.match(html, /rented or aggregated hashrate \(big\)/);
});

test('no published stratum URL yields a placeholder host, not a wrong one', async () => {
    const html = await ports(pool([HOME, RENTED]), '');
    assert.match(html, /stratum\+tcp:\/\/&lt;pool-host&gt;:3335/);
    assert.doesNotMatch(html, /example\.org/);
});

test('a forknet difficulty is not rounded away to zero', async () => {
    const tiny = { port: 3335, label: 'tiny', min_diff: 3e-10,
                   initial_diff: 3e-10, promised_min_diff: 3e-10 };
    const html = await ports(pool([HOME, tiny], { network_difficulty: 1e-12 }));
    assert.match(html, /3e-10/);
    assert.doesNotMatch(html, /difficulty 0,/);
});

test('a proxy that never published promised_min_diff promises nothing', async () => {
    /* An older proxy writes port/label/min_diff/initial_diff only. Reading a
     * missing promise as a floor would print a block-loss warning at every
     * miner of a pool that is not losing any. Goes through the real
     * pool_meta read rather than a hand-built object, because the default is
     * parseListeners' to get wrong. */
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-ports-')),
                           'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO pool_meta
                  (id, pool_mode, fee_bps, rate_source, rate_sats_per_diff,
                   gross_sats_per_diff, effective_fee_bps, network_difficulty,
                   block_value_sats, credited_from, listeners, updated_at)
                VALUES (1, 'pplns-coinbase', 100, 'derived', 0, 0, 100, 1200,
                        312500000, 1, ?, 1)`)
      .run(JSON.stringify([
          { port: 3334, label: '',    min_diff: 1,     initial_diff: 1 },
          { port: 3335, label: 'old', min_diff: 65536, initial_diff: 65536 },
      ]));
    const meta = poolMeta(db);
    assert.equal(meta.listeners[1].promised_min_diff, 0);

    const html = await ports(meta);
    assert.match(html, /rented or aggregated hashrate \(old\)/);
    assert.doesNotMatch(html, /A held floor costs blocks/);
});
