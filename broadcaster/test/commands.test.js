import { test } from 'node:test';
import assert from 'node:assert/strict';

import { Commands, COMMANDS } from '../lib/commands.js';
import { emptyState } from '../lib/state.js';

const quietLog = { info() {}, warn() {}, error() {} };
const T0 = Date.UTC(2026, 9, 2, 10, 0, 0);
const CHAT = -1001518607784;

function status() {
    return {
        pool: {
            db_ready: true, hashrate: 1e12, hashrate_1h: 2e12, hashrate_5m: 3e12, workers_active: 4,
            accepted: 1000, rejected: 5, reject_rate_pct: 0.5, best_share_24h: 123456, blocks_lifetime: 7, last_share_ts: T0 / 1000 - 5,
        },
    };
}

class FakeTelegram {
    constructor(pending = []) { this.sent = []; this.menus = []; this.pending = pending; this.polls = []; }
    async getMe() { return { username: 'avonpool_bot' }; }
    async setMyCommands(c) { this.menus.push(c); }
    async getUpdates(args) { this.polls.push(args); const p = this.pending; this.pending = []; return p; }
    async send(html, opts) { this.sent.push({ html, opts }); return this.sent.length; }
}

function setup({ pending, dashboardFails = false, state = emptyState() } = {}) {
    const telegram = new FakeTelegram(pending);
    const dashboard = {
        async status() { if (dashboardFails) throw new Error('HTTP 502'); return status(); },
        async blocks() { return [{ hash: 'h', height: 970802, status: 'confirmed', ts: T0 / 1000 - 3600 }]; },
    };
    const cfg = { chatId: String(CHAT), poolName: 'avonpool_beta', publicUrl: null,
                  commandCooldownSec: 30, staleSharesSec: 900 };
    const c = new Commands({ dashboard, telegram, state, cfg, log: quietLog, persist() {} });
    return { c, telegram, state };
}

const cmd = (text, extra = {}) => ({
    message_id: 77, text, chat: { id: CHAT, type: 'supergroup' },
    is_topic_message: true, message_thread_id: 23563, ...extra,
});

test('first start registers the menu and skips the backlog', async () => {
    const { c, telegram, state } = setup({ pending: [{ update_id: 40, message: cmd('/pool_status') }] });
    await c.init();
    assert.deepEqual(telegram.menus, [COMMANDS]);
    assert.deepEqual(telegram.polls[0], { offset: -1, timeoutSec: 0 });
    assert.equal(state.updateOffset, 41);
    assert.equal(telegram.sent.length, 0);
});

test('a restart keeps its offset instead of skipping again', async () => {
    const { c, telegram } = setup({ state: { ...emptyState(), updateOffset: 12 } });
    await c.init();
    assert.equal(telegram.polls.length, 0);
});

test('/pool_status is answered in its topic, as a reply, with the last block', async () => {
    const { c, telegram, state } = setup({ state: { ...emptyState(), updateOffset: 5 } });
    await c.init();
    telegram.pending = [{ update_id: 5, message: cmd('/pool_status@avonpool_bot') }];
    await c.pollOnce(() => T0);
    assert.equal(state.updateOffset, 6);
    assert.equal(telegram.sent.length, 1);
    const { html, opts } = telegram.sent[0];
    assert.deepEqual(opts, { chatId: CHAT, threadId: 23563, replyTo: 77 });
    assert.match(html, /avonpool_beta — status/);
    assert.match(html, /Hashrate now: <b>3\.00 TH\/s<\/b> \(5m\)/);
    assert.match(html, /1,000 accepted · 5 rejected \(0\.50%\)/);
    assert.match(html, /Last block: <b>#970,802<\/b> · 1h 0m ago/);
    assert.match(html, /^🟢/);
});

test('General topic: no thread id', async () => {
    const { c, telegram } = setup();
    await c.init();
    await c.handle(cmd('/pool_status', { is_topic_message: undefined, message_thread_id: undefined }), T0);
    assert.equal(telegram.sent[0].opts.threadId, null);
});

test('other chats, other bots and other text are ignored', async () => {
    const { c, telegram } = setup();
    await c.init();
    await c.handle(cmd('/pool_status', { chat: { id: -100999 } }), T0);
    await c.handle(cmd('/pool_status@some_other_bot'), T0);
    await c.handle(cmd('/pool_statusx'), T0);
    await c.handle(cmd('what is the pool_status'), T0);
    await c.handle(cmd(undefined), T0);
    assert.equal(telegram.sent.length, 0);
});

test('a public chat configured by @username matches', async () => {
    const { c, telegram } = setup();
    c.cfg.chatId = '@AvonPool';
    await c.init();
    await c.handle(cmd('/pool_status', { chat: { id: -1002, username: 'avonpool' } }), T0);
    assert.equal(telegram.sent.length, 1);
});

test('one reply per chat per cooldown', async () => {
    const { c, telegram } = setup();
    await c.init();
    await c.handle(cmd('/pool_status'), T0);
    await c.handle(cmd('/pool_status'), T0 + 10_000);
    assert.equal(telegram.sent.length, 1);
    await c.handle(cmd('/pool_status'), T0 + 31_000);
    assert.equal(telegram.sent.length, 2);
});

test('a dashboard failure is answered, not swallowed', async () => {
    const { c, telegram } = setup({ dashboardFails: true });
    await c.init();
    await c.handle(cmd('/pool_status'), T0);
    assert.match(telegram.sent[0].html, /can't be read right now/);
});
