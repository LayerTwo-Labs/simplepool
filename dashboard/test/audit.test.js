/* Smoke tests for the audit surface.
 *
 * These exist because compiling the EJS templates is not enough: a template
 * can compile fine and still throw at render time, and a lib function can be
 * syntactically valid while calling a helper that is not in scope. Both of
 * those shipped once. Every test here therefore drives the real code path
 * end-to-end — build a DB, query through the real lib functions, render the
 * real template — rather than asserting on shapes.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import * as stats from '../lib/stats.js';
import * as admin from '../lib/admin.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const RATE  = 2783.21412641578;   /* derived: 312500000/111157.455 * 0.99 */
const GROSS = 2811.32740041998;

/* A DB shaped like one the proxy has been running against. Returns the raw
 * better-sqlite3 handle plus the lazy {get()} wrapper the dashboard passes
 * around, because different call sites use different ones — which is exactly
 * what broke before. */
function makeDb({ rateSource = 'derived', rate = RATE, feeBps = 100,
                  effectiveFeeBps = 100, shares = 40 } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-audit-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));

    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (1, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 1000, 2000, '3RobWZetukZUXY9kk763AtMyoJtJ')`).run();

    const ins = db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash, credited_sats)
                            VALUES (1, ?, ?, ?, ?, ?)`);
    let accrued = 0;
    for (let i = 0; i < shares; i++) {
        const diff = 1 + (i % 5);
        const credited = Math.floor(diff * rate);   /* per-share truncation, as the proxy does */
        accrued += credited;
        ins.run(1000 + i, diff, i === 7 ? 1 : 0, 'hash' + i, credited);
    }
    db.prepare(`INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
                VALUES (1, ?, 0, 2000)`).run(accrued);
    db.prepare(`INSERT INTO pool_meta (id, pool_mode, fee_bps, rate_source,
                    rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                    network_difficulty, block_value_sats, credited_from, updated_at)
                VALUES (1, 'pps-classic', ?, ?, ?, ?, ?, 111157.455354832, 312500000, 900, 2000)`)
      .run(feeBps, rateSource, rate, GROSS, effectiveFeeBps);

    return { db, handle: { get: () => db }, accrued, file };
}

function render(view, locals) {
    return ejs.renderFile(path.join(VIEWS, view),
                          { ...fmt.all, ...locals },
                          { filename: path.join(VIEWS, view) });
}

test('poolMeta accepts both a lazy handle and a raw db', () => {
    const { db, handle } = makeDb();
    /* stats.js passes a resolved db, admin.js passes the wrapper. Both must
     * work — mixing them up threw "unwrap is not defined" at render time. */
    for (const h of [db, handle]) {
        const m = stats.poolMeta(h);
        assert.ok(m, 'poolMeta returned null');
        assert.equal(m.rate_source, 'derived');
        assert.equal(m.fee_bps, 100);
        assert.ok(Math.abs(m.rate_sats_per_diff - RATE) < 1e-6);
        assert.equal(m.accrues, true);
    }
});

test('poolMeta returns null on a DB with no pool_meta row', () => {
    const { db } = makeDb();
    db.prepare('DELETE FROM pool_meta').run();
    assert.equal(stats.poolMeta(db), null);
});

test('audit sums stored credited_sats and matches the ledger exactly', () => {
    const { handle, accrued } = makeDb();
    const audit = admin.workerAudit(handle, 1);
    assert.ok(audit, 'workerAudit returned null');
    assert.equal(audit.totals.accrued_computed, accrued);
    assert.equal(audit.ledger.accrued, accrued);
    /* The whole point: these agree because both come from the same
     * per-share amount, not from re-deriving against a current rate. */
    assert.equal(audit.totals.accrued_computed - audit.ledger.accrued, 0);
    assert.equal(audit.meta.rate_source, 'derived');
});

test('audit does NOT re-derive history from the current rate', () => {
    /* Shares were credited at RATE; the rate then moves (difficulty change).
     * The audit must still report what was actually credited. */
    const { db, handle, accrued } = makeDb();
    db.prepare('UPDATE pool_meta SET rate_sats_per_diff = ?').run(RATE / 2);
    const audit = admin.workerAudit(handle, 1);
    assert.equal(audit.totals.accrued_computed, accrued,
                 'audit changed when the current rate changed — it is re-deriving');
    assert.equal(audit.ledger.accrued, accrued);
});

test('admin-worker template renders (derived)', async () => {
    const { handle } = makeDb();
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    assert.match(html, /derived/);
    assert.match(html, /Rate &amp; fee|Rate & fee/);
});

test('admin-worker template renders and flags a drifted override', async () => {
    /* An override whose implied fee has drifted from fee_bps is the exact
     * failure pool_meta exists to expose, so the page must say so. */
    const { handle } = makeDb({
        rateSource: 'override', rate: 1000, feeBps: 100,
        effectiveFeeBps: 6443,          /* 1 - 1000/2811.33 */
    });
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    assert.match(html, /override/);
    assert.match(html, /disagrees with the\s+configured/);
});

test('admin-worker template renders on a DB with no pool_meta', async () => {
    const { db, handle } = makeDb();
    db.prepare('DELETE FROM pool_meta').run();
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    /* Must degrade to "rate unknown", never crash or invent a rate. */
    assert.match(html, /predates\s+rate publishing/);
});

test('public worker page renders with the pps audit', async () => {
    const { handle } = makeDb();
    const w = stats.worker(handle, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 86400);
    assert.ok(w.worker, 'worker not found');
    assert.ok(w.pps_audit, 'pps_audit missing');
    assert.equal(w.pps_audit.accrued, w.pps_audit.accrued_computed);
    const html = await render('worker.ejs', { ...w, name: w.worker.name,
                                              fmtHashrate: stats.fmtHashrate });
    assert.ok(html.length > 0);
});

test('solo-style DB (no credits) does not produce a pps audit', () => {
    const { db, handle } = makeDb();
    db.prepare('DELETE FROM pps_credits').run();
    db.prepare('UPDATE shares SET credited_sats = 0').run();
    const w = stats.worker(handle, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 86400);
    assert.equal(w.pps_audit, null);
});
