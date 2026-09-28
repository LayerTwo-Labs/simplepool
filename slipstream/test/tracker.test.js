import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RpcError } from '../lib/rpc.js';
import { BLOCK_1, BLOCK_2, TXID_A, fakePool, makeSlipstream } from './helpers.js';

const HEX = '0200000001' + '00'.repeat(60);

async function submitted(opts = {}) {
    const ctx = makeSlipstream(opts);
    await ctx.slipstream.submit(HEX, 'x');
    return ctx;
}

const eventsOf = (store) => store.events(TXID_A).map(e => e.event);

test('in and out of the template, without an event per poll', async () => {
    const { slipstream, enforcer, store } = await submitted();
    enforcer.template.transactions = [{ txid: TXID_A, weight: 400, fee: 200 }];
    await slipstream.tick();
    await slipstream.tick();
    const row = store.get(TXID_A);
    assert.equal(row.status, 'in_template');
    assert.ok(row.first_in_template_at !== null && row.last_in_template_at !== null);

    enforcer.template.transactions = [];
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'pending');
    assert.deepEqual(eventsOf(store), ['accepted', 'in_template', 'pending']);
});

test('mined by the pool, then confirmed', async () => {
    const { slipstream, bitcoind, store } = await submitted({ pool: fakePool({ ours: [BLOCK_1] }) });
    bitcoind.mine(TXID_A, BLOCK_1, { height: 100, confirmations: 1 });
    await slipstream.tick();
    let row = store.get(TXID_A);
    assert.equal(row.status, 'mined');
    assert.equal(row.mined_block_hash, BLOCK_1);
    assert.equal(row.mined_height, 100);
    assert.equal(row.mined_by_pool, 1);

    bitcoind.headers.set(BLOCK_1, { height: 100, confirmations: 6 });
    await slipstream.tick();
    row = store.get(TXID_A);
    assert.equal(row.status, 'confirmed');
    assert.equal(row.confirmations, 6);
    assert.deepEqual(eventsOf(store), ['accepted', 'mined', 'confirmed']);
});

test('mined by another pool: it was relayed, so that is recorded, not assumed away', async () => {
    const { slipstream, bitcoind, store } = await submitted({ pool: fakePool({ ours: [] }) });
    bitcoind.mine(TXID_A, BLOCK_1);
    await slipstream.tick();
    assert.equal(store.get(TXID_A).mined_by_pool, 0);
});

test('an orphaned block the node put the tx back from: pending again', async () => {
    const { slipstream, bitcoind, store } = await submitted();
    bitcoind.mine(TXID_A, BLOCK_1);
    await slipstream.tick();

    bitcoind.headers.set(BLOCK_1, { height: 100, confirmations: -1 });
    bitcoind.txBlocks.delete(TXID_A);
    bitcoind.mempool.set(TXID_A, { vsize: 100, weight: 400, fees: { base: 200e-8 } });
    await slipstream.tick();
    const row = store.get(TXID_A);
    assert.equal(row.status, 'pending');
    assert.equal(row.mined_block_hash, null);
    assert.equal(row.resubmissions, 0, 'the node put it back, nothing was sent');
});

test('an orphaned tx mined again elsewhere follows its new block', async () => {
    const { slipstream, bitcoind, store } = await submitted();
    bitcoind.mine(TXID_A, BLOCK_1);
    await slipstream.tick();

    bitcoind.headers.set(BLOCK_1, { height: 100, confirmations: -1 });
    bitcoind.mine(TXID_A, BLOCK_2);
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'mined');
    assert.equal(store.get(TXID_A).mined_block_hash, BLOCK_2);
});

test('out of the mempool, not mined: sent again, and pending if the node takes it', async () => {
    const { slipstream, bitcoind, store } = await submitted();
    bitcoind.mempool.delete(TXID_A);   // evicted, or a node restart without mempool.dat
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'pending');
    assert.equal(store.get(TXID_A).resubmissions, 1);
    assert.equal(bitcoind.sent.length, 2);
    assert.ok(eventsOf(store).includes('rebroadcast'));
});

test("out of the mempool and refused again: dropped, with the node's reason", async () => {
    const { slipstream, bitcoind, store } = await submitted();
    bitcoind.mempool.delete(TXID_A);   // replaced, or its input spent by a block
    bitcoind.sendError = new RpcError('sendrawtransaction', -25, 'bad-txns-inputs-missingorspent');
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'dropped');
    assert.equal(store.get(TXID_A).status_reason, 'bad-txns-inputs-missingorspent');
});

test('"already in block chain" on a rebroadcast waits for the block, rather than dropping', async () => {
    const { slipstream, bitcoind, store } = await submitted();
    bitcoind.mempool.delete(TXID_A);
    bitcoind.sendError = new RpcError('sendrawtransaction', -27, 'Transaction already in block chain');
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'pending');
});

test('an unreachable enforcer fails the tick and changes nothing', async () => {
    const { slipstream, enforcer, store } = await submitted();
    enforcer.down = true;
    await assert.rejects(slipstream.tick());
    assert.equal(store.get(TXID_A).status, 'pending');
    assert.ok(slipstream.enforcerError);
});
