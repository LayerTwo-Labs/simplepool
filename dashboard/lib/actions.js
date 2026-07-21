/* Backend implementations for the admin dashboard's write actions.
 *
 * Every function returns { ok, msg, detail? } so the route handler can
 * uniformly format flash messages regardless of which action ran. Never
 * throws — errors are captured into { ok: false, msg }.
 *
 * External systems these talk to:
 *   - Thunder JSON-RPC       — nudgeMine, removeFromMempool
 *   - payout worker HTTP     — triggerPayout
 *   - bip300301_enforcer     — createDeposit (via grpcurl on the host)
 */

import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

/* ---------- Thunder RPC helper ---------- */

async function thunderRpc(rpcUrl, method, params) {
    const body = { jsonrpc: '2.0', id: 1, method, params: params ?? [] };
    const ctl  = new AbortController();
    const t    = setTimeout(() => ctl.abort(), 30_000);
    try {
        const r = await fetch(rpcUrl, {
            method: 'POST',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify(body),
            signal: ctl.signal,
        });
        const text = await r.text();
        let j;
        try { j = JSON.parse(text); }
        catch { throw new Error(`thunder rpc ${method}: non-json response: ${text.slice(0, 200)}`); }
        if (j.error) {
            const msg = j.error.message || JSON.stringify(j.error);
            throw new Error(`thunder rpc ${method}: ${msg}`);
        }
        return j.result;
    } finally {
        clearTimeout(t);
    }
}

/* ---------- Action #4: nudge Thunder to mint a sidechain block ---------- */

export async function nudgeMine({ thunderRpcUrl }) {
    try {
        const result = await thunderRpc(thunderRpcUrl, 'mine', []);
        return {
            ok: true,
            msg: 'Thunder mine nudged',
            detail: result ? String(result).slice(0, 200) : 'ok',
        };
    } catch (e) {
        /* Thunder often returns "BMM request with same prev_bytes already
         * exists" when it can't create a new BMM commit yet. That's not
         * really a failure from the operator's POV — surface it plainly. */
        const msg = e.message || String(e);
        return { ok: false, msg: 'nudge mine failed', detail: msg };
    }
}

/* ---------- Action #3: remove a stuck tx from Thunder's mempool ---------- */

const TXID_RE = /^[0-9a-fA-F]{64}$/;

export async function removeFromMempool({ thunderRpcUrl, txid }) {
    if (!TXID_RE.test(txid || '')) {
        return { ok: false, msg: 'invalid txid', detail: 'expected 64 hex chars' };
    }
    try {
        const result = await thunderRpc(thunderRpcUrl, 'remove_from_mempool', [txid]);
        return {
            ok: true,
            msg: `removed ${txid.slice(0, 12)}… from Thunder mempool`,
            detail: result != null ? String(result).slice(0, 200) : 'ok',
        };
    } catch (e) {
        return { ok: false, msg: 'remove_from_mempool failed', detail: e.message };
    }
}

/* ---------- Action #2: kick the payout worker to run one cycle ---------- */

export async function triggerPayout({ payoutAdminUrl }) {
    if (!payoutAdminUrl) {
        return { ok: false, msg: 'PAYOUT_ADMIN_URL not configured' };
    }
    const ctl = new AbortController();
    const t   = setTimeout(() => ctl.abort(), 60_000);
    try {
        const r = await fetch(new URL('/tick', payoutAdminUrl), {
            method: 'POST',
            signal: ctl.signal,
        });
        const j = await r.json().catch(() => ({}));
        if (!r.ok || j.ok === false) {
            return {
                ok: false,
                msg: 'payout worker /tick failed',
                detail: j.error || `http ${r.status}`,
            };
        }
        const res = j.result || {};
        return {
            ok: true,
            msg: `payout tick fired`,
            detail: `attempted=${res.attempted ?? 0} paid=${res.paid ?? 0} ` +
                    `failed=${res.failed ?? 0}` +
                    (res.reserve_short ? ' (reserve short)' : ''),
        };
    } catch (e) {
        return { ok: false, msg: 'payout worker unreachable', detail: e.message };
    } finally {
        clearTimeout(t);
    }
}

