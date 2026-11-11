import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import Database from 'better-sqlite3';
import { leaderboardByAddress } from '../lib/stats.js';

const NOW = 1780000000;
const WINDOW = 3600;

test('address preaggregation preserves legacy leaderboard semantics', t => {
    t.mock.method(Date, 'now', () => NOW * 1000);

    const db = new Database(':memory:');
    t.after(() => db.close());

    db.exec(readFileSync(
        new URL('../../schema.sql', import.meta.url),
        'utf8'
    ));

    const addWorker = db.prepare(`
        INSERT INTO workers
            (id, name, first_seen, last_seen, payout_address)
        VALUES (?, ?, ?, ?, ?)
    `);

    const workers = [
        [1, 'alpha.rigA', 'alpha'],
        [2, 'alpha.rigB', 'alpha'],
        [3, 'beta.rigC', 'beta'],
        [4, 'legacy.rigD', null],
        [5, 'empty.rigE', ''],
        [6, 'alpha.offline', 'alpha'],
        [7, 'old.rigF', 'old'],
        [8, 'beta.rigG', 'beta']
    ];

    for (const [id, name, address] of workers) {
        addWorker.run(id, name, NOW - 7200, NOW, address);
    }

    const addShare = db.prepare(`
        INSERT INTO shares (worker_id, ts, difficulty)
        VALUES (?, ?, ?)
    `);

    const cutoff = NOW - WINDOW;

    const shares = [
        [1, cutoff, 3],       // Exact cutoff: included
        [1, NOW - 20, 2],
        [2, NOW - 30, 5],
        [3, NOW - 8, 20],
        [4, NOW - 9, 6],
        [5, NOW - 7, 7],
        [8, cutoff, 11],
        [7, cutoff - 1, 100], // Before cutoff: excluded
        [1, cutoff - 1, 200]  // Old work: excluded
    ];

    for (const row of shares) addShare.run(...row);

    // Original upstream query serves as the SQL reference.
    const legacy = db.prepare(`
        SELECT COALESCE(NULLIF(w.payout_address, ''), w.name) AS addr,
               COUNT(s.id) AS shares,
               MAX(s.ts) AS last_seen,
               MIN(s.ts) AS first_ts,
               COALESCE(SUM(s.difficulty), 0) AS sum_diff,
               COUNT(DISTINCT w.id) AS rigs,
               GROUP_CONCAT(DISTINCT w.name) AS worker_names
          FROM shares s
          JOIN workers w ON w.id = s.worker_id
         WHERE s.ts >= ?
         GROUP BY addr
         ORDER BY sum_diff DESC
         LIMIT ?
    `).all(cutoff, 50);

    const optimized = leaderboardByAddress(
        { get: () => db }, WINDOW, 50
    );

    assert.equal(optimized.length, legacy.length);

    const result = new Map(
        optimized.map(row => [row.address, row])
    );

    const totalDifficulty = legacy.reduce(
        (sum, row) => sum + row.sum_diff, 0
    );

    function labels(names) {
        return (names || '')
            .split(',')
            .map(name => {
                const dot = name.indexOf('.');
                return dot >= 0 ? name.slice(dot + 1) : '';
            })
            .filter(Boolean)
            .sort();
    }

    for (const expected of legacy) {
        const actual = result.get(expected.addr);

        assert.ok(actual, `Missing ${expected.addr}`);
        assert.equal(actual.shares, expected.shares);
        assert.equal(actual.rigs, expected.rigs);
        assert.equal(actual.last_seen, expected.last_seen);

        assert.deepEqual(
            [...actual.labels].sort(),
            labels(expected.worker_names)
        );

        const pct = expected.sum_diff /
                    totalDifficulty * 100;

        assert.ok(
            Math.abs(actual.share_of_pool_pct - pct) < 1e-9,
            `${expected.addr}: share percentage changed`
        );

        assert.ok(
            Number.isFinite(actual.hashrate_est) &&
            actual.hashrate_est > 0,
            `${expected.addr}: invalid hashrate`
        );
    }

    assert.equal(result.get('alpha').shares, 3);
    assert.equal(result.get('alpha').rigs, 2);
    assert.equal(result.get('beta').shares, 2);
    assert.equal(result.get('beta').rigs, 2);

    assert.ok(result.has('legacy.rigD'));
    assert.ok(result.has('empty.rigE'));
    assert.ok(!result.has('old'));

    // The old worker and the inactive rig must not appear.
    assert.ok(!result.has('alpha.offline'));

    // Existing LIMIT semantics must remain unchanged.
    const top = leaderboardByAddress(
        { get: () => db }, WINDOW, 1
    );

    assert.equal(top.length, 1);
    assert.equal(top[0].address, 'beta');
    assert.equal(top[0].share_of_pool_pct, 100);
});
