/* A fake enforcer and bitcoind, scripted per test, around a real in-memory
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
        expiryBlocks: 144,
        rateLimitPerMin: 30,
        trustProxy: false,
        presentation: {},
        ...overrides,
    };
}

/* An enforcer whose mempool is a Map of txid -> status, and whose
 * template is whatever the test sets. */
export class FakeEnforcer {
    constructor() {
        this.template = { height: 100, previousblockhash: TIP, transactions: [] };
        this.statuses = new Map();
        this.submitted = [];
        this.removed = [];
        this.nextSubmit = null;
    }

    async getBlockTemplate() {
        if (this.down) throw new Error('connect ECONNREFUSED');
        return this.template;
    }

    /* `nextSubmit` scripts the next answer; by default any tx is accepted as
     * `txid` at 2 sat/vB. */
    async submit(hex) {
        this.submitted.push(hex);
        const next = this.nextSubmit ?? { txid: TXID_A, accepted: true, fee_sat: 200, vsize: 100 };
        this.nextSubmit = null;
        if (next instanceof RpcError) throw next;
        const resp = { wtxid: next.txid, weight: (next.vsize ?? 100) * 4, ...next };
        if (resp.accepted) this.statuses.set(resp.txid, { status: 'pending', txid: resp.txid });
        return resp;
    }

    async status(txid) {
        return this.statuses.get(txid) ?? { status: 'unknown', txid };
    }

    async remove(txid) {
        this.removed.push(txid);
        this.statuses.set(txid, { status: 'removed', txid, reason: 'withdrawn' });
        return [txid];
    }
}

export class FakeBitcoind {
    constructor() {
        this.headers = new Map();   // block hash -> { confirmations, height }
        this.txBlocks = new Map();  // txid -> block hash
    }

    async blockConfirmations(hash) { return this.headers.get(hash) ?? null; }
    async txBlock(txid) { return this.txBlocks.get(txid) ?? null; }
}

export function fakePool({ meta = null, ours = [] } = {}) {
    return {
        meta: () => meta,
        foundBlock: (hash) => ours.includes(hash),
    };
}

export function makeSlipstream({ cfg = {}, bitcoind = null, pool = fakePool() } = {}) {
    const enforcer = new FakeEnforcer();
    const slipstream = new Slipstream({
        store: openStore(':memory:'),
        enforcer,
        bitcoind,
        pool,
        cfg: baseCfg(cfg),
        log: quietLog,
    });
    return { slipstream, enforcer, store: slipstream.store };
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
