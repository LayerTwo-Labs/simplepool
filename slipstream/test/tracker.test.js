import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
    BLOCK_1, BLOCK_2, TIP, TXID_A, FakeBitcoind, fakePool, makeSlipstream,
} from './helpers.js';

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
    const bitcoind = new FakeBitcoind();
    const { slipstream, enforcer, store } = await submitted({
        bitcoind, pool: fakePool({ ours: [BLOCK_1] }),
    });
    enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason: 'mined', block_hash: BLOCK_1 });
    bitcoind.headers.set(BLOCK_1, { confirmations: 1, height: 100 });
    await slipstream.tick();
    let row = store.get(TXID_A);
    assert.equal(row.status, 'mined');
    assert.equal(row.mined_block_hash, BLOCK_1);
    assert.equal(row.mined_height, 100);
    assert.equal(row.mined_by_pool, 1);

    bitcoind.headers.set(BLOCK_1, { confirmations: 6, height: 100 });
    await slipstream.tick();
    row = store.get(TXID_A);
    assert.equal(row.status, 'confirmed');
    assert.equal(row.confirmations, 6);
    assert.deepEqual(eventsOf(store), ['accepted', 'mined', 'confirmed']);
});

test('without bitcoind, depth comes from the template height', async () => {
    const { slipstream, enforcer, store } = await submitted();
    enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason: 'mined', block_hash: TIP });
    await slipstream.tick();
    assert.equal(store.get(TXID_A).mined_height, 99);
    assert.equal(store.get(TXID_A).mined_by_pool, 0);
    enforcer.template = { ...enforcer.template, height: 105 };
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'confirmed');
});

test('an orphaned block puts the tx back in the enforcer', async () => {
    const bitcoind = new FakeBitcoind();
    const { slipstream, enforcer, store } = await submitted({ bitcoind });
    enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason: 'mined', block_hash: BLOCK_1 });
    bitcoind.headers.set(BLOCK_1, { confirmations: 1, height: 100 });
    await slipstream.tick();

    bitcoind.headers.set(BLOCK_1, { confirmations: -1, height: 100 });
    await slipstream.tick();
    const row = store.get(TXID_A);
    assert.equal(row.status, 'pending');
    assert.equal(row.mined_block_hash, null);
    assert.equal(row.resubmissions, 1);
    assert.equal(enforcer.submitted.length, 2);
});

test('an orphaned tx mined again elsewhere follows its new block', async () => {
    const bitcoind = new FakeBitcoind();
    const { slipstream, enforcer, store } = await submitted({ bitcoind });
    enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason: 'mined', block_hash: BLOCK_1 });
    bitcoind.headers.set(BLOCK_1, { confirmations: 1, height: 100 });
    await slipstream.tick();

    bitcoind.headers.set(BLOCK_1, { confirmations: -1, height: 100 });
    bitcoind.headers.set(BLOCK_2, { confirmations: 1, height: 100 });
    bitcoind.txBlocks.set(TXID_A, BLOCK_2);
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'mined');
    assert.equal(store.get(TXID_A).mined_block_hash, BLOCK_2);
    assert.equal(enforcer.submitted.length, 1, 'not resubmitted');
});

test('a reorg, or an enforcer restart, resubmits', async () => {
    for (const status of [
        { status: 'removed', txid: TXID_A, reason: 'reorged', block_hash: BLOCK_1 },
        { status: 'unknown', txid: TXID_A },
    ]) {
        const { slipstream, enforcer, store } = await submitted();
        enforcer.statuses.set(TXID_A, status);
        await slipstream.tick();
        assert.equal(store.get(TXID_A).status, 'pending', status.status);
        assert.equal(store.get(TXID_A).resubmissions, 1, status.status);
    }
});

test('a resubmission the enforcer refuses drops the tx, with its reason', async () => {
    const { slipstream, enforcer, store } = await submitted();
    enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason: 'reorged', block_hash: BLOCK_1 });
    enforcer.nextSubmit = { txid: TXID_A, accepted: false, reject_reason: 'bad-txns-inputs-missingorspent' };
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'dropped');
    assert.equal(store.get(TXID_A).status_reason, 'bad-txns-inputs-missingorspent');
});

test('a conflict or a lost parent drops the tx', async () => {
    for (const reason of ['conflict_mined', 'parent_removed', 'rejected_by_enforcer']) {
        const { slipstream, enforcer, store } = await submitted();
        enforcer.statuses.set(TXID_A, { status: 'removed', txid: TXID_A, reason });
        await slipstream.tick();
        assert.equal(store.get(TXID_A).status, 'dropped', reason);
        assert.equal(store.get(TXID_A).status_reason, reason);
    }
});

test('a tx that waits past the expiry is withdrawn', async () => {
    const { slipstream, enforcer, store } = await submitted({ cfg: { expiryBlocks: 10 } });
    enforcer.template = { ...enforcer.template, height: 110 };
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'expired');
    assert.deepEqual(enforcer.removed, [TXID_A]);
    // The withdrawal the expiry caused is not mistaken for someone else's
    await slipstream.tick();
    assert.equal(store.get(TXID_A).status, 'expired');
});

test('an unreachable enforcer fails the tick and changes nothing', async () => {
    const { slipstream, enforcer, store } = await submitted();
    enforcer.down = true;
    await assert.rejects(slipstream.tick());
    assert.equal(store.get(TXID_A).status, 'pending');
    assert.ok(slipstream.enforcerError);
});
