import express from 'express';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderFile } from 'ejs';
import { openDb } from './lib/db.js';
import * as stats from './lib/stats.js';
import * as admin from './lib/admin.js';        /* PPS ADMIN PATCH */

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PORT = parseInt(process.env.PORT || '8081', 10);
// Default to the live SQLite database. WAL mode + readonly handle is safe
// to point at the file the proxy is actively writing to. The snapshot path
// (data/shares.snapshot.db) is still supported for users who run the cron
// `.backup` job documented in the README — set PROXY_DB_PATH to override.
const DB_PATH = process.env.PROXY_DB_PATH || '../data/shares.db';

const app = express();
app.engine('ejs', renderFile);
app.set('views', path.join(__dirname, 'views'));
app.set('view engine', 'ejs');
app.disable('x-powered-by');
app.use('/static', express.static(path.join(__dirname, 'public'), { maxAge: '1h' }));

const db = openDb(path.resolve(__dirname, DB_PATH));

/* Shown on the public "Connect a miner" card. Env-driven so the shipped
 * template doesn't hardcode any particular deployment's host. */
const PUBLIC_STRATUM_URL = process.env.PUBLIC_STRATUM_URL || 'stratum+tcp://<pool-host>:3334';
/* Read once here so the public per-worker page can render the audit
 * cross-check for the miner without needing admin auth. The admin
 * patch block below also reads this — the constant is shared. */
const PPS_SATS_PER_DIFF = parseFloat(process.env.POOL_PPS_SATS_PER_DIFF || '1000');

app.get('/', (req, res) => {
    const ov = stats.overview(db);
    const lb = stats.leaderboard(db);
    const lbAddr = stats.leaderboardByAddress(db);
    const blocks = stats.recentBlocks(db, 5);
    const node = stats.nodeStatus(db);
    res.render('index', {
        ov, lb, lbAddr, blocks, node,
        stratumUrl: PUBLIC_STRATUM_URL,
        fmtHashrate: stats.fmtHashrate,
        fmtBtc: stats.fmtBtc,
        fmtPct: stats.fmtPct,
    });
});

app.get('/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name, 86400, PPS_SATS_PER_DIFF);
    if (!w.worker) return res.status(404).render('404', { what: 'worker' });
    res.render('worker', {
        ...w, name: req.params.name,
        fmtHashrate: stats.fmtHashrate,
        fmtPct: stats.fmtPct,
    });
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

app.get('/api/overview', (req, res) => res.json(stats.overview(db)));
app.get('/api/node', (req, res) => res.json(stats.nodeStatus(db) || {}));
app.get('/api/leaderboard', (req, res) => res.json(stats.leaderboard(db)));
app.get('/api/leaderboard/by-address', (req, res) => res.json(stats.leaderboardByAddress(db)));
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
app.get('/healthz', (req, res) => res.json({ ok: true, db_ready: db.ready() }));


/* PPS ADMIN PATCH — pool operator view. Basic auth via ADMIN_USER /
 * ADMIN_PASSWORD env vars; both required or the routes 503 (so we
 * fail closed if someone forgets to set them). */
const ADMIN_USER = process.env.ADMIN_USER || '';
const ADMIN_PASS = process.env.ADMIN_PASSWORD || '';
const RESERVE_ADDRESS = process.env.POOL_THUNDER_RESERVE_ADDRESS || '(unset)';
const THUNDER_RPC_URL = process.env.THUNDER_RPC_URL || 'http://127.0.0.1:6009';
/* PPS_SATS_PER_DIFF is declared at the top of the file so both the
 * public /worker/:name view and the admin routes can share it. */

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

async function adminSummary() {
    const [reserve, totals, workers, inFlight, payouts, deposits, blocks] = await Promise.all([
        admin.thunderBalance(THUNDER_RPC_URL),
        Promise.resolve(admin.poolTotals(db)),
        Promise.resolve(admin.perWorkerBalances(db)),
        Promise.resolve(admin.inFlight(db)),
        Promise.resolve(admin.recentPayouts(db, 25)),
        Promise.resolve(admin.recentDeposits(db, 25)),
        Promise.resolve(admin.recentBlocksFound(db, 15)),
    ]);
    return {
        reserve, reserveAddress: RESERVE_ADDRESS,
        totals, workers, inFlight,
        payouts, deposits, blocks,
    };
}

app.get('/admin', requireAdminAuth, async (req, res) => {
    try {
        const data = await adminSummary();
        res.render('admin', data);
    } catch (e) {
        res.status(500).send('admin: ' + e.message);
    }
});

app.get('/api/admin/summary', requireAdminAuth, async (req, res) => {
    try {
        res.json(await adminSummary());
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});

/* Per-worker audit: "why is my balance N sats?" answered end-to-end. */
function workerAuditFor(workerId) {
    const audit = admin.workerAudit(db, workerId, { rate: PPS_SATS_PER_DIFF });
    if (!audit) return null;
    audit.payouts = admin.payoutsForWorker(db, workerId, 100);
    return audit;
}

app.get('/admin/worker/:id', requireAdminAuth, (req, res) => {
    try {
        const audit = workerAuditFor(parseInt(req.params.id, 10));
        if (!audit) return res.status(404).render('404', { what: 'worker' });
        res.render('admin-worker', { audit });
    } catch (e) {
        res.status(500).send('admin: ' + e.message);
    }
});

app.get('/api/admin/worker/:id', requireAdminAuth, (req, res) => {
    try {
        const audit = workerAuditFor(parseInt(req.params.id, 10));
        if (!audit) return res.status(404).json({ error: 'unknown worker id' });
        res.json(audit);
    } catch (e) {
        res.status(500).json({ error: e.message });
    }
});
/* END PPS ADMIN PATCH */

app.use((req, res) => res.status(404).render('404', { what: 'page' }));

app.listen(PORT, () => {
    console.log(`simplepool dashboard on :${PORT} (db: ${db.path})`);
});
