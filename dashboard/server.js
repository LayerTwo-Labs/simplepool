import express from 'express';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderFile } from 'ejs';
import { openDb } from './lib/db.js';
import * as stats from './lib/stats.js';

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

app.get('/', (req, res) => {
    const ov = stats.overview(db);
    const lb = stats.leaderboard(db);
    const lbAddr = stats.leaderboardByAddress(db);
    const blocks = stats.recentBlocks(db, 5);
    res.render('index', {
        ov, lb, lbAddr, blocks,
        fmtHashrate: stats.fmtHashrate,
        fmtBtc: stats.fmtBtc,
    });
});

app.get('/worker/:name', (req, res) => {
    const w = stats.worker(db, req.params.name);
    if (!w.worker) return res.status(404).render('404', { what: 'worker' });
    res.render('worker', { ...w, name: req.params.name, fmtHashrate: stats.fmtHashrate });
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

app.use((req, res) => res.status(404).render('404', { what: 'page' }));

app.listen(PORT, () => {
    console.log(`simplepool dashboard on :${PORT} (db: ${db.path})`);
});
