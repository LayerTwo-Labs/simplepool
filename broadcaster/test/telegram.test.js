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