/* ---------- Action #1: BTC → Thunder deposit (via bip300301_enforcer) ---------- */

/* We shell out to `grpcurl` because the enforcer's gRPC on :50051 uses
 * server reflection — no .proto files needed, one static Go binary handles
 * it. The exact call matches ~/scripts/deposit_all_sidechains.sh on the
 * pool host:
 *
 *   grpcurl -d '{sidechain_id, address, value_sats, fee_sats}' -plaintext \
 *       ENFORCER_ADDR cusf.mainchain.v1.WalletService/CreateDepositTransaction
 *
 * On success the enforcer returns a txid; we insert into the pool DB's
 * `deposits` table so the admin ledger shows the operator action.
 */
export async function createDeposit({
    grpcurlBin, enforcerGrpcAddr, sidechainId, address, valueSats, feeSats,
    db,   /* writable handle */
}) {
    /* Validate inputs up front — grpcurl errors are less friendly. */
    const sid = Number(sidechainId);
    if (!Number.isInteger(sid) || sid < 0 || sid > 255) {
        return { ok: false, msg: 'invalid sidechain_id (expected 0..255)' };
    }
    if (!address || typeof address !== 'string' || address.length < 8 || address.length > 128) {
        return { ok: false, msg: 'invalid recipient address' };
    }
    const val = BigInt(valueSats || 0);
    if (val <= 0n) return { ok: false, msg: 'value_sats must be > 0' };
    const fee = BigInt(feeSats || 0);
    if (fee < 0n) return { ok: false, msg: 'fee_sats must be >= 0' };

    const payload = JSON.stringify({
        sidechain_id: sid,
        address,
        value_sats:   Number(val),
        fee_sats:     Number(fee),
    });
    let stdout;
    try {
        const res = await execFileAsync(grpcurlBin, [
            '-plaintext',
            '-d', payload,
            enforcerGrpcAddr,
            'cusf.mainchain.v1.WalletService/CreateDepositTransaction',
        ], { timeout: 60_000, maxBuffer: 1024 * 1024 });
        stdout = res.stdout;
    } catch (e) {
        const stderr = (e.stderr || '').toString().slice(0, 500);
        const stdouterr = (e.stdout || '').toString().slice(0, 300);
        return {
            ok: false,
            msg: 'CreateDepositTransaction failed',
            detail: stderr || stdouterr || e.message,
        };
    }

    /* Enforcer returns JSON with a `txid` field (bytes as base64 or hex
     * depending on version). Parse whatever it gave us; if we can't find
     * a txid, still consider the call a success and echo the raw output
     * so the operator can inspect. */
    let j = {};
    try { j = JSON.parse(stdout); } catch { /* ignore */ }
    const txid =
        j.txid?.hex || j.txid?.value?.hex ||
        (typeof j.txid === 'string' ? j.txid : null);

    /* Log to deposits so /admin history shows it. Best-effort — never fail
     * the whole action just because the DB write hiccuped. */
    try {
        const nowSec = Math.floor(Date.now() / 1000);
        db.prepare(`
            INSERT INTO deposits
                (ts, btc_txid, sats_deposited, fee_sats, thunder_recipient, ctip_before, ctip_after, note)
            VALUES (?, ?, ?, ?, ?, NULL, NULL, ?)
        `).run(nowSec, txid || '(pending)', Number(val), Number(fee),
               address, `via admin dashboard, sidechain_id=${sid}`);
    } catch (e) {
        return {
            ok: true,
            msg: `deposit tx created (DB log failed: ${e.message})`,
            detail: txid ? `txid=${txid}` : stdout.slice(0, 300),
        };
    }
    return {
        ok: true,
        msg: `deposit tx submitted`,
        detail: txid ? `txid=${txid}, ${val} sats to ${address}` :
                       `see raw enforcer response: ${stdout.slice(0, 200)}`,
    };
}
