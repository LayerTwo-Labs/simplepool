/* simplepool dashboard — two surfaces, one process:
 *
 *   - Public read-only:  /, /worker/:name, /blocks, /api/*
 *   - Admin:             /admin/* (Basic auth on every route)
 *
 * Admin routes live in lib/admin-router.js; server.js is the assembly:
 * open DB handles, mount routes, wire middleware.
 */

import express from 'express';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderFile } from 'ejs';
import { openDb } from './lib/db.js';
import { openAdminDb } from './lib/db-admin.js';
import * as stats from './lib/stats.js';
import * as fmt from './lib/fmt.js';
import { createAdminRouter } from './lib/admin-router.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT     = parseInt(process.env.PORT || '8081', 10);
const DB_PATH  = process.env.PROXY_DB_PATH || '../data/shares.db';

const app = express();
app.engine('ejs', renderFile);
app.set('views', path.join(__dirname, 'views'));
app.set('view engine', 'ejs');
app.disable('x-powered-by');

/* --- security headers on every response --------------------------------- */
app.use((_req, res, next) => {
    /* No cookies here yet, but if any land later they must be same-origin. */
    res.setHeader('X-Content-Type-Options',  'nosniff');
    res.setHeader('X-Frame-Options',         'DENY');
    res.setHeader('Referrer-Policy',         'same-origin');
    res.setHeader('Permissions-Policy',      'interest-cohort=()');
    next();
});

/* --- fmt helpers on every render ---------------------------------------- */
app.use((_req, res, next) => {
    Object.assign(res.locals, fmt.all);
    next();
});

app.use('/static', express.static(path.join(__dirname, 'public'), { maxAge: '1h' }));

/* Two handles: read-only for public + admin read routes, writable one
 * confined to admin write actions (lives inside admin router). */
const db   = openDb(path.resolve(__dirname, DB_PATH));
const dbRw = openAdminDb(path.resolve(__dirname, DB_PATH));

/* --- public-side config ------------------------------------------------- */
const PUBLIC_STRATUM_URL = process.env.PUBLIC_STRATUM_URL || 'stratum+tcp://<pool-host>:3334';
const PPS_SATS_PER_DIFF  = parseFloat(process.env.POOL_PPS_SATS_PER_DIFF || '1000');

/* --- admin-side config -------------------------------------------------- */
const ADMIN_USER           = process.env.ADMIN_USER     || '';
const ADMIN_PASS           = process.env.ADMIN_PASSWORD || '';
const RESERVE_ADDRESS      = process.env.POOL_THUNDER_RESERVE_ADDRESS || '(unset)';
const THUNDER_RPC_URL      = process.env.THUNDER_RPC_URL      || 'http://127.0.0.1:6009';
const PAYOUT_ADMIN_URL     = process.env.PAYOUT_ADMIN_URL     || '';
const ENFORCER_GRPC_ADDR   = process.env.ENFORCER_GRPC_ADDR   || '127.0.0.1:50051';
const GRPCURL_BIN          = process.env.GRPCURL_BIN          || 'grpcurl';
const THUNDER_SIDECHAIN_ID = parseInt(process.env.THUNDER_SIDECHAIN_ID || '9', 10);

function requireAdminAuth(req, res, next) {
    if (!ADMIN_USER || !ADMIN_PASS) {
        return res.status(503).send('admin disabled — set ADMIN_USER + ADMIN_PASSWORD');
    }
    const h = req.headers.authorization || '';
    if (h.startsWith('Basic ')) {
        const [u, p] = Buffer.from(h.slice(6), 'base64').toString('utf8').split(':');
        if (u === ADMIN_USER && p === ADMIN_PASS) return next();
    }
    res.setHeader('WWW-Authenticate', 'Basic realm="simplepool admin"');
    return res.status(401).send('unauthorised');
}

/* ================================ PUBLIC ================================ */

app.get('/', (_req, res) => {
    const ov     = stats.overview(db);
    const lb     = stats.leaderboard(db);
    const lbAddr = stats.leaderboardByAddress(db);
    const blocks = stats.recentBlocks(db, 5);
    const node   = stats.nodeStatus(db);
    res.render('index', {
        ov, lb, lbAddr, blocks, node,
        stratumUrl:  PUBLIC_STRATUM_URL,
        fmtHashrate: stats.fmtHashrate,
        fmtBtc:      stats.fmtBtc,
    });
});

app.get('/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name, 86400, PPS_SATS_PER_DIFF);
    if (!w.worker) return res.status(404).render('404', { what: 'worker' });
    res.render('worker', {
        ...w, name: req.params.name,
        fmtHashrate: stats.fmtHashrate,
    });
});

/* Search box on the public nav points here (GET /worker-lookup?name=…).
 * Strip whitespace, cap length, redirect to the canonical URL — or 404
 * quietly if empty. */
app.get('/worker-lookup', (req, res) => {
    const raw = typeof req.query.name === 'string' ? req.query.name.trim() : '';
    if (!raw) return res.redirect(302, '/');
    const name = raw.slice(0, 200);
    return res.redirect(302, '/worker/' + encodeURIComponent(name));
});

app.get('/blocks', (req, res) => {
    const beforeTs = req.query.before ? Number(req.query.before) : null;
    const page = stats.allBlocks(db, { limit: 50, beforeTs });
    res.render('blocks', {
        rows: page.rows,
        next_before: page.next_before,
        fmtBtc: stats.fmtBtc,
    });
});

/* --- JSON API (unchanged) ---------------------------------------------- */
app.get('/api/overview',              (_req, res) => res.json(stats.overview(db)));
app.get('/api/node',                  (_req, res) => res.json(stats.nodeStatus(db) || {}));
app.get('/api/leaderboard',           (_req, res) => res.json(stats.leaderboard(db)));
app.get('/api/leaderboard/by-address',(_req, res) => res.json(stats.leaderboardByAddress(db)));
app.get('/api/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name);
    if (!w.worker) return res.status(404).json({ error: 'unknown worker' });
    res.json(w);
});
app.get('/api/blocks', (req, res) => {
    const beforeTs = req.query.before ? Number(req.query.before) : null;
    const limit = req.query.limit ? Number(req.query.limit) : 50;
    res.json(stats.allBlocks(db, { limit, beforeTs }));
});
app.get('/healthz', (_req, res) => res.json({ ok: true, db_ready: db.ready() }));

/* ================================ ADMIN ================================ */

app.use('/admin',
    requireAdminAuth,
    createAdminRouter({
        db,
        dbRw,
        THUNDER_RPC_URL,
        PAYOUT_ADMIN_URL,
        ENFORCER_GRPC_ADDR,
        GRPCURL_BIN,
        THUNDER_SIDECHAIN_ID,
        RESERVE_ADDRESS,
        PPS_SATS_PER_DIFF,
    }));

/* ================================ 404 =================================== */

app.use((_req, res) => res.status(404).render('404', { what: 'page' }));

app.listen(PORT, () => {
    console.log(`simplepool dashboard on :${PORT} (db: ${db.path})`);
});
