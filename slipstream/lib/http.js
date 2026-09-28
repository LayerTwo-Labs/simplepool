/* The public HTTP surface.
 *
 *   GET  /info.json          the pool, for pool directories (also /api/info)
 *   GET  /api/fees           minimum submission rate and current mineable rate
 *   POST /api/tx             submit a raw tx, as hex: a text/plain body, or
 *                            JSON {"hex": "..."} (also POST /tx)
 *   GET  /api/tx/:txid       where a tx stands, with its full history
 *   GET  /api/txs            recent txs; ?status= and ?limit= (max 500)
 *   GET  /healthz
 *
 * Plain node:http, like the payout worker's admin surface: a handful of
 * routes does not need a framework. Everything is readable cross-origin,
 * since pool directories fetch info.json from their own pages.
 */

import { createServer } from 'node:http';

import { buildInfo } from './info.js';
import { summary } from './slipstream.js';

/* Hex doubles the size, and JSON wraps it. */
const MAX_BODY_BYTES = 2 * 1_000_000 + 1024;

const TXID_RE = /^[0-9a-f]{64}$/;
const STATUSES = ['pending', 'in_template', 'mined', 'confirmed', 'dropped'];

/* Fixed-window per-address counter. Crude, and enough: a submission costs
 * the enforcer a node round trip, so the point is only to bound that. */
export function rateLimiter(perMinute) {
    const windows = new Map();
    return (key) => {
        const minute = Math.floor(Date.now() / 60_000);
        const w = windows.get(key);
        if (!w || w.minute !== minute) {
            windows.set(key, { minute, count: 1 });
            if (windows.size > 10_000) {
                for (const [k, v] of windows) if (v.minute !== minute) windows.delete(k);
            }
            return true;
        }
        w.count += 1;
        return w.count <= perMinute;
    };
}

/* Behind a proxy, the LAST X-Forwarded-For entry: the one the proxy itself
 * added. Anything before it came from the client, which can write whatever
 * it likes there -- taking the first entry would let a client pick its own
 * rate-limit key. */
export function clientAddress(req, trustProxy) {
    if (trustProxy) {
        const fwd = req.headers['x-forwarded-for'];
        if (typeof fwd === 'string' && fwd.length > 0) {
            const hops = fwd.split(',').map(h => h.trim()).filter(Boolean);
            if (hops.length > 0) return hops[hops.length - 1];
        }
    }
    return req.socket.remoteAddress ?? 'unknown';
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        let size = 0;
        const chunks = [];
        req.on('data', (chunk) => {
            size += chunk.length;
            if (size > MAX_BODY_BYTES) {
                reject(Object.assign(new Error('body too large'), { tooLarge: true }));
                req.destroy();
                return;
            }
            chunks.push(chunk);
        });
        req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
        req.on('error', reject);
    });
}

function txHexFrom(body, contentType) {
    if ((contentType ?? '').includes('application/json')) {
        const parsed = JSON.parse(body);
        return typeof parsed === 'string' ? parsed : (parsed?.hex ?? parsed?.tx ?? '');
    }
    return body;
}

export function createHandler({ slipstream, pool, cfg, version, log }) {
    const allow = rateLimiter(cfg.rateLimitPerMin);
    return async (req, res) => {
        const send = (status, body) => {
            res.statusCode = status;
            res.setHeader('content-type', 'application/json');
            res.setHeader('access-control-allow-origin', '*');
            res.setHeader('cache-control', 'no-store');
            res.end(JSON.stringify(body));
        };
        const url = new URL(req.url, 'http://localhost');
        const path = url.pathname.replace(/\/+$/, '') || '/';
        try {
            if (req.method === 'OPTIONS') {
                res.statusCode = 204;
                res.setHeader('access-control-allow-origin', '*');
                res.setHeader('access-control-allow-methods', 'GET, POST, OPTIONS');
                res.setHeader('access-control-allow-headers', 'content-type');
                return res.end();
            }
            if (req.method === 'GET' && (path === '/info.json' || path === '/api/info')) {
                res.setHeader('cache-control', 'public, max-age=60');
                const body = buildInfo(pool.meta(), cfg.presentation, { version });
                res.statusCode = 200;
                res.setHeader('content-type', 'application/json');
                res.setHeader('access-control-allow-origin', '*');
                return res.end(JSON.stringify(body, null, 2));
            }
            if (req.method === 'GET' && path === '/api/fees') {
                return send(200, slipstream.fees());
            }
            if (req.method === 'POST' && (path === '/api/tx' || path === '/tx')) {
                const who = clientAddress(req, cfg.trustProxy);
                if (!allow(who)) return send(429, { accepted: false, reject_reason: 'rate-limited' });
                let hex;
                try {
                    hex = txHexFrom(await readBody(req), req.headers['content-type']);
                } catch (e) {
                    if (e.tooLarge) return send(413, { accepted: false, reject_reason: 'tx-size' });
                    return send(400, { accepted: false, reject_reason: 'invalid-body' });
                }
                const { httpStatus, body } = await slipstream.submit(hex, who);
                return send(httpStatus, body);
            }
            const txMatch = path.match(/^\/(?:api\/)?tx\/([0-9a-fA-F]+)$/);
            if (req.method === 'GET' && txMatch) {
                const txid = txMatch[1].toLowerCase();
                if (!TXID_RE.test(txid)) return send(400, { error: 'invalid txid' });
                const row = slipstream.store.get(txid);
                if (!row) return send(404, { error: 'unknown txid' });
                return send(200, { tx: summary(row), events: slipstream.store.events(txid) });
            }
            if (req.method === 'GET' && path === '/api/txs') {
                const status = url.searchParams.get('status');
                if (status && !STATUSES.includes(status)) {
                    return send(400, { error: `status must be one of ${STATUSES.join(', ')}` });
                }
                const limit = Math.min(Math.max(Number(url.searchParams.get('limit')) || 50, 1), 500);
                return send(200, { txs: slipstream.store.recent({ status, limit }) });
            }
            if (req.method === 'GET' && path === '/healthz') {
                const age = slipstream.templateAt === null
                    ? null : Math.floor(Date.now() / 1000) - slipstream.templateAt;
                const ok = slipstream.enforcerError === null && age !== null;
                return send(ok ? 200 : 503, {
                    ok, template_age_s: age, enforcer_error: slipstream.enforcerError,
                });
            }
            if (req.method === 'GET' && path === '/') {
                return send(200, {
                    service: 'simplepool slipstream',
                    endpoints: ['GET /info.json', 'GET /api/fees', 'POST /api/tx',
                                'GET /api/tx/:txid', 'GET /api/txs', 'GET /healthz'],
                });
            }
            return send(404, { error: 'not found' });
        } catch (e) {
            log.error(`${req.method} ${path}: ${e.stack ?? e.message}`);
            return send(500, { error: 'internal error' });
        }
    };
}

export function startHttp(opts) {
    const { cfg, log } = opts;
    const server = createServer(createHandler(opts));
    server.listen(cfg.port, cfg.bind, () => {
        log.info(`slipstream http listening on ${cfg.bind}:${cfg.port}`);
    });
    return server;
}
