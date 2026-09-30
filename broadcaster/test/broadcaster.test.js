import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Broadcaster } from '../lib/broadcaster.js';
import { emptyState } from '../lib/state.js';
import { TelegramError } from '../lib/telegram.js';

const quietLog = { info() {}, warn() {}, error() {} };

/* 2026-09-30 10:00 UTC */
const T0 = Date.UTC(2026, 8, 30, 10, 0, 0);
const MIN = 60_000;

function cfg(overrides = {}) {
    return {
        poolName: 'testpool',
        publicUrl: 'https://pool.example',
        digestHour: 12,
        live: false,
        liveMs: 300_000,
        staleSharesSec: 900,
        debounce: 2,
        ...overrides,
    };
}

function status({ lastShareTs, healthOk = true, healthStatus } = {}) {
    return {
        pool: {
            db_ready: true,
            hashrate: 1e12, hashrate_1h: 2e12, hashrate_5m: 3e12,
            workers_active: 4, accepted: 1000, reject_rate_pct: 0.5,
            best_share_24h: 123456, blocks_lifetime: 7,
            last_share_ts: lastShareTs, mode: 'pps-classic', fee_bps: 100,
        },
        health: healthStatus
            ? { ok: false, status: healthStatus }
            : { ok: healthOk, failing: healthOk ? [] : [{ id: 'x', label: 'Something broke' }] },
    };
}

function block(hash, height, st = 'pending', ts = T0 / 1000) {
    return { hash, height, status: st, ts, reward_sats: 312500000, finder: 'rig<1>' };
}

class FakeDashboard {
    constructor() { this.s = status({ lastShareTs: T0 / 1000 }); this.b = []; }
    async status() { return this.s; }
    async blocks() { return [...this.b].sort((a, b) => b.ts - a.ts); }
}

class FakeTelegram {
    constructor() { this.posts = []; this.edits = []; this.pins = []; this.editError = null; }
    async send(html) { this.posts.push(html); return this.posts.length; }
    async edit(id, html) {
        if (this.editError) { const e = this.editError; this.editError = null; throw e; }
        this.edits.push({ id, html });
    }
    async pin(id) { this.pins.push(id); }
}

function setup(cfgOverrides, state = emptyState()) {
    const dashboard = new FakeDashboard();
    const telegram = new FakeTelegram();
    let saves = 0;
    const b = new Broadcaster({
        dashboard, telegram, state, cfg: cfg(cfgOverrides), log: quietLog,
        persist: () => { saves++; },
    });
    return { b, dashboard, telegram, state, saves: () => saves };
}

test('first run records existing blocks without posting them', async () => {
    const { b, dashboard, telegram, state } = setup();
    dashboard.b = [block('aa', 100, 'confirmed'), block('bb', 101)];
    await b.tick(T0);
    assert.equal(telegram.posts.length, 0);
    assert.equal(state.initialized, true);
    assert.deepEqual(state.blocks, { aa: 'confirmed', bb: 'pending' });
});

test('a new block is announced once, oldest first, and escaped', async () => {
    const { b, dashboard, telegram } = setup();
    await b.tick(T0);
    dashboard.b = [block('cc', 102, 'pending', T0 / 1000 + 2), block('dd', 103, 'pending', T0 / 1000 + 5)];
    await b.tick(T0 + MIN);
    assert.equal(telegram.posts.length, 2);
    assert.match(telegram.posts[0], /Height <b>102<\/b>/);
    assert.match(telegram.posts[1], /Height <b>103<\/b>/);
    assert.match(telegram.posts[0], /rig&lt;1&gt;/);
    assert.match(telegram.posts[0], /3\.125 BTC/);

    await b.tick(T0 + 2 * MIN);
    assert.equal(telegram.posts.length, 2, 'no repeat');
});

test('pending -> confirmed is silent; confirmed -> orphaned is announced', async () => {
    const { b, dashboard, telegram } = setup();
    dashboard.b = [block('ee', 104)];
    await b.tick(T0);
    dashboard.b = [block('ee', 104, 'confirmed')];
    await b.tick(T0 + MIN);
    assert.equal(telegram.posts.length, 0);
    dashboard.b = [block('ee', 104, 'orphaned')];
    await b.tick(T0 + 2 * MIN);
    assert.equal(telegram.posts.length, 1);
    assert.match(telegram.posts[0], /orphaned/);
});

test('a block first seen already rejected is not announced', async () => {
    const { b, dashboard, telegram } = setup();
    await b.tick(T0);
    dashboard.b = [block('ff', 105, 'rejected')];
    await b.tick(T0 + MIN);
    assert.equal(telegram.posts.length, 0);
});

