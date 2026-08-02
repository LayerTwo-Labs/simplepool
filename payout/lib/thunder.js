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

    /* Build, sign, broadcast a Thunder tx from the node's wallet to `dest`.
     * Returns the txid (hex). Throws on insufficient funds, bad address, etc.
     *
     * Three RPCs under the hood. Only submit_transaction can leave a tx
     * on the network, so a throw from create/sign is always a clean
     * abort; a throw from submit carries the same broadcast ambiguity
     * the old one-shot transfer had, and payout.js already treats it
     * that way (abort + retry next tick, stuck-row sweep as backstop). */
    async transfer(dest, valueSats, feeSats) {
        const unsigned = await this._call('create_transfer',
            [dest, Number(valueSats), Number(feeSats)]);
        const signed = await this._call('sign_transaction', [unsigned, false]);
        return this._call('submit_transaction', [signed]);
    }

    async getWalletAddresses() {
        return this._call('get_wallet_addresses', []);
    }
}
