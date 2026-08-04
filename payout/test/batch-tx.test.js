/* Splitting a Thunder transfer into many recipients.
 *
 * transferBatchDetailed asks Thunder to build a transfer for the TOTAL and
 * then replaces that one output with one per recipient. Everything else —
 * inputs, the utreexo proof, the change output — is left exactly as Thunder
 * produced it.
 *
 * The failure modes here are silent and expensive: outputs summing to less
 * than the inputs burns the difference as fee, and splitting the CHANGE
 * output instead of the payment would hand the entire wallet to one worker.
 * So both are asserted before anything is signed, and both are tested.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ThunderClient } from '../lib/thunder.js';

const CHANGE_ADDR = 'JPbJrEKEaA69dAADY2qfW7dfyYQ';

/* Mirrors a real create_transfer response: the requested output, then change.
 * `proof` carries input positions, not outputs, which is why appending
 * outputs does not invalidate it. */
function stub({ inputTotal = 1_000_000_000n, feeSats = 100n, overrideOutputs } = {}) {
    const c = new ThunderClient('http://127.0.0.1:1');
    const seen = {};
    c._call = async (method, params) => {
        if (method === 'create_transfer') {
            const [dest, value] = params;
            seen.created = { dest, value };
            return {
                inputs: [[{ Deposit: 'aa:0' }, [1, 2, 3]]],
                proof: { targets: [0], hashes: [{ Some: [9, 9] }] },
                outputs: overrideOutputs ?? [
                    { address: dest, content: { Value: value } },
                    { address: CHANGE_ADDR,
                      content: { Value: Number(inputTotal - BigInt(value) - feeSats) } },
                ],
            };
        }
        if (method === 'sign_transaction')   { seen.signed = params[0]; return { authorized: params[0] }; }
        if (method === 'submit_transaction') { return 'batchtxid'; }
        throw new Error('unexpected ' + method);
    };
    return { c, seen };
}

const R = (address, sats) => ({ address, sats: BigInt(sats) });

test('one output becomes many, and the total is unchanged', async () => {
    const { c, seen } = stub();
    const r = await c.transferBatchDetailed(
        [R('addrA', 5_000_000), R('addrB', 6_000_000), R('addrC', 7_000_000)], 100n);

    assert.equal(r.txid, 'batchtxid');
    assert.equal(r.recipients, 3);

    /* Thunder was asked for the total, once. */
    assert.equal(seen.created.value, 18_000_000);

    const outs = seen.signed.outputs;
    assert.equal(outs.length, 4, '3 recipients + change');
    assert.deepEqual(outs.slice(0, 3).map(o => [o.address, o.content.Value]),
                     [['addrA', 5_000_000], ['addrB', 6_000_000], ['addrC', 7_000_000]]);
    /* Change untouched: 1e9 - 18e6 - 100 */
    assert.equal(outs[3].address, CHANGE_ADDR);
    assert.equal(outs[3].content.Value, 981_999_900);
    /* Every satoshi accounted for. */
    assert.equal(outs.reduce((a, o) => a + o.content.Value, 0), 1_000_000_000 - 100);
});

test('inputs and proof are passed through untouched', async () => {
    /* The proof commits to inputs, not outputs — rebuilding it would be both
     * unnecessary and a way to get it wrong. */
    const { c, seen } = stub();
    await c.transferBatchDetailed([R('addrA', 1_000), R('addrB', 2_000)], 100n);
    assert.deepEqual(seen.signed.inputs, [[{ Deposit: 'aa:0' }, [1, 2, 3]]]);
    assert.deepEqual(seen.signed.proof, { targets: [0], hashes: [{ Some: [9, 9] }] });
});

test('a single recipient still works', async () => {
    const { c, seen } = stub();
    await c.transferBatchDetailed([R('addrA', 4_000_000)], 100n);
    const outs = seen.signed.outputs;
    assert.equal(outs.length, 2);
    assert.equal(outs[0].content.Value, 4_000_000);
});

test('an ambiguous payment output is refused, not guessed', async () => {
    /* Change happening to equal the payment. Picking the wrong one would
     * split the change and send the whole wallet to the recipients. */
    const { c } = stub({
        overrideOutputs: [
            { address: 'addrA', content: { Value: 5_000_000 } },
            { address: 'addrA', content: { Value: 5_000_000 } },
        ],
    });
    await assert.rejects(
        () => c.transferBatchDetailed([R('addrA', 5_000_000)], 100n),
        /expected exactly one output of 5000000/);
});

test('a missing payment output is refused', async () => {
    const { c } = stub({
        overrideOutputs: [{ address: CHANGE_ADDR, content: { Value: 999_999_900 } }],
    });
    await assert.rejects(
        () => c.transferBatchDetailed([R('addrA', 5_000_000)], 100n),
        /expected exactly one output/);
});

test('nothing is signed when the payment output cannot be located', async () => {
    const { c, seen } = stub({ overrideOutputs: [{ address: 'other', content: { Value: 1 } }] });
    await assert.rejects(() => c.transferBatchDetailed([R('addrA', 5_000_000)], 100n));
    assert.equal(seen.signed, undefined, 'must fail before signing, not after');
});

test('empty and non-positive amounts are rejected up front', async () => {
    const { c } = stub();
    await assert.rejects(() => c.transferBatchDetailed([], 100n), /no recipients/);
    await assert.rejects(() => c.transferBatchDetailed([R('a', 0)], 100n), /non-positive/);
    await assert.rejects(() => c.transferBatchDetailed([R('a', -5)], 100n), /non-positive/);
});

test('a total beyond safe integer range is refused rather than rounded', async () => {
    const { c } = stub();
    await assert.rejects(
        () => c.transferBatchDetailed([R('a', 2n ** 60n)], 100n),
        /exceeds safe integer range/);
});

test('failure stages are reported, with the transaction attached', async () => {
    for (const [failAt, stage] of [['create_transfer', 'create'],
                                   ['sign_transaction', 'sign'],
                                   ['submit_transaction', 'submit']]) {
        const { c } = stub();
        const inner = c._call.bind(c);
        c._call = async (m, p) => { if (m === failAt) throw new Error('nope'); return inner(m, p); };
        const err = await c.transferBatchDetailed([R('a', 1000)], 100n).catch(e => e);
        assert.equal(err.stage, stage, `${failAt} should report stage=${stage}`);
        if (stage !== 'create') assert.ok(err.unsigned, 'unsigned tx retained for diagnosis');
    }
});
