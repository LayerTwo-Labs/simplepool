/* Only one payout can be in flight against a Thunder wallet.
 *
 * Thunder selects UTXOs without excluding those already spent by transactions
 * in its own mempool, and a transfer consumes every wallet UTXO — returning
 * the remainder as change that is unspendable until the tx is mined. So a
 * second transfer picks the very inputs the first one spends and is rejected:
 *
 *     mempool error: can't add transaction, utxo double spent
 *
 * Observed in production: one payout broadcast, then four failures every
 * thirty seconds indefinitely, because nothing in the loop knew to wait.
 *
 * These tests pin the two rules that follow — wait while a payout is
 * unconfirmed, and never broadcast twice in one tick — and, just as
 * importantly, the cases that must NOT block: a confirmed payout, and a txid
 * the node has forgotten.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import Database from 'better-sqlite3';

import { runOnce } from '../lib/payout.js';
import { lastBroadcastPayout } from '../lib/db.js';

/* Minimal schema — just the tables the payout loop touches. */
function makeDb({ payouts = [], owed = {} } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-payout-')), 'p.db');
    const db = new Database(file);
    db.exec(`
        CREATE TABLE workers (id INTEGER PRIMARY KEY, name TEXT, payout_address TEXT);
        CREATE TABLE pps_credits (worker_id INTEGER PRIMARY KEY, accrued_sats INTEGER,
                                  paid_sats INTEGER, last_updated INTEGER);
        CREATE TABLE payouts_in_flight (id INTEGER PRIMARY KEY AUTOINCREMENT,
                                  worker_id INTEGER, sats INTEGER, txid TEXT, started_at INTEGER);
        CREATE TABLE payouts (id INTEGER PRIMARY KEY AUTOINCREMENT, worker_id INTEGER,
                                  sats INTEGER, fee_sats INTEGER, txid TEXT,
                                  paid_at INTEGER, note TEXT);
        CREATE TABLE tx_attempts (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER,
                                  kind TEXT, status TEXT, stage TEXT, txid TEXT, raw_tx TEXT,
                                  amount_sats INTEGER, fee_sats INTEGER, destination TEXT,
                                  worker_id INTEGER, error TEXT, detail TEXT);
    `);
    let i = 0;
    for (const [name, sats] of Object.entries(owed)) {
        i++;
        db.prepare('INSERT INTO workers VALUES (?,?,?)').run(i, name, 'addr' + i);
        db.prepare('INSERT INTO pps_credits VALUES (?,?,0,0)').run(i, sats);
    }
    for (const p of payouts) {
        db.prepare(`INSERT INTO payouts (worker_id, sats, fee_sats, txid, paid_at)
                    VALUES (?,?,?,?,?)`).run(p.worker_id ?? 1, p.sats ?? 100, 100, p.txid, p.paid_at ?? 1000);
    }
    return db;
}

/* Thunder stub. `txState` maps txid -> {known, confirmed}. */
function thunderStub({ txState = {}, balance = 10n ** 12n, onTransfer } = {}) {
    const calls = { transfers: 0, getTx: 0 };
    return {
        calls,
        async balance() { return { available_sats: String(balance), total_sats: String(balance) }; },
        async getTransaction(txid) {
            calls.getTx++;
            return txState[txid] ?? { known: false, confirmed: false, blockHash: null };
        },
        async transferDetailed(dest, sats) {
            calls.transfers++;
            if (onTransfer) onTransfer(calls.transfers);
            return { txid: `tx${calls.transfers}`, unsigned: {}, signed: {} };
        },
    };
}

const quietLog = { info() {}, warn() {}, error() {}, debug() {} };
const cfg = { minSats: 10000n, maxPerTick: 50, dryRun: false, intervalMs: 1000 };

test('an unconfirmed payout blocks the tick instead of double spending', async () => {
    const db = makeDb({
        payouts: [{ txid: 'abc123', worker_id: 1 }],
        owed: { rig1: 5_000_000, rig2: 6_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: false } } });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.attempted, 0);
    assert.equal(r.waiting_on, 'abc123');
    assert.equal(thunder.calls.transfers, 0, 'must not broadcast while one is pending');
});

