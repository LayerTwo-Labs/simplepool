import { test } from 'node:test';
import assert from 'node:assert/strict';

import { RpcError } from '../lib/rpc.js';
import { TXID_A, fullTemplate, makeSlipstream } from './helpers.js';

const HEX = '0200000001' + '00'.repeat(60);

const submissions = (store) =>
    store.db.prepare('SELECT txid, accepted, reject_reason FROM slipstream_submissions ORDER BY id').all();

test('an accepted tx is tracked as pending, and the submission logged', async () => {
    const { slipstream, store } = makeSlipstream();
    const { httpStatus, body } = await slipstream.submit(HEX, '203.0.113.5');
    assert.equal(httpStatus, 200);
    assert.equal(body.accepted, true);
    assert.equal(body.status, 'pending');
    assert.equal(body.fee_rate, 2);
    assert.equal(body.raw_hex, undefined, 'the raw tx is not echoed');
    const row = store.get(TXID_A);
    assert.equal(row.raw_hex, HEX);
    assert.equal(row.submitter, '203.0.113.5');
    assert.equal(row.submitted_height, 100);
    assert.deepEqual(submissions(store), [{ txid: TXID_A, accepted: 1, reject_reason: null }]);
    assert.deepEqual(store.events(TXID_A).map(e => e.event), ['accepted']);
});

test('under the mineable rate: taken back out of the enforcer, and refused', async () => {
    const { slipstream, enforcer, store } = makeSlipstream();
    enforcer.template = fullTemplate({ feeRate: 3 });
    const { body } = await slipstream.submit(HEX, 'x');
    assert.equal(body.accepted, false);
    assert.equal(body.reject_reason, 'fee-rate-too-low');
    assert.equal(body.required_fee_rate, 3);
    assert.deepEqual(enforcer.removed, [TXID_A]);
    assert.equal(store.get(TXID_A), null);
    assert.deepEqual(submissions(store), [{ txid: TXID_A, accepted: 0, reject_reason: 'fee-rate-too-low' }]);
});

test('refusals are logged too, with the reason', async () => {
    const { slipstream, enforcer, store } = makeSlipstream();

    assert.equal((await slipstream.submit('zz', 'x')).httpStatus, 400);

    enforcer.nextSubmit = { txid: TXID_A, accepted: false, reject_reason: 'missing-inputs' };
    const refused = await slipstream.submit(HEX, 'x');
    assert.equal(refused.httpStatus, 200);
    assert.equal(refused.body.reject_reason, 'missing-inputs');

    enforcer.nextSubmit = new RpcError('submitslipstreamtx', -32601, 'slipstream is disabled');
    assert.equal((await slipstream.submit(HEX, 'x')).httpStatus, 503);

    assert.deepEqual(submissions(store).map(s => s.reject_reason),
        ['invalid-hex', 'missing-inputs', 'slipstream-disabled']);
    assert.equal(store.get(TXID_A), null);
});

test('resubmitting a tracked tx returns where it stands', async () => {
    const { slipstream, enforcer } = makeSlipstream();
    await slipstream.submit(HEX, 'x');
    enforcer.nextSubmit = { txid: TXID_A, accepted: true, already_present: true, fee_sat: 200, vsize: 100 };
    const { body } = await slipstream.submit(HEX, 'y');
    assert.equal(body.accepted, true);
    assert.equal(body.already_tracked, true);
    assert.equal(body.submitter, 'x');
});