test('shares stopping is announced only after it holds for `debounce` polls', async () => {
    const { b, dashboard, telegram, state } = setup();
    const lastShare = T0 / 1000;
    dashboard.s = status({ lastShareTs: lastShare });
    await b.tick(T0);
    assert.equal(state.sharesFlowing, true);

    await b.tick(T0 + 16 * MIN);            // stale, 1st poll
    assert.equal(telegram.posts.length, 0);
    await b.tick(T0 + 17 * MIN);            // stale, 2nd poll
    assert.equal(telegram.posts.length, 1);
    assert.match(telegram.posts[0], /no shares accepted for 17m/);

    await b.tick(T0 + 18 * MIN);
    assert.equal(telegram.posts.length, 1, 'said once');

    dashboard.s = status({ lastShareTs: (T0 + 19 * MIN) / 1000 });
    await b.tick(T0 + 19 * MIN);
    await b.tick(T0 + 20 * MIN);
    assert.equal(telegram.posts.length, 2);
    assert.match(telegram.posts[1], /coming in again/);
});

test('a single flapping poll does not post', async () => {
    const { b, dashboard, telegram } = setup({ debounce: 3 });
    await b.tick(T0);
    dashboard.s = status({ lastShareTs: T0 / 1000, healthOk: false });
    await b.tick(T0 + MIN);
    await b.tick(T0 + 2 * MIN);
    dashboard.s = status({ lastShareTs: (T0 + 3 * MIN) / 1000, healthOk: true });
    await b.tick(T0 + 3 * MIN);
    dashboard.s = status({ lastShareTs: (T0 + 4 * MIN) / 1000, healthOk: false });
    await b.tick(T0 + 4 * MIN);
    assert.equal(telegram.posts.length, 0);
});

test('health failing lists the failing checks; "checking" is ignored', async () => {
    const { b, dashboard, telegram, state } = setup();
    dashboard.s = status({ lastShareTs: T0 / 1000, healthStatus: 'checking' });
    await b.tick(T0);
    assert.equal(state.healthOk, null);

    dashboard.s = status({ lastShareTs: T0 / 1000, healthOk: true });
    await b.tick(T0 + MIN);
    assert.equal(state.healthOk, true);
    assert.equal(telegram.posts.length, 0);

    dashboard.s = status({ lastShareTs: (T0 + 2 * MIN) / 1000, healthOk: false });
    await b.tick(T0 + 2 * MIN);
    await b.tick(T0 + 3 * MIN);
    assert.equal(telegram.posts.length, 1);
    assert.match(telegram.posts[0], /• Something broke/);
});

test('digest posts once per UTC day at or after the configured hour', async () => {
    const { b, dashboard, telegram, state } = setup();
    await b.tick(T0);                                  // 10:00, seeded, before hour
    assert.equal(state.lastDigestDate, null);
    dashboard.s = status({ lastShareTs: (T0 + 2 * 60 * MIN) / 1000 });
    dashboard.b = [block('gg', 106, 'confirmed', T0 / 1000 - 3600)];
    await b.tick(T0 + 2 * 60 * MIN);                   // 12:00
    const digests = telegram.posts.filter((p) => p.includes('daily report'));
    assert.equal(digests.length, 1);
    assert.match(digests[0], /2026-09-30/);
    assert.match(digests[0], /2\.00 TH\/s<\/b> \(1h\)/);
    assert.match(digests[0], /pps-classic · fee 1\.00%/);
    assert.match(digests[0], /href="https:\/\/pool\.example\/"/);

    await b.tick(T0 + 3 * 60 * MIN);
    assert.equal(telegram.posts.filter((p) => p.includes('daily report')).length, 1);
});

test('first run after the digest hour does not backfill today', async () => {
    const { b, telegram, state } = setup();
    await b.tick(T0 + 4 * 60 * MIN);                   // 14:00
    assert.equal(state.lastDigestDate, '2026-09-30');
    assert.equal(telegram.posts.length, 0);
});

test('live message is posted and pinned once, then edited', async () => {
    const { b, telegram, state } = setup({ live: true });
    await b.tick(T0);
    assert.equal(telegram.posts.length, 1);
    assert.deepEqual(telegram.pins, [1]);
    assert.equal(state.liveMessageId, 1);

    await b.tick(T0 + MIN);                            // within liveMs
    assert.equal(telegram.edits.length, 0);
    await b.tick(T0 + 5 * MIN);
    assert.equal(telegram.edits.length, 1);
    assert.equal(telegram.edits[0].id, 1);
    assert.equal(telegram.posts.length, 1);
});

test('a deleted live message is replaced', async () => {
    const { b, telegram, state } = setup({ live: true });
    await b.tick(T0);
    telegram.editError = new TelegramError(400, 'Bad Request: message to edit not found');
    await b.tick(T0 + 5 * MIN);
    assert.equal(telegram.posts.length, 2);
    assert.equal(state.liveMessageId, 2);
});

test('a failed post is retried next tick, not skipped', async () => {
    const { b, dashboard, telegram } = setup();
    await b.tick(T0);
    dashboard.b = [block('hh', 107)];
    const realSend = telegram.send.bind(telegram);
    telegram.send = async () => { throw new TelegramError('network', 'ECONNRESET'); };
    await assert.rejects(b.tick(T0 + MIN));
    telegram.send = realSend;
    await b.tick(T0 + 2 * MIN);
    assert.equal(telegram.posts.length, 1);
    assert.match(telegram.posts[0], /107/);
});