test('a confirmed payout does not block', async () => {
    const db = makeDb({
        payouts: [{ txid: 'abc123', worker_id: 1 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: { abc123: { known: true, confirmed: true } } });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.paid, 1);
    assert.equal(thunder.calls.transfers, 1);
});

test('a txid the node has forgotten does not block forever', async () => {
    /* Evicted from the mempool, or the node was reset. Nothing is holding the
     * inputs, so refusing to pay would strand every miner permanently. */
    const db = makeDb({
        payouts: [{ txid: 'gone999', worker_id: 1 }],
        owed: { rig1: 5_000_000 },
    });
    const thunder = thunderStub({ txState: {} });   /* unknown txid */

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.paid, 1);
});

test('an unreachable node blocks rather than guessing', async () => {
    /* "Cannot tell" must not be read as "nothing pending" — broadcasting
     * against a wallet we cannot see is how the double spend happened. */
    const db = makeDb({ payouts: [{ txid: 'abc123' }], owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    thunder.getTransaction = async () => ({ known: false, confirmed: false, error: 'ECONNREFUSED' });

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.attempted, 0);
    assert.equal(thunder.calls.transfers, 0);
});

test('no prior payout at all does not block a first payout', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000 } });
    const thunder = thunderStub();
    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(r.paid, 1);
    assert.equal(thunder.calls.getTx, 0, 'nothing to check when no payout was ever made');
});

test('at most one payout is broadcast per tick', async () => {
    /* The in-tick version of the same conflict: worker 2 would select the
     * inputs worker 1 just spent. Four due workers used to mean one success
     * and three double-spend failures. */
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000, rig4: 8_000_000 } });
    const thunder = thunderStub();

    const r = await runOnce({ db, thunder, cfg }, quietLog);
    assert.equal(thunder.calls.transfers, 1, 'exactly one broadcast');
    assert.equal(r.paid, 1);
    assert.equal(r.failed, 0, 'the deferred workers are not failures');

    /* Only the paid worker is debited; the rest keep their full balance. */
    const paid = db.prepare('SELECT worker_id, paid_sats FROM pps_credits WHERE paid_sats > 0').all();
    assert.equal(paid.length, 1);
    /* And no in-flight rows are left behind to strand them. */
    assert.equal(db.prepare('SELECT COUNT(*) n FROM payouts_in_flight').get().n, 0);
});

test('successive ticks drain the queue one worker at a time', async () => {
    const db = makeDb({ owed: { rig1: 5_000_000, rig2: 6_000_000, rig3: 7_000_000 } });
    const confirmed = {};
    const thunder = thunderStub({ txState: confirmed });

    for (let i = 1; i <= 3; i++) {
        const r = await runOnce({ db, thunder, cfg }, quietLog);
        assert.equal(r.paid, 1, `tick ${i} should pay exactly one`);
        confirmed[`tx${i}`] = { known: true, confirmed: true };   /* mined between ticks */
    }
    assert.equal(db.prepare('SELECT COUNT(*) n FROM payouts').get().n, 3);
    assert.equal(db.prepare('SELECT COUNT(*) n FROM pps_credits WHERE accrued_sats > paid_sats').get().n, 0);
});

test('lastBroadcastPayout ignores rows with no txid and picks the newest', async () => {
    const db = makeDb({ payouts: [
        { txid: 'old', worker_id: 1, paid_at: 5000 },
        { txid: '',    worker_id: 1, paid_at: 5000 },   /* never broadcast */
        { txid: 'new', worker_id: 1, paid_at: 5000 },   /* same second as 'old' */
    ] });
    /* Same paid_at throughout: ordering must come from id, not the timestamp,
     * or a same-second pair reports the wrong one. */
    assert.equal(lastBroadcastPayout(db).txid, 'new');
});
