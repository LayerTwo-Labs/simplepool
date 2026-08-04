/* Thunder JSON-RPC 2.0 client.
 *
 * Per LayerTwo-Labs/thunder-rust (rpc-api/lib.rs, app/rpc_server.rs):
 *   - HTTP JSON-RPC, default port 6000 + sidechain_number = 6009
 *   - No auth required (permissive CORS, no bearer/basic)
 *   - Methods we care about:
 *       create_transfer(dest, value_sats, fee_sats) -> Transaction (unsigned)
 *       sign_transaction(tx, broadcast?) -> Authorized<Transaction>
 *       submit_transaction(authorized_tx) -> Txid
 *       balance() -> Balance { available_sats, total_sats, ... }
 *       get_wallet_addresses() -> [Address]
 *
 * Thunder v0.17.0 removed the one-shot transfer/withdraw methods in
 * favor of the create/sign/submit triple; transfer() below composes
 * them so callers keep the old broadcast-and-return-txid shape.
 *
 * jsonrpsee uses JSON-RPC 2.0 strict-positional params. */

export class ThunderClient {
    constructor({ url, user, pass, timeoutMs = 10000 }) {
        this.url = url;
        this.timeoutMs = timeoutMs;
        this.auth = (user && pass)
            ? 'Basic ' + Buffer.from(`${user}:${pass}`).toString('base64')
            : null;
        this._id = 0;
    }

    async _call(method, params) {
        const id = ++this._id;
        const headers = { 'Content-Type': 'application/json' };
        if (this.auth) headers.Authorization = this.auth;
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), this.timeoutMs);
        let res;
        try {
            res = await fetch(this.url, {
                method: 'POST',
                headers,
                body: JSON.stringify({ jsonrpc: '2.0', id, method, params }),
                signal: ctrl.signal,
            });
        } finally {
            clearTimeout(t);
        }
        if (!res.ok) {
            throw new Error(`thunder rpc ${method}: HTTP ${res.status} ${res.statusText}`);
        }
        const body = await res.json();
        if (body.error) {
            const e = body.error;
            throw new Error(`thunder rpc ${method}: ${e.code} ${e.message}`);
        }
        return body.result;
    }

    /* Returns { available_sats, total_sats, ... } — Thunder's Balance struct.
     * We only need available_sats to gate payouts. */
    async balance() {
        return this._call('balance', []);
    }

    /* Where a broadcast transaction currently stands.
     *
     *   { known: false }                    node has never seen this txid
     *   { known: true, confirmed: false }   in the mempool, holding its inputs
     *   { known: true, confirmed: true }    mined, inputs released
     *
     * `get_transaction` returns `{ tx, block_hash }`, with both null for an
     * unknown txid and `block_hash` null while a known tx is unconfirmed —
     * so the two fields have to be read together. This distinction is what
     * lets the payout loop tell "still settling" from "gone", which decide
     * opposite things: wait, or stop waiting.
     *
     * Never throws: an unreachable node returns { known: false, error }, and
     * callers must treat that as "cannot tell" rather than "not pending". */
    async getTransaction(txid) {
        try {
            const r = await this._call('get_transaction', [txid]);
            const tx = r?.tx ?? null;
            const blockHash = r?.block_hash ?? r?.blockHash ?? null;
            return { known: tx !== null, confirmed: blockHash !== null, blockHash };
        } catch (e) {
            return { known: false, confirmed: false, blockHash: null, error: e.message };
        }
    }

    /* Build, sign, broadcast a Thunder tx from the node's wallet to `dest`.
     * Returns the txid (hex). Throws on insufficient funds, bad address, etc.
     *
     * Three RPCs under the hood. Only submit_transaction can leave a tx
     * on the network, so a throw from create/sign is always a clean
     * abort; a throw from submit carries the same broadcast ambiguity
     * the old one-shot transfer had, and payout.js already treats it
     * that way (abort + retry next tick, stuck-row sweep as backstop). */
    async transfer(dest, valueSats, feeSats) {
        return (await this.transferDetailed(dest, valueSats, feeSats)).txid;
    }

    /* Same three RPCs, but keeps hold of the intermediate transactions and
     * reports which step failed.
     *
     * The transaction is what an operator needs to diagnose a rejection, and
     * previously it was discarded — a failure surfaced as a bare message with
     * no way to inspect what had been built. On error this throws with
     * `.stage` ('create' | 'sign' | 'submit') and whatever transactions had
     * been produced by then attached, so the caller can record them.
     *
     * The stage also disambiguates the broadcast question: create and sign
     * are local, so a throw from either is a clean abort that definitely put
     * nothing on the network. Only a throw from `submit` carries the usual
     * did-it-or-didn't-it ambiguity. */
    async transferDetailed(dest, valueSats, feeSats) {
        let unsigned = null, signed = null;
        const fail = (stage, err) => {
            err.stage    = stage;
            err.unsigned = unsigned;
            err.signed   = signed;
            return err;
        };
        try {
            unsigned = await this._call('create_transfer',
                [dest, Number(valueSats), Number(feeSats)]);
        } catch (e) { throw fail('create', e); }
        try {
            signed = await this._call('sign_transaction', [unsigned, false]);
        } catch (e) { throw fail('sign', e); }
        try {
            const txid = await this._call('submit_transaction', [signed]);
            return { txid, unsigned, signed };
        } catch (e) { throw fail('submit', e); }
    }

    async getWalletAddresses() {
        return this._call('get_wallet_addresses', []);
    }
}
