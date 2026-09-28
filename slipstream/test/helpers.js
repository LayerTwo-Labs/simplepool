/* A fake bitcoind and enforcer, scripted per test, around a real in-memory
 * store: the store is what the tests are about, so it is not faked. */

import { openStore } from '../lib/store.js';
import { Slipstream } from '../lib/slipstream.js';
import { RpcError } from '../lib/rpc.js';

export const TXID_A = 'a'.repeat(64);
export const TXID_B = 'b'.repeat(64);
export const BLOCK_1 = '1'.repeat(64);
export const BLOCK_2 = '2'.repeat(64);
export const TIP = 'f'.repeat(64);

export const quietLog = { debug() {}, info() {}, warn() {}, error() {} };

export function baseCfg(overrides = {}) {
    return {
        minFeeRate: 1,
        confirmations: 6,
        rateLimitPerMin: 30,
        trustProxy: false,
        presentation: {},
        ...overrides,
    };
}

/* A node with a mempool (txid -> entry) and a chain (txid -> block hash,
 * block hash -> header). */
export class FakeBitcoind {
    constructor() {
        this.mempool = new Map();
        this.txBlocks = new Map();
        this.headers = new Map();
        this.sent = [];
        this.nextTest = null;
        this.sendError = null;
    }

    /* By default any tx checks out as TXID_A, 100 vB paying 200 sat. */
    async testAccept(_hex) {
        const next = this.nextTest ?? { txid: TXID_A, allowed: true, vsize: 100, fees: { base: 200e-8 } };
        this.nextTest = null;
        if (next instanceof RpcError) throw next;
        this._lastTest = { wtxid: next.txid, ...next };
        return this._lastTest;
    }

    async send(hex) {
        this.sent.push(hex);
        if (this.sendError) {
            const e = this.sendError;
            this.sendError = null;
            throw e;
        }
        const { txid, vsize = 100, fees = { base: 200e-8 } } = this._lastTest ?? { txid: TXID_A };
        this.mempool.set(txid, { vsize, weight: vsize * 4, fees });
        return txid;
    }

    async mempoolEntry(txid) { return this.mempool.get(txid) ?? null; }
    async txBlock(txid) { return this.txBlocks.get(txid) ?? null; }
    async blockConfirmations(hash) { return this.headers.get(hash) ?? null; }

    /* Move a tx from the mempool into `block`, `confirmations` deep. */
    mine(txid, block, { height = 100, confirmations = 1 } = {}) {
        this.mempool.delete(txid);
        this.txBlocks.set(txid, block);
        this.headers.set(block, { height, confirmations });
    }
}

export class FakeEnforcer {
    constructor() {
        this.template = { height: 100, previousblockhash: TIP, transactions: [] };
    }

    async getBlockTemplate() {
        if (this.down) throw new Error('connect ECONNREFUSED');
        return this.template;
    }
}

export function fakePool({ meta = null, ours = [] } = {}) {
    return {
        meta: () => meta,
        foundBlock: (hash) => ours.includes(hash),
    };
}

export function makeSlipstream({ cfg = {}, pool = fakePool() } = {}) {
    const bitcoind = new FakeBitcoind();
    const enforcer = new FakeEnforcer();
    const slipstream = new Slipstream({
        store: openStore(':memory:'),
        bitcoind,
        enforcer,
        pool,
        cfg: baseCfg(cfg),
        log: quietLog,
    });
    return { slipstream, bitcoind, enforcer, store: slipstream.store };
}

/* A template carrying `count` txs of `weight` wu each, paying `feeRate`. */
export function fullTemplate({ feeRate, weight = 400_000, count = 10 }) {
    return {
        height: 100,
        previousblockhash: TIP,
        transactions: Array.from({ length: count }, (_, i) => ({
            txid: String(i).padStart(64, '0'),
            weight,
            fee: Math.round(feeRate * (weight / 4)),
        })),
    };
}
