/* JSON-RPC clients for bitcoind and the enforcer's block template server.
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

/* RPC_INVALID_ADDRESS_OR_KEY: what bitcoind answers for an unknown txid or
 * block hash. */
const NOT_FOUND = -5;

const orNull = async (promise) => {
    try {
        return await promise;
    } catch (e) {
        if (e instanceof RpcError && e.code === NOT_FOUND) return null;
        throw e;
    }
};

/* The pool's own bitcoind: where a slipstream tx is checked, broadcast and
 * followed. The enforcer's template mempool mirrors this node's mempool, so
 * a tx accepted here reaches the templates the proxy mines. */
export class BitcoindClient {
    constructor(opts) {
        this.rpc = new RpcClient(opts);
    }

    /* testmempoolaccept for one tx: { txid, wtxid, allowed, vsize, fees,
     * 'reject-reason' }. Checks everything sendrawtransaction would, and
     * broadcasts nothing. */
    async testAccept(txHex) {
        const [result] = await this.rpc.call('testmempoolaccept', [[txHex]]);
        return result;
    }

    send(txHex) { return this.rpc.call('sendrawtransaction', [txHex]); }

    /* { vsize, weight, fees: { base }, ... }, or null outside the mempool */
    mempoolEntry(txid) { return orNull(this.rpc.call('getmempoolentry', [txid])); }

    /* The block a tx is confirmed in, if it is confirmed at all. Needs
     * txindex, which the enforcer already requires of this node. */
    async txBlock(txid) {
        const tx = await orNull(this.rpc.call('getrawtransaction', [txid, true]));
        return tx?.blockhash && tx.confirmations > 0 ? tx.blockhash : null;
    }

    /* { confirmations, height } of a block, confirmations -1 once it has left
     * the main chain; null if the node does not know it. */
    async blockConfirmations(blockHash) {
        const header = await orNull(this.rpc.call('getblockheader', [blockHash, true]));
        return header ? { confirmations: header.confirmations, height: header.height } : null;
    }
}

/* The enforcer's block template server: read only for the template the
 * proxy is mining, which is where the mineable rate comes from and how a tx
 * is seen to be in play. */
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
}
