/* Which of the three RPCs failed decides whether aborting a payout is safe.
 *
 * create and sign are local to the Thunder node, so a throw from either
 * definitely put nothing on the network and the in-flight row can be dropped
 * freely. Only a throw from submit carries broadcast ambiguity. Getting the
 * stage wrong would either strand payouts or risk double-paying, so it is
 * pinned here — along with the transactions, which are what an operator needs
 * to diagnose the failure and which used to be discarded.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ThunderClient } from '../lib/thunder.js';

/* Stub _call so no network is involved: `failAt` names the RPC that throws. */
function client(failAt) {
    const c = new ThunderClient('http://127.0.0.1:1');
    c._call = async (method) => {
        if (method === failAt) throw new Error(`${method} exploded`);
        if (method === 'create_transfer')   return { unsigned: true };
        if (method === 'sign_transaction')  return { signed: true };
        if (method === 'submit_transaction') return 'txid-abc';
        throw new Error('unexpected ' + method);
    };
    return c;
}

test('happy path returns txid plus both transactions', async () => {
    const r = await client(null).transferDetailed('addr', 100, 1);
    assert.equal(r.txid, 'txid-abc');
    assert.deepEqual(r.unsigned, { unsigned: true });
    assert.deepEqual(r.signed,   { signed: true });
});

test('transfer() still returns just the txid', async () => {
    assert.equal(await client(null).transfer('addr', 100, 1), 'txid-abc');
});

test('create failure: stage=create, no transactions yet', async () => {
    await assert.rejects(client('create_transfer').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'create');
        assert.equal(e.unsigned, null);
        assert.equal(e.signed, null);
        return true;
    });
});

test('sign failure: stage=sign, unsigned tx retained', async () => {
    await assert.rejects(client('sign_transaction').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'sign');
        assert.deepEqual(e.unsigned, { unsigned: true });
        assert.equal(e.signed, null);
        return true;
    });
});

test('submit failure: stage=submit, SIGNED tx retained', async () => {
    /* The important one — this is the tx that hit the node and was refused,
     * so it must survive to be shown. */
    await assert.rejects(client('submit_transaction').transferDetailed('a', 1, 1), (e) => {
        assert.equal(e.stage, 'submit');
        assert.deepEqual(e.signed, { signed: true });
        assert.match(e.message, /submit_transaction exploded/);
        return true;
    });
});
