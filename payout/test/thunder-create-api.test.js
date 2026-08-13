/* Thunder changed create_transfer out from under us.
 *
 * Up to v0.17.0 it returned an unsigned transaction, which is what the batching
 * path needs: it replaces the single payment output with one per recipient,
 * then signs and submits. Commit a195d67 ("RPC: sign and broadcast txs created
 * via `create_*`", first released in v0.17.1) changed it to sign and broadcast
 * internally and return a bare Txid.
 *
 * Against that, the old code threw `create_transfer returned no outputs` — but
 * only AFTER Thunder had already put the transaction on the network. The payout
 * was live and untracked, and because Thunder cannot spend the change of an
 * unconfirmed transaction, every later attempt failed with `utxo double spent`.
 * The queue stopped permanently while looking like a create-stage failure, which
 * callers treat as a clean abort. It is not one.
 *
 * So the new-API branch is pinned here from both directions: it must be taken
 * when a Txid comes back, and it must NOT be taken when an unsigned tx does.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ThunderClient } from '../lib/thunder.js';

const ADDR_A = '3RobWZetukZUXY9kk763AtMyoJtJ';
const ADDR_B = '3MA4uE5RsmQmJuSNYwGC125NVnVJ';
const TXID = 'feb614d78c11367b8f02beed4cb8d8fcd169ed7513847a7659d567130c0bfa6b';

/* create_transfer returns a bare Txid, as thunder >= 0.17.1 does. sign and
 * submit throw, because reaching them at all is the bug this guards. */
function newApi(returns = TXID) {
    const c = new ThunderClient('http://127.0.0.1:1');
    const seen = { calls: [] };
    c._call = async (method, params) => {
        seen.calls.push(method);
        if (method === 'create_transfer') { seen.created = params; return returns; }
        throw new Error(`${method} must not be called once the node has broadcast`);
    };
    return { c, seen };
}

test('transferDetailed: a bare Txid is treated as already broadcast', async () => {
    const { c, seen } = newApi();
    const r = await c.transferDetailed(ADDR_A, 500, 100);
    assert.equal(r.txid, TXID);
    assert.equal(r.broadcastByNode, true);
    /* Nothing to retain: the node built and signed it, we never saw either. */
    assert.equal(r.unsigned, null);
    assert.equal(r.signed, null);
    assert.deepEqual(seen.calls, ['create_transfer']);
});

test('transferBatchDetailed: one shared address is the normal case', async () => {
    /* A miner's rigs all authenticate with the same Thunder address and differ
     * only by the .rig suffix, so the batch collapses to a single output and
     * the node paying the whole total to it is exactly right. */
    const { c, seen } = newApi();
    const r = await c.transferBatchDetailed(
        [{ address: ADDR_A, sats: 300 }, { address: ADDR_A, sats: 200 }], 100);
    assert.equal(r.txid, TXID);
    assert.equal(r.broadcastByNode, true);
    assert.equal(r.recipients, 2);
    assert.equal(r.total, 500n);
    assert.deepEqual(seen.created, [ADDR_A, 500, 100]);
    assert.deepEqual(seen.calls, ['create_transfer']);
});

test('transferBatchDetailed: distinct addresses are refused, at stage=submit', async () => {
    /* The dangerous case. create_transfer is called with recipients[0] and the
     * FULL total, so the node has already paid everyone's money to one address.
     * Nothing can undo that, and the stage must say so: 'create' would tell the
     * caller nothing was broadcast, and the in-flight row would be dropped. */
    const { c } = newApi();
    await assert.rejects(
        c.transferBatchDetailed(
            [{ address: ADDR_A, sats: 300 }, { address: ADDR_B, sats: 200 }], 100),
        (e) => {
            assert.equal(e.stage, 'submit');
            assert.match(e.message, /already broadcast/);
            assert.match(e.message, new RegExp(TXID));
            assert.match(e.message, /2 distinct addresses/);
            return true;
        });
});

test('an object carrying a txid field is also recognised', async () => {
    const { c } = newApi({ txid: TXID });
    const r = await c.transferDetailed(ADDR_A, 500, 100);
    assert.equal(r.txid, TXID);
    assert.equal(r.broadcastByNode, true);
});

test('the old unsigned-tx API still takes the splice path', async () => {
    /* Regression guard: an unsigned tx must not be mistaken for a Txid, or the
     * batch would silently pay the total to recipients[0]. */
    const c = new ThunderClient('http://127.0.0.1:1');
    const seen = { calls: [] };
    c._call = async (method, params) => {
        seen.calls.push(method);
        if (method === 'create_transfer') {
            const [dest, value] = params;
            return {
                inputs: [[{ Deposit: 'aa:0' }, [1, 2, 3]]],
                proof: { targets: [0], hashes: [] },
                outputs: [
                    { address: dest, content: { Value: value } },
                    { address: 'JPbJrEKEaA69dAADY2qfW7dfyYQ', content: { Value: 400 } },
                ],
            };
        }
        if (method === 'sign_transaction')   { seen.signedTx = params[0]; return { authorized: true }; }
        if (method === 'submit_transaction') return 'old-api-txid';
        throw new Error('unexpected ' + method);
    };

    const r = await c.transferBatchDetailed(
        [{ address: ADDR_A, sats: 300 }, { address: ADDR_B, sats: 200 }], 100);
    assert.equal(r.txid, 'old-api-txid');
    assert.notEqual(r.broadcastByNode, true);
    assert.deepEqual(seen.calls, ['create_transfer', 'sign_transaction', 'submit_transaction']);
    /* The single 500 output became one per recipient, change untouched. */
    assert.deepEqual(seen.signedTx.outputs, [
        { address: ADDR_A, content: { Value: 300 } },
        { address: ADDR_B, content: { Value: 200 } },
        { address: 'JPbJrEKEaA69dAADY2qfW7dfyYQ', content: { Value: 400 } },
    ]);
});
