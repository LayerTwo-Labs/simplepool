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
    assert.equal((await c.balance()).available_sats, 500000n);
});

test('balance returns the shape payout.js actually reads', async () => {
    /* payout.js does `BigInt(bal.available_sats ?? bal.total_sats ?? 0)` --
     * ThunderClient's shape, and the reason the payout loop can drive either
     * rail without branching. This client used to return a bare BigInt, on
     * which both fields are undefined, so the reserve gate read every wallet
     * as empty and refused to pay: "reserve short — available=0". Asserting
     * the returned value equals a BigInt passes just fine against that, which
     * is how it survived. Assert the way the caller reads it instead. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { GetBalance: { confirmedSats: '25000000000' } });
    const bal = await c.balance();
    assert.equal(BigInt(bal.available_sats ?? bal.total_sats ?? 0), 25000000000n);
});

test('a balance past 2^53 survives as an exact integer', async () => {
    /* The enforcer sends confirmedSats as a decimal string. Routing it
     * through Number() rounds above 2^53 -- about 90,000 BTC, which a pool
     * wallet can hold -- and the rounding is silently in either direction. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { GetBalance: { confirmedSats: '9007199254740993' } });   /* 2^53 + 1 */
    assert.equal((await c.balance()).available_sats, 9007199254740993n);
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

/* Settlement, as the enforcer actually reports it.
 *
 * These three pin the shape that made the L1 rail credit a miner the moment
 * it broadcast. paid means MINED, not sent: crediting on broadcast marks the
 * debt settled while the transaction can still be dropped, and there is no
 * negative credit to undo it with. */

test('a mined transaction is confirmed, from confirmationInfo', async () => {
    /* The enforcer reports confirmation as a confirmationInfo submessage, not
     * as a `confirmations` count. Reading the count meant this returned
     * confirmed:false for every transaction ever mined. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListTransactions: { transactions: [
        { txid: { hex: 'aa' },
          confirmationInfo: { height: 812, blockHash: { hex: 'bb' } } },
    ] } });
    assert.deepEqual(await c.getTransaction('aa'),
                     { confirmed: true, known: true, error: null });
});

test('a mempool transaction is known but not confirmed', async () => {
    /* Known-and-unconfirmed is what payout.js turns into "pending", which
     * blocks the next tick instead of re-broadcasting into a double spend. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListTransactions: { transactions: [{ txid: { hex: 'aa' } }] } });
    assert.deepEqual(await c.getTransaction('aa'),
                     { confirmed: false, known: true, error: null });
});

test('confirmationInfo with only a timestamp is NOT confirmation', async () => {
    /* The shape a real enforcer returns for a transaction still in the
     * mempool:
     *
     *   "confirmationInfo": { "timestamp": "..." }            unmined
     *   "confirmationInfo": { "height": 812, "blockHash": ... }  mined
     *
     * The timestamp is when the wallet saw it, not when it was mined. Keying
     * on the presence of confirmationInfo therefore reported every broadcast
     * as confirmed, and paid_sats moved while the transaction could still be
     * dropped. Key on height/blockHash. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListTransactions: { transactions: [
        { txid: { hex: 'aa' }, confirmationInfo: { timestamp: '2026-09-07T10:02:33Z' } },
    ] } });
    assert.deepEqual(await c.getTransaction('aa'),
                     { confirmed: false, known: true, error: null });
});

test('an unconfirmed change output is not evidence of settlement', async () => {
    /* payout.js cross-checks settlement against wallet outputs, and the
     * enforcer applies a transaction to its wallet as soon as it broadcasts
     * it -- so the change output of an unmined payout shows up here at once.
     * Counting it settled the batch on broadcast. Only confirmed outputs may
     * stand as proof; unconfirmedLastSeen is present exactly while unmined. */
    const c = new EnforcerWalletClient({ addr: 'x' });
    stub(c, { ListUnspentOutputs: { outputs: [
        { txid: { hex: 'unmined' }, vout: 1, unconfirmedLastSeen: '2026-09-07T10:01:02Z' },
        { txid: { hex: 'mined'   }, vout: 0 },
    ] } });
    const w = await c.walletUtxos();
    assert.equal(w.ok, true);
    assert.deepEqual(w.utxos.map(u => u.txid), ['mined']);
});
