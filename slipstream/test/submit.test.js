import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RpcError } from '../lib/rpc.js';
import { TXID_A, fullTemplate, makeSlipstream } from './helpers.js';

const HEX = '0200000001' + '00'.repeat(60);

const submissions = (store) =>
    store.db.prepare('SELECT txid, accepted, reject_reason FROM slipstream_submissions ORDER BY id').all();

test('an accepted tx is broadcast, tracked as pending, and the submission logged', async () => {
    const { slipstream, bitcoind, store } = makeSlipstream();
    const { httpStatus, body } = await slipstream.submit(HEX, '203.0.113.5');
    assert.equal(httpStatus, 200);
    assert.equal(body.accepted, true);
    assert.equal(body.status, 'pending');
    assert.equal(body.fee_sats, 200);
    assert.equal(body.fee_rate, 2);
    assert.equal(body.raw_hex, undefined, 'the raw tx is not echoed');
    assert.deepEqual(bitcoind.sent, [HEX]);
    const row = store.get(TXID_A);
    assert.equal(row.raw_hex, HEX);
    assert.equal(row.weight, 400);
    assert.equal(row.submitter, '203.0.113.5');
    assert.equal(row.submitted_height, 100);
    assert.deepEqual(submissions(store), [{ txid: TXID_A, accepted: 1, reject_reason: null }]);
    assert.deepEqual(store.events(TXID_A).map(e => e.event), ['accepted']);
});

test('under the mineable rate: refused before anything is broadcast', async () => {
    const { slipstream, bitcoind, enforcer, store } = makeSlipstream();
    enforcer.template = fullTemplate({ feeRate: 3 });
    const { body } = await slipstream.submit(HEX, 'x');
    assert.equal(body.accepted, false);
    assert.equal(body.reject_reason, 'fee-rate-too-low');
    assert.equal(body.required_fee_rate, 3);
    assert.deepEqual(bitcoind.sent, [], 'nothing can be taken back once sent, so nothing is sent');
    assert.equal(store.get(TXID_A), null);
    assert.deepEqual(submissions(store), [{ txid: TXID_A, accepted: 0, reject_reason: 'fee-rate-too-low' }]);
});

test("the node's refusals are passed on, and logged", async () => {
    const { slipstream, bitcoind, store } = makeSlipstream();

    assert.equal((await slipstream.submit('zz', 'x')).httpStatus, 400);

    bitcoind.nextTest = { txid: TXID_A, allowed: false, 'reject-reason': 'missing-inputs' };
    const refused = await slipstream.submit(HEX, 'x');
    assert.equal(refused.httpStatus, 200);
    assert.equal(refused.body.reject_reason, 'missing-inputs');

    bitcoind.nextTest = new RpcError('testmempoolaccept', -22, 'TX decode failed');
    assert.equal((await slipstream.submit(HEX, 'x')).httpStatus, 400);

    // Passed the check, lost a race before the broadcast
    bitcoind.sendError = new RpcError('sendrawtransaction', -26, 'txn-mempool-conflict');
    assert.equal((await slipstream.submit(HEX, 'x')).body.reject_reason, 'txn-mempool-conflict');

    assert.deepEqual(submissions(store).map(s => s.reject_reason),
        ['invalid-hex', 'missing-inputs', 'tx-decode-failed', 'txn-mempool-conflict']);
    assert.equal(store.get(TXID_A), null);
});

test('a tx the node already has is taken on without sending it again', async () => {
    const { slipstream, bitcoind, store } = makeSlipstream();
    bitcoind.mempool.set(TXID_A, { vsize: 100, weight: 400, fees: { base: 500e-8 } });
    bitcoind.nextTest = { txid: TXID_A, allowed: false, 'reject-reason': 'txn-already-in-mempool' };
    const { body } = await slipstream.submit(HEX, 'x');
    assert.equal(body.accepted, true);
    assert.equal(body.fee_sats, 500);
    assert.deepEqual(bitcoind.sent, []);
    assert.equal(store.get(TXID_A).status, 'pending');
});

test('resubmitting a tracked tx returns where it stands', async () => {
    const { slipstream } = makeSlipstream();
    await slipstream.submit(HEX, 'x');
    const { body } = await slipstream.submit(HEX, 'y');
    assert.equal(body.accepted, true);
    assert.equal(body.already_tracked, true);
    assert.equal(body.submitter, 'x');
});
