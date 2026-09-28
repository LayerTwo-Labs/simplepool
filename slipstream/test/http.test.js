import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';

import { clientAddress, createHandler } from '../lib/http.js';
import { TXID_A, fakePool, makeSlipstream, quietLog } from './helpers.js';

const HEX = '0200000001' + '00'.repeat(60);

async function serve(cfgOverrides = {}, meta = { pool_mode: 'solo', fee_bps: 0 }) {
    const ctx = makeSlipstream({ cfg: cfgOverrides });
    const pool = fakePool({ meta });
    const server = createServer(createHandler({
        slipstream: ctx.slipstream, pool, cfg: ctx.slipstream.cfg, version: '0.1.0', log: quietLog,
    }));
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const base = `http://127.0.0.1:${server.address().port}`;
    return { ...ctx, base, close: () => new Promise(resolve => server.close(resolve)) };
}

test('info.json is served cross-origin', async () => {
    const { base, close } = await serve({ presentation: { slipstreamUrl: 'https://s.example' } });
    try {
        const res = await fetch(`${base}/info.json`);
        assert.equal(res.status, 200);
        assert.equal(res.headers.get('access-control-allow-origin'), '*');
        const info = await res.json();
        assert.equal(info.mode, 'solo');
        assert.equal(info.slipstream_url, 'https://s.example');
        assert.equal(info.version, '0.1.0');
    } finally {
        await close();
    }
});

test('submit as text or JSON, then read the tx back with its history', async () => {
    const { base, close } = await serve();
    try {
        let res = await fetch(`${base}/api/tx`, { method: 'POST', body: HEX });
        assert.equal(res.status, 200);
        assert.equal((await res.json()).accepted, true);

        res = await fetch(`${base}/tx`, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ hex: HEX }),
        });
        assert.equal((await res.json()).already_tracked, true);

        res = await fetch(`${base}/api/tx/${TXID_A}`);
        const { tx, events } = await res.json();
        assert.equal(tx.status, 'pending');
        assert.equal(tx.raw_hex, undefined);
        assert.deepEqual(events.map(e => e.event), ['accepted']);

        res = await fetch(`${base}/api/txs?status=pending`);
        assert.equal((await res.json()).txs.length, 1);
        assert.equal((await fetch(`${base}/api/txs?status=bogus`)).status, 400);
        assert.equal((await fetch(`${base}/api/tx/${'c'.repeat(64)}`)).status, 404);
    } finally {
        await close();
    }
});

test('fees are reported, and submissions are rate limited per address', async () => {
    const { base, close } = await serve({ rateLimitPerMin: 2 });
    try {
        const fees = await (await fetch(`${base}/api/fees`)).json();
        assert.equal(fees.min_submission_rate, 1);
        assert.equal(fees.required_rate, 1);
        const statuses = [];
        for (let i = 0; i < 3; i++) {
            statuses.push((await fetch(`${base}/api/tx`, { method: 'POST', body: HEX })).status);
        }
        assert.deepEqual(statuses, [200, 200, 429]);
    } finally {
        await close();
    }
});

test('behind a proxy, the client address is the hop the proxy added, not one the client wrote', () => {
    const req = (fwd) => ({ headers: { 'x-forwarded-for': fwd }, socket: { remoteAddress: '127.0.0.1' } });
    assert.equal(clientAddress(req('1.2.3.4, 203.0.113.9'), true), '203.0.113.9');
    assert.equal(clientAddress(req('203.0.113.9'), true), '203.0.113.9');
    assert.equal(clientAddress(req('1.2.3.4'), false), '127.0.0.1');
    assert.equal(clientAddress({ headers: {}, socket: { remoteAddress: '::1' } }, true), '::1');
});
