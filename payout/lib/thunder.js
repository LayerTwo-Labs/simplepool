/* Thunder JSON-RPC 2.0 client.
 *
 * Per LayerTwo-Labs/thunder-rust (rpc-api/lib.rs, app/rpc_server.rs):
 *   - HTTP JSON-RPC, default port 6000 + sidechain_number = 6009
 *   - No auth required (permissive CORS, no bearer/basic)
 *   - Methods we care about:
 *       transfer(dest: Address, value_sats: u64, fee_sats: u64) -> Txid
 *       balance() -> Balance { available_sats, total_sats, ... }
 *       get_wallet_addresses() -> [Address]
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
     * Returns the txid (hex). Throws on insufficient funds, bad address, etc. */
    async transfer(dest, valueSats, feeSats) {
        return this._call('transfer', [dest, Number(valueSats), Number(feeSats)]);
    }

    async getWalletAddresses() {
        return this._call('get_wallet_addresses', []);
    }
}
