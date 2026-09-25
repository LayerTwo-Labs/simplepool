/* JSON-RPC clients for the enforcer's block template server and bitcoind.
 *
 * Both speak the same wire format; they differ only in auth. The template
 * server has none, bitcoind takes basic auth from user/pass or its cookie
 * file. A cookie is re-read on every call, because bitcoind rewrites it on
 * each restart and a stale one fails every request until this restarts too.
 */

import fs from 'node:fs';

export class RpcError extends Error {
    constructor(method, code, message) {
        super(`${method}: ${code} ${message}`);
        this.method = method;
        this.code = code;
        this.rpcMessage = message;
    }
}

export class RpcClient {
    constructor({ url, user = null, pass = null, cookieFile = null, timeoutMs = 30000 }) {
        this.url = url;
        this.user = user;
        this.pass = pass;
        this.cookieFile = cookieFile;
        this.timeoutMs = timeoutMs;
        this._id = 0;
    }

    _auth() {
        if (this.cookieFile) {
            const cookie = fs.readFileSync(this.cookieFile, 'utf8').trim();
            return 'Basic ' + Buffer.from(cookie).toString('base64');
        }
        if (this.user && this.pass) {
            return 'Basic ' + Buffer.from(`${this.user}:${this.pass}`).toString('base64');
        }
        return null;
    }

    async call(method, params = []) {
        const headers = { 'Content-Type': 'application/json' };
        const auth = this._auth();
        if (auth) headers.Authorization = auth;
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), this.timeoutMs);
        let res;
        try {
            res = await fetch(this.url, {
                method: 'POST',
                headers,
                body: JSON.stringify({ jsonrpc: '2.0', id: ++this._id, method, params }),
                signal: ctrl.signal,
            });
        } finally {
            clearTimeout(t);
        }
        // bitcoind answers RPC errors with HTTP 500 and a JSON body, so read
        // the body before deciding the status means transport failure.
        const text = await res.text();
        let body;
        try {
            body = JSON.parse(text);
        } catch {
            throw new Error(`${method}: HTTP ${res.status} ${res.statusText}`);
        }
        if (body.error) throw new RpcError(method, body.error.code, body.error.message);
        return body.result;
    }
}

/* The enforcer's block template server, as far as slipstream needs it. */
export class EnforcerClient {
    constructor(opts) {
        this.rpc = new RpcClient(opts);
    }

    /* The enforcer serves only coinbasetxn templates, and says so rather
     * than falling back. */
    getBlockTemplate() {
        return this.rpc.call('getblocktemplate', [{
            rules: ['segwit'],
            capabilities: ['coinbasetxn'],
        }]);
    }

    submit(txHex)   { return this.rpc.call('submitslipstreamtx', [txHex]); }
    status(txid)    { return this.rpc.call('getslipstreamtx', [txid]); }
    remove(txid)    { return this.rpc.call('removeslipstreamtx', [txid]); }
}

/* bitcoind, read-only. */
export class BitcoindClient {
    constructor(opts) {
        this.rpc = new RpcClient(opts);
    }

    /* Confirmations of `blockHash`, -1 once it has left the main chain, or
     * null if bitcoind does not know it. */
    async blockConfirmations(blockHash) {
        try {
            const header = await this.rpc.call('getblockheader', [blockHash, true]);
            return { confirmations: header.confirmations, height: header.height };
        } catch (e) {
            if (e instanceof RpcError && e.code === -5) return null;
            throw e;
        }
    }

    /* The block a tx is confirmed in, if it is confirmed at all. Needs
     * txindex, which the enforcer already requires of this node. */
    async txBlock(txid) {
        try {
            const tx = await this.rpc.call('getrawtransaction', [txid, true]);
            return tx.blockhash && tx.confirmations > 0 ? tx.blockhash : null;
        } catch (e) {
            if (e instanceof RpcError && e.code === -5) return null;
            throw e;
        }
    }
}
