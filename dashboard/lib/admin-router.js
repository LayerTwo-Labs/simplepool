/* Admin router: the 5 sub-pages + the 4 write-action POSTs + the
 * per-worker audit page + /admin/logout.
 *
 * Every route on this router is protected by requireAdminAuth (mounted
 * in server.js). Writable DB access is confined to the deposit action.
 * All page renders share a single adminSummary() probe to keep the
 * chrome (balances, health chips) consistent across tabs.
 */

import express from 'express';
import * as admin   from './admin.js';
import * as actions from './actions.js';
import { issueToken, consumeToken } from './csrf.js';

export function createAdminRouter({
    db,                     // read-only handle
    dbRw,                   // writable handle (lazy wrapper)
    THUNDER_RPC_URL,
    PAYOUT_ADMIN_URL,
    ENFORCER_GRPC_ADDR,
    GRPCURL_BIN,
    THUNDER_SIDECHAIN_ID,
    RESERVE_ADDRESS,
    PPS_SATS_PER_DIFF,
} = {}) {
    const router = express.Router();
    const parseAdminForm = express.urlencoded({ extended: false, limit: '4kb' });

    /* --- shared probe: called once per admin page render --- */
    async function adminSummary() {
        const [reserve, enforcer, totals, workers, inFlight, payouts, deposits, blocks] =
            await Promise.all([
                admin.thunderBalance(THUNDER_RPC_URL),
                admin.enforcerBalance(GRPCURL_BIN, ENFORCER_GRPC_ADDR),
                Promise.resolve(admin.poolTotals(db)),
                Promise.resolve(admin.perWorkerBalances(db)),
                Promise.resolve(admin.inFlight(db)),
                Promise.resolve(admin.recentPayouts(db, 25)),
                Promise.resolve(admin.recentDeposits(db, 25)),
                Promise.resolve(admin.recentBlocksFound(db, 15)),
            ]);
        return {
            reserve, enforcer,
            reserveAddress:        RESERVE_ADDRESS,
            reserveSidechainId:    THUNDER_SIDECHAIN_ID,
            payoutAdminConfigured: !!PAYOUT_ADMIN_URL,
            totals, workers, inFlight, payouts, deposits, blocks,
        };
    }

    /* --- flash: read from query string on GET, write via redirect on POST --- */
    function readFlash(req) {
        const msg = typeof req.query.flash === 'string' ? req.query.flash : '';
        if (!msg) return null;
        return {
            ok:     req.query.flashOk === '1',
            msg,
            detail: typeof req.query.flashDetail === 'string' ? req.query.flashDetail : '',
        };
    }
    function flashRedirect(res, result, defaultTarget = '/admin') {
        const q = new URLSearchParams();
        q.set('flash',   result.msg    || (result.ok ? 'ok' : 'failed'));
        q.set('flashOk', result.ok ? '1' : '0');
        if (result.detail) q.set('flashDetail', result.detail);
        res.redirect(302, defaultTarget + '?' + q.toString());
    }
    /* Actions honour a hidden `return_to` form field so each POST goes
     * back to the page it was fired from. Whitelist against a small set
     * of admin routes so a bad actor can't redirect to elsewhere. */
    const RETURN_TO_ALLOW = new Set([
        '/admin', '/admin/overview',
        '/admin/workers', '/admin/deposits', '/admin/payouts', '/admin/tools',
    ]);
    function returnTarget(req) {
        const t = req.body?.return_to;
        if (typeof t === 'string' && RETURN_TO_ALLOW.has(t)) return t;
        return '/admin';
    }

    /* --- page helpers --- */
    async function renderAdminPage(view, req, res) {
        const summary = await adminSummary();
        res.render(view, {
            ...summary,
            csrfToken: issueToken(),
            flash:     readFlash(req),
        });
    }

    /* --- CSRF gate for POST actions --- */
    function requireCsrf(req, res, next) {
        if (consumeToken(req.body?.csrf)) return next();
        flashRedirect(res,
            { ok: false, msg: 'CSRF token missing or already used',
              detail: 'refresh the admin page and retry' },
            returnTarget(req));
    }

    /* --- GET page routes --- */
    router.get('/',          (req, res, next) => renderAdminPage('admin-overview', req, res).catch(next));
    router.get('/overview',  (req, res)       => res.redirect(301, '/admin'));
    router.get('/workers',   (req, res, next) => renderAdminPage('admin-workers',  req, res).catch(next));
    router.get('/deposits',  (req, res, next) => renderAdminPage('admin-deposits', req, res).catch(next));
    router.get('/payouts',   (req, res, next) => renderAdminPage('admin-payouts',  req, res).catch(next));
    router.get('/tools',     (req, res, next) => renderAdminPage('admin-tools',    req, res).catch(next));

    /* --- Per-worker audit (unchanged surface, uses admin.workerAudit) --- */
    function workerAuditFor(workerId) {
        const audit = admin.workerAudit(db, workerId, { rate: PPS_SATS_PER_DIFF });
        if (!audit) return null;
        audit.payouts = admin.payoutsForWorker(db, workerId, 100);
        return audit;
    }
    router.get('/worker/:id', (req, res) => {
        try {
            const audit = workerAuditFor(parseInt(req.params.id, 10));
            if (!audit) return res.status(404).render('404', { what: 'worker' });
            res.render('admin-worker', { audit });
        } catch (e) {
            res.status(500).send('admin: ' + e.message);
        }
    });

    /* --- JSON API kept for scripted consumers --- */
    router.get('/api/summary', async (req, res) => {
        try { res.json(await adminSummary()); }
        catch (e) { res.status(500).json({ error: e.message }); }
    });
    router.get('/api/worker/:id', (req, res) => {
        try {
            const audit = workerAuditFor(parseInt(req.params.id, 10));
            if (!audit) return res.status(404).json({ error: 'unknown worker id' });
            res.json(audit);
        } catch (e) {
            res.status(500).json({ error: e.message });
        }
    });

    /* --- POST write actions --- */
    router.post('/action/nudge-mine', parseAdminForm, requireCsrf, async (req, res) => {
        const r = await actions.nudgeMine({ thunderRpcUrl: THUNDER_RPC_URL });
        flashRedirect(res, r, returnTarget(req));
    });

    router.post('/action/remove-from-mempool', parseAdminForm, requireCsrf, async (req, res) => {
        const r = await actions.removeFromMempool({
            thunderRpcUrl: THUNDER_RPC_URL,
            txid: (req.body?.txid || '').trim(),
        });
        flashRedirect(res, r, returnTarget(req));
    });

    router.post('/action/trigger-payout', parseAdminForm, requireCsrf, async (req, res) => {
        const r = await actions.triggerPayout({ payoutAdminUrl: PAYOUT_ADMIN_URL });
        flashRedirect(res, r, returnTarget(req));
    });

    router.post('/action/deposit', parseAdminForm, requireCsrf, async (req, res) => {
        const r = await actions.createDeposit({
            grpcurlBin:       GRPCURL_BIN,
            enforcerGrpcAddr: ENFORCER_GRPC_ADDR,
            sidechainId:      req.body?.sidechain_id ?? THUNDER_SIDECHAIN_ID,
            address:          (req.body?.address || '').trim(),
            valueSats:        req.body?.value_sats,
            feeSats:          req.body?.fee_sats,
            db:               dbRw?.get() || null,
        });
        flashRedirect(res, r, returnTarget(req));
    });

    /* --- /admin/logout: 401 clears the browser's cached Basic creds ---
     * Realm string must be ASCII (HTTP headers) — no em dashes here. */
    router.get('/logout', (_req, res) => {
        res.setHeader('WWW-Authenticate', 'Basic realm="simplepool admin - signed out"');
        res.status(401).send(
            '<!doctype html><meta charset="utf-8">' +
            '<title>signed out · simplepool</title>' +
            '<link rel="stylesheet" href="/static/style.css">' +
            '<main class="wrap"><section class="card">' +
            '<h2>Signed out</h2>' +
            '<p>Browser Basic-auth cache is cleared. ' +
            '<a href="/admin">Sign back in</a> or ' +
            '<a href="/">head to the public dashboard</a>.</p>' +
            '</section></main>');
    });

    /* --- central error handler for /admin/* — never leak stack traces --- */
    router.use((err, _req, res, _next) => {
        console.error('[admin] route error:', err);
        flashRedirect(res,
            { ok: false, msg: 'admin route error', detail: err.message || String(err) },
            '/admin');
    });

    return router;
}
