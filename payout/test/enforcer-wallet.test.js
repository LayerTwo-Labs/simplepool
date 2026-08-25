/* The L1 payout rail. Nothing here talks to a real enforcer: the point is
 * the shape of the request we send it and the shape of the answers we read,
 * both of which are where a rail gets a payment wrong silently. */

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EnforcerWalletClient } from '../lib/enforcer-wallet.js';

/* Stand in for the ConnectRPC layer by recording what would be sent. */
function stub(client, handlers) {
    const calls = [];
    client._call = async (method, body) => {
        calls.push({ method, body });
        const h = handlers[method];
        if (typeof h === 'function') return h(body);
        if (h instanceof Error) throw h;
        return h ?? {};
    };
    return calls;
}

test('a batch becomes one SendTransaction with a destinations map', async () => {
    const c = new EnforcerWalletClient({ addr: '127.0.0.1:50051', feeRateSatPerVb: 7 });
    const calls = stub(c, { SendTransaction: { txid: { hex: 'deadbeef' } } });

    const r = await c.transferBatchDetailed([
        { address: 'bcrt1qalice', sats: 1000n },
        { address: 'bcrt1qbob',   sats: 2500n },
    ], 999 /* absolute fee — ignored on this rail */);

    assert.equal(calls.length, 1);
    assert.equal(calls[0].method, 'SendTransaction');
    assert.deepEqual(calls[0].body.destinations,
                     { bcrt1qalice: 1000, bcrt1qbob: 2500 });
    /* A rate, not an absolute fee: the enforcer selects the inputs, so only
     * it knows the size of the transaction the fee applies to. */
    assert.deepEqual(calls[0].body.fee_rate, { sat_per_vbyte: 7 });
    assert.equal(r.txid, 'deadbeef');
});

test('two rigs on one payout address are added, not overwritten', async () => {
    /* destinations is keyed by address. Sending the list unmerged lets the
     * second entry replace the first, paying that miner once for two debts
     * while the ledger marks both settled — a shortfall that balances
     * perfectly on the pool's side and is invisible except to the miner. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    const calls = stub(c, { SendTransaction: { txid: 'abc' } });

    await c.transferBatchDetailed([
        { address: 'bcrt1qsame', sats: 1000n },
        { address: 'bcrt1qsame', sats: 250n  },
        { address: 'bcrt1qother', sats: 7n   },
    ]);

    assert.deepEqual(calls[0].body.destinations,
                     { bcrt1qsame: 1250, bcrt1qother: 7 });
});

test('a reply with no txid is an error, not a silent success', async () => {
    /* Returning undefined here would mark the batch broadcast with txid
     * undefined, and settlement would then look for a transaction that
     * cannot be found — stranding the batch in flight forever. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { SendTransaction: { ok: true } });
    await assert.rejects(() => c.transferBatchDetailed([{ address: 'a', sats: 1n }]),
                         /no txid/);
});

test('non-positive and address-less recipients are refused before sending', async () => {
    const c = new EnforcerWalletClient({ addr: 'x' });
    const calls = stub(c, { SendTransaction: { txid: 'z' } });
    await assert.rejects(() => c.transferBatchDetailed([{ address: 'a', sats: 0n }]),
                         /non-positive/);
    await assert.rejects(() => c.transferBatchDetailed([{ address: '', sats: 5n }]),
                         /missing address/);
    await assert.rejects(() => c.transferBatchDetailed([]), /no recipients/);
    assert.equal(calls.length, 0);
});

test('an encrypted wallet is unlocked once, before the first spend', async () => {
    const c = new EnforcerWalletClient({ addr: 'x', passphrase: 'hunter2' });
    const calls = stub(c, { UnlockWallet: {}, SendTransaction: { txid: 't' } });

    await c.transferBatchDetailed([{ address: 'a', sats: 1n }]);
    await c.transferBatchDetailed([{ address: 'b', sats: 1n }]);

    const unlocks = calls.filter(x => x.method === 'UnlockWallet');
    assert.equal(unlocks.length, 1);
    assert.equal(unlocks[0].body.password, 'hunter2');
    /* and it happened before the first spend, not after it failed */
    assert.equal(calls[0].method, 'UnlockWallet');
});

test('a wallet with no passphrase is never asked to unlock', async () => {
    const c = new EnforcerWalletClient({ addr: 'x' });
    const calls = stub(c, { SendTransaction: { txid: 't' } });
    await c.transferBatchDetailed([{ address: 'a', sats: 1n }]);
    assert.equal(calls.filter(x => x.method === 'UnlockWallet').length, 0);
});

test('balance counts confirmed sats only', async () => {
    /* A coinbase output is not spendable until 100 deep. Counting anything
     * else is how a pool promises what it cannot send. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { GetBalance: { confirmedSats: 500000, pendingSats: 999999999 } });
    assert.equal(await c.balance(), 500000n);
});

test('an unreachable node is unknown, never confirmed and never evicted', async () => {
    /* settlementState() in payout.js turns this into "unknown", which blocks
     * and asks for a human. Reporting it as not-known-and-no-error would let
     * the batch be re-queued and paid twice. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListTransactions: new Error('connection refused') });
    const st = await c.getTransaction('abc');
    assert.equal(st.confirmed, false);
    assert.equal(st.known, false);
    assert.match(st.error, /connection refused/);
});

test('a transaction is settled only once it has a confirmation', async () => {
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListTransactions: { transactions: [
        { txid: { hex: 'aaa' }, confirmations: 0 },
        { txid: { hex: 'bbb' }, confirmations: 3 },
    ] } });
    assert.deepEqual(await c.getTransaction('bbb'), { confirmed: true,  known: true,  error: null });
    assert.deepEqual(await c.getTransaction('aaa'), { confirmed: false, known: true,  error: null });
    assert.deepEqual(await c.getTransaction('ccc'), { confirmed: false, known: false, error: null });
});

test('L1 needs no mining nudge, but answers the call payout.js makes', async () => {
    /* Thunder advances only when a mainchain block commits to it, so its rail
     * nudges. Bitcoin blocks arrive unasked. Answering rather than throwing is
     * what lets the payout loop drive either rail without branching. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    assert.equal((await c.mine()).ok, true);
    assert.deepEqual(await c.mempool(), { ok: true, txids: [] });
});
