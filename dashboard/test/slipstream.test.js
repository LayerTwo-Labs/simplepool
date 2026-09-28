/* The slipstream page and its nav link.
 *
 * The page reads the slipstream service's API, never its database, and has
 * to render when the service is down: a pool whose slipstream service fell
 * over should lose one card, not the page.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';
import ejs from 'ejs';

import * as fmt from '../lib/fmt.js';
import { fetchSlipstream } from '../lib/slipstream.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VIEWS = path.resolve(__dirname, '../views');

const TXID = 'ab'.repeat(32);
const FEES = { min_submission_rate: 1, mineable_rate: 3.5, required_rate: 3.5,
               template_height: 101, updated_at: Math.floor(Date.now() / 1000) };
const TXS = [
    { txid: TXID, fee_rate: 7.09, status: 'confirmed', status_reason: null,
      mined_block_hash: 'cd'.repeat(32), mined_height: 100, mined_by_pool: 1,
      submitted_at: Math.floor(Date.now() / 1000) - 600 },
    { txid: 'ef'.repeat(32), fee_rate: 2, status: 'dropped', status_reason: 'conflict_mined',
      mined_block_hash: null, mined_height: null, mined_by_pool: null,
      submitted_at: Math.floor(Date.now() / 1000) - 60 },
];

const render = (view, locals) => ejs.renderFile(path.join(VIEWS, view), {
    ...fmt.all, pool: null, health: null, stratumUrl: 'stratum+tcp://pool.example:3334',
    sidechainId: 9, slipstreamEnabled: true, ...locals,
}, { views: [VIEWS] });

test('the page shows both rates, how to submit, and each tx where it stands', async () => {
    const html = await render('slipstream.ejs', {
        slip: { configured: true, ok: true, fees: FEES, txs: TXS },
        submitUrl: 'https://slipstream.example/',
    });
    assert.match(html, /Minimum submission rate<\/label><strong>1\.00 sat\/vB/);
    assert.match(html, /Current mineable rate<\/label><strong>3\.50 sat\/vB/);
    assert.match(html, /curl -X POST --data-binary "\$RAW_TX_HEX" https:\/\/slipstream\.example\/api\/tx/);
    assert.match(html, /href="https:\/\/slipstream\.example\/info\.json"/);
    assert.match(html, /confirmed/);
    assert.match(html, /\(ours\)/);
    assert.match(html, /dropped<\/span>\s*<span class="muted small">\(conflict_mined\)/);
    assert.match(html, /href="\/slipstream" class="active">Slipstream/);
});

test('a service that is down costs the page its content, not the page', async () => {
    const html = await render('slipstream.ejs', {
        slip: { configured: true, ok: false, fees: null, txs: [], error: 'HTTP 502' },
        submitUrl: null,
    });
    assert.match(html, /not answering right now \(HTTP 502\)/);
    assert.doesNotMatch(html, /Minimum submission rate/);
});

test('the nav links to slipstream only on a pool that runs it', async () => {
    const nav = (slipstreamEnabled) => render('partial/nav-public.ejs', { slipstreamEnabled });
    assert.match(await nav(true), /href="\/slipstream"/);
    assert.doesNotMatch(await nav(false), /href="\/slipstream"/);
});

/* A stub HTTP server answering one handler. */
async function stub(handler) {
    const calls = [];
    const server = createServer(async (req, res) => {
        let body = '';
        for await (const chunk of req) body += chunk;
        calls.push({ url: req.url, body: body ? JSON.parse(body) : null });
        const { status = 200, json } = handler(req.url, body ? JSON.parse(body) : null);
        res.statusCode = status;
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify(json));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    return { url: `http://127.0.0.1:${server.address().port}`, calls,
             close: () => new Promise(resolve => server.close(resolve)) };
}

test('fetchSlipstream reads fees and txs from the service API', async () => {
    const s = await stub((url) => url.startsWith('/api/fees')
        ? { json: FEES }
        : { json: { txs: url.includes('status=pending') ? [TXS[1]] : [TXS[0]] } });
    try {
        const all = await fetchSlipstream(s.url);
        assert.equal(all.ok, true);
        assert.equal(all.fees.mineable_rate, 3.5);
        assert.equal(all.txs[0].txid, TXID);
        const open = await fetchSlipstream(s.url + '/', { statuses: ['pending', 'in_template'] });
        assert.ok(s.calls.some(c => c.url.startsWith('/api/txs?status=in_template')));
        assert.equal(open.txs.length, 2);
    } finally {
        await s.close();
    }
    assert.equal((await fetchSlipstream('')).configured, false);
    const down = await fetchSlipstream('http://127.0.0.1:1');
    assert.equal(down.ok, false);
});
