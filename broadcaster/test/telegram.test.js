import { test } from 'node:test';
import assert from 'node:assert/strict';

import { TelegramClient, TelegramError } from '../lib/telegram.js';

function fakeFetch(responses) {
    const calls = [];
    const fn = async (url, init) => {
        calls.push({ url, body: JSON.parse(init.body) });
        const [status, json] = responses.shift();
        return { status, json: async () => json };
    };
    fn.calls = calls;
    return fn;
}

const noSleep = async () => {};

test('send posts HTML to the chat and returns the message id', async () => {
    const f = fakeFetch([[200, { ok: true, result: { message_id: 42 } }]]);
    const t = new TelegramClient({ token: 'SECRET', chatId: '@chan', fetchImpl: f, sleep: noSleep, minGapMs: 0 });
    assert.equal(await t.send('<b>hi</b>'), 42);
    assert.match(f.calls[0].url, /\/botSECRET\/sendMessage$/);
    assert.equal(f.calls[0].body.chat_id, '@chan');
    assert.equal(f.calls[0].body.parse_mode, 'HTML');
});

test('send names the topic only when one is set; edit and pin never do', async () => {
    const ok = (id) => [200, { ok: true, result: { message_id: id } }];
    const f = fakeFetch([ok(1), ok(2), [200, { ok: true, result: true }], [200, { ok: true, result: true }]]);
    const t = new TelegramClient({ token: 'x', chatId: -1001518607784, threadId: 23563, fetchImpl: f, sleep: noSleep, minGapMs: 0 });
    await t.send('a');
    await t.edit(1, 'b');
    await t.pin(1);
    assert.equal(f.calls[0].body.message_thread_id, 23563);
    assert.equal(f.calls[1].body.message_thread_id, undefined);
    assert.equal(f.calls[2].body.message_thread_id, undefined);

    const g = fakeFetch([ok(3)]);
    await new TelegramClient({ token: 'x', chatId: 'c', fetchImpl: g, sleep: noSleep, minGapMs: 0 }).send('c');
    assert.equal('message_thread_id' in g.calls[0].body, false);
});

test('429 waits retry_after and tries again', async () => {
    const slept = [];
    const f = fakeFetch([
        [429, { ok: false, error_code: 429, description: 'Too Many Requests', parameters: { retry_after: 7 } }],
        [200, { ok: true, result: { message_id: 1 } }],
    ]);
    const t = new TelegramClient({ token: 'x', chatId: 'c', fetchImpl: f, sleep: async (ms) => slept.push(ms), minGapMs: 0 });
    await t.send('a');
    assert.equal(f.calls.length, 2);
    assert.ok(slept.includes(7000));
});

test('errors carry Telegram\'s description but never the token', async () => {
    const f = fakeFetch([[403, { ok: false, error_code: 403, description: 'Forbidden: bot is not a member' }]]);
    const t = new TelegramClient({ token: 'SECRET', chatId: 'c', fetchImpl: f, sleep: noSleep, minGapMs: 0 });
    const err = await t.send('a').catch((e) => e);
    assert.ok(err instanceof TelegramError);
    assert.equal(err.code, 403);
    assert.doesNotMatch(err.message, /SECRET/);

    const t2 = new TelegramClient({
        token: 'SECRET', chatId: 'c', sleep: noSleep, minGapMs: 0,
        fetchImpl: async (url) => { throw new TypeError(`fetch failed for ${url}`); },
    });
    const err2 = await t2.send('a').catch((e) => e);
    assert.doesNotMatch(err2.message, /SECRET/);
});

test('"message is not modified" on edit is not an error', async () => {
    const f = fakeFetch([[400, { ok: false, error_code: 400, description: 'Bad Request: message is not modified' }]]);
    const t = new TelegramClient({ token: 'x', chatId: 'c', fetchImpl: f, sleep: noSleep, minGapMs: 0 });
    await t.edit(1, 'same');
});

test('calls are spaced minGapMs apart', async () => {
    const slept = [];
    const f = fakeFetch([
        [200, { ok: true, result: { message_id: 1 } }],
        [200, { ok: true, result: { message_id: 2 } }],
    ]);
    const t = new TelegramClient({ token: 'x', chatId: 'c', fetchImpl: f, sleep: async (ms) => slept.push(ms), minGapMs: 3000 });
    await Promise.all([t.send('a'), t.send('b')]);
    assert.equal(slept.length, 1);
    assert.ok(slept[0] > 2900 && slept[0] <= 3000);
});

test('a command reply overrides chat and topic and quotes the command', async () => {
    const f = fakeFetch([[200, { ok: true, result: { message_id: 9 } }]]);
    const t = new TelegramClient({ token: 'x', chatId: 'c', threadId: 1, fetchImpl: f, sleep: noSleep, minGapMs: 0 });
    await t.send('a', { chatId: -100, threadId: null, replyTo: 77 });
    const body = f.calls[0].body;
    assert.equal(body.chat_id, -100);
    assert.equal('message_thread_id' in body, false);
    assert.deepEqual(body.reply_parameters, { message_id: 77, allow_sending_without_reply: true });
});

test('getUpdates is not queued behind posts and asks only for messages', async () => {
    let release;
    const gate = new Promise((r) => { release = r; });
    const calls = [];
    let updatesBody;
    const fetchImpl = async (url, init) => {
        calls.push(url.split('/').pop());
        if (url.endsWith('getUpdates')) updatesBody = JSON.parse(init.body);
        if (url.endsWith('sendMessage')) await gate;
        return { status: 200, json: async () => ({ ok: true, result: url.endsWith('getUpdates') ? [] : { message_id: 1 } }) };
    };
    const t = new TelegramClient({ token: 'x', chatId: 'c', fetchImpl, sleep: noSleep, minGapMs: 0 });
    const posting = t.send('a');
    await new Promise((r) => setImmediate(r));
    assert.deepEqual(await t.getUpdates({ offset: 3, timeoutSec: 0 }), []);
    assert.deepEqual(calls, ['sendMessage', 'getUpdates']);
    assert.deepEqual(updatesBody.allowed_updates, ['message']);
    release();
    await posting;
});
