/* The drivechain status card and the reading behind it.
 *
 * The card makes claims about how this pool votes, so the tests pin the
 * arithmetic to the enforcer's rules (strictly more votes than the
 * threshold; an occupied slot is held to the withdrawal-bundle window) and
 * pin the failure modes to "says unknown", never to a guess.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import ejs from 'ejs';

import * as fmt from '../lib/fmt.js';
import {
    fetchDrivechain, voteProgress, approvingProposal, backingBundle,
    startDrivechainMonitor, currentDrivechain, _resetDrivechain,
} from '../lib/drivechain.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const VIEWS = path.resolve(__dirname, '../views');

const TIP  = 'aa'.repeat(32);
const HASH = 'bb'.repeat(32);
const M6ID = 'cc'.repeat(32);
const decl = (title, description) => ({ v0: { title, description } });

/* A mainnet-shaped enforcer: two active sidechains, one proposal for an empty
 * slot and one that would replace slot 9, one pending bundle on slot 9. */
function enforcer(overrides = {}) {
    const replies = {
        'ValidatorService/GetChainTip': { blockHeaderInfo: { blockHash: { hex: TIP }, height: 1000, timestamp: '1794525983' } },
        'ValidatorService/GetChainInfo': { network: 'NETWORK_MAINNET', bip300Constants: {
            withdrawalBundleMaxAge: 12096, withdrawalBundleInclusionThreshold: 6048,
            unusedSidechainSlotProposalMaxAge: 2016, unusedSidechainSlotActivationThreshold: 1815,
            activationHeight: 100 } },
        'ValidatorService/GetSidechains': { sidechains: [
            { sidechainNumber: 24, declaration: decl('Elements+', 'Confidential assets'), voteCount: 1816, proposalHeight: 200, activationHeight: 300 },
            { sidechainNumber: 9,  declaration: decl('Thunder', 'Big blocks'), voteCount: 1816, proposalHeight: 100, activationHeight: 250 },
        ] },
        'ValidatorService/GetSidechainProposals': { sidechainProposals: [
            { sidechainNumber: 47, declaration: decl('Shuttle', 'A DEX'), descriptionSha256dHash: { hex: HASH },
              voteCount: 700, proposalHeight: 288, proposalAge: 712 },
            { sidechainNumber: 9, declaration: decl('Usurper', 'Replaces Thunder'), descriptionSha256dHash: { hex: 'dd'.repeat(32) },
              voteCount: 10, proposalHeight: 900, proposalAge: 100 },
        ] },
        'BlockProducerService/GetBlockProducerState': {
            ackPolicy: 'ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS',
            withdrawalBundlePolicy: 'WITHDRAWAL_BUNDLE_POLICY_NONE',
            explicitAcks: [],
            explicitBundleAcks: [{ sidechainNumber: 9, m6id: { hex: M6ID } }],
        },
        'ValidatorService/GetBmmHStarCommitment': (body) => body.sidechain_id === 9
            /* Thunder: missed the tip, committed in the two blocks before. */
            ? { commitment: { ancestorCommitments: [{ commitment: { hex: '01' } }, { commitment: { hex: '02' } }, {}] } }
            : { commitment: { ancestorCommitments: [{}, {}, {}] } },
        'ValidatorService/GetWithdrawalBundleProposals': (body) => body.sidechain_id === 9
            ? { proposals: [{ m6id: { hex: M6ID }, voteCount: 41, proposalHeight: 945 }] }
            : { proposals: [] },
        ...overrides,
    };
    const calls = [];
    const rpc = async (rpcPath, body) => {
        const key = rpcPath.replace('cusf.mainchain.v1.', '');
        calls.push({ key, body });
        const r = replies[key];
        if (r === undefined) throw new Error(`enforcer ${key}: unimplemented`);
        if (r instanceof Error) throw r;
        return typeof r === 'function' ? r(body) : r;
    };
    return { rpc, calls };
}

const render = (drivechain) => ejs.renderFile(
    path.join(VIEWS, 'partial/drivechain-status.ejs'),
    { ...fmt.all, drivechain }, { views: [VIEWS] });
const snap = (data, extra = {}) => ({
    available: true, fetched_at: Math.floor(Date.now() / 1000) - 5, error: null, data, ...extra });

test('a proposal passes on strictly more votes than the threshold', () => {
    const g = voteProgress({ votes: 700, age: 712, threshold: 1815, maxAge: 2016 });
    assert.equal(g.needed, 1816);
    assert.equal(g.blocks_left, 1304);
    assert.equal(g.fails, 12);
    assert.equal(g.max_fails, 201);
    assert.equal(g.fails_remaining, 189);
    assert.equal(g.can_still_pass, true);
    /* 202 misses is one more than the window can spare. */
    assert.equal(voteProgress({ votes: 0, age: 202, threshold: 1815, maxAge: 2016 }).can_still_pass, false);
    assert.equal(voteProgress({ votes: null, age: 1, threshold: 1, maxAge: 2 }), null);
});

test('the reading: sidechains, activity, proposals, bundles and our votes', async () => {
    const { rpc, calls } = enforcer();
    const d = await fetchDrivechain('unused', { rpc });

    assert.equal(d.network, 'mainnet');
    assert.deepEqual(d.tip, { height: 1000, hash: TIP, timestamp: 1794525983 });
    assert.deepEqual(d.active.map(s => s.slot), [9, 24], 'sorted by slot');
    assert.equal(d.active[0].title, 'Thunder');

    /* Activity is asked of the tip, for a 12-block window. */
    const bmm = calls.find(c => c.key.endsWith('GetBmmHStarCommitment') && c.body.sidechain_id === 9);
    assert.deepEqual(bmm.body, { block_hash: { hex: TIP }, sidechain_id: 9, max_ancestors: 11 });
    assert.deepEqual(d.active[0].activity,
        { checked: true, blocks_examined: 4, commitments: 2, blocks_since_last: 1 });
    assert.equal(d.active[1].activity.blocks_since_last, null);

    const [usurper, shuttle] = d.proposals;
    assert.equal(shuttle.slot, 47);
    assert.equal(shuttle.replaces_active, false);
    assert.equal(shuttle.progress.needed, 1816);
    assert.equal(shuttle.approving, true, 'new-slots approves an empty slot');
    /* An occupied slot is held to the bundle window and is not auto-approved. */
    assert.equal(usurper.replaces_active, true);
    assert.equal(usurper.progress.needed, 6049);
    assert.equal(usurper.progress.blocks_left, 11996);
    assert.equal(usurper.approving, false);

    const thunder = d.withdrawals.find(w => w.slot === 9);
    assert.equal(thunder.bundles.length, 1);
    assert.equal(thunder.bundles[0].age, 55);
    assert.equal(thunder.bundles[0].progress.needed, 6049);
    assert.equal(thunder.bundles[0].backing, true, 'hand-picked under policy none');
    assert.deepEqual(d.withdrawals.find(w => w.slot === 24).bundles, []);
});

test('snake_case fields and { value } wrappers read the same', async () => {
    const { rpc } = enforcer({
        'ValidatorService/GetChainTip': { block_header_info: { block_hash: { hex: { value: TIP } }, height: 7 } },
        'ValidatorService/GetSidechains': { sidechains: [
            { sidechain_number: { value: 3 }, declaration: { v0: { title: { value: 'Snake' } } }, activation_height: { value: 5 } },
        ] },
        'ValidatorService/GetSidechainProposals': {},
    });
    const d = await fetchDrivechain('unused', { rpc });
    assert.equal(d.tip.height, 7);
    assert.equal(d.tip.hash, TIP);
    assert.deepEqual([d.active[0].slot, d.active[0].title, d.active[0].activation_height], [3, 'Snake', 5]);
});

test('how we vote, policy by policy', () => {
    const base = { explicit_acks: [], explicit_bundle_acks: [], bundle_policy: 'none' };
    const fresh = { slot: 47, description_hash: HASH, replaces_active: false };
    const evict = { slot: 9,  description_hash: HASH, replaces_active: true };
    assert.equal(approvingProposal({ ...base, ack_policy: 'all' }, evict), true);
    assert.equal(approvingProposal({ ...base, ack_policy: 'new-slots' }, fresh), true);
    assert.equal(approvingProposal({ ...base, ack_policy: 'new-slots' }, evict), false);
    assert.equal(approvingProposal({ ...base, ack_policy: 'none' }, fresh), false);
    assert.equal(approvingProposal({ ...base, ack_policy: 'none',
        explicit_acks: [{ slot: 47, description_hash: HASH }] }, fresh), true);
    /* An explicit ack is for one description, not for whatever lands in the slot. */
    assert.equal(approvingProposal({ ...base, ack_policy: 'none',
        explicit_acks: [{ slot: 47, description_hash: 'ee'.repeat(32) }] }, fresh), false);
    assert.equal(approvingProposal({ ...base, ack_policy: 'unknown' }, fresh), null);
    assert.equal(approvingProposal(null, fresh), null);

    const b = { slot: 9, m6id: M6ID };
    const bp = (bundle_policy) => ({ ...base, ack_policy: 'none', bundle_policy });
    assert.equal(backingBundle(bp('none'), b, 1), false);
    assert.equal(backingBundle(bp('alarm'), b, 1), false);
    assert.equal(backingBundle(bp('all'), b, 1), true);
    assert.equal(backingBundle(bp('all'), b, 2), null, 'only one of several is backed, and we cannot tell which');
    assert.equal(backingBundle(bp('known'), b, 1), null);
    assert.equal(backingBundle(null, b, 1), null);
});

test('an older enforcer still yields the tables, with the gaps marked', async () => {
    const { rpc } = enforcer({
        'ValidatorService/GetChainInfo': new Error('unimplemented'),
        'BlockProducerService/GetBlockProducerState': new Error('unimplemented'),
        'ValidatorService/GetWithdrawalBundleProposals': new Error('unimplemented'),
        'ValidatorService/GetBmmHStarCommitment': new Error('boom'),
    });
    const d = await fetchDrivechain('unused', { rpc });
    assert.equal(d.consensus, null);
    assert.equal(d.policy, null);
    assert.equal(d.proposals[0].progress, null);
    assert.equal(d.proposals[0].approving, null);
    assert.deepEqual(d.active[0].activity, { checked: false, error: 'boom' });
    assert.equal(d.withdrawals[0].queried, false);

    const html = await render(snap(d));
    assert.match(html, /does not report its block-producer policy/);
    assert.match(html, /could not check/);
    assert.match(html, /Could not ask the enforcer for pending withdrawal bundles/);
    assert.match(html, /unknown<\/span>/);
});

test('a validator that cannot be read is an error, not an empty chain', async () => {
    const { rpc } = enforcer({ 'ValidatorService/GetSidechains': new Error('connection refused') });
    await assert.rejects(fetchDrivechain('unused', { rpc }), /connection refused/);
});

test('the card: counts in the summary, votes in the tables', async () => {
    const { rpc } = enforcer();
    const html = await render(snap(await fetchDrivechain('unused', { rpc })));
    assert.match(html, /<details class="card" id="drivechain">/);
    assert.match(html, /2 sidechains active/);
    assert.match(html, /· 2 in vote/);
    assert.match(html, /· 1 withdrawal pending/);
    assert.match(html, /approve proposals for empty slots/);
    assert.match(html, /1 bundle backed by hand/);
    assert.match(html, /1 block ago/);
    assert.match(html, /none in 4 blocks/);
    assert.match(html, /would replace the sidechain in this slot/);
    assert.match(html, /700 <span class="muted">\/ 1,816<\/span>/);
    assert.match(html, /41 <span class="muted">\/ 6,049<\/span>/);
    assert.match(html, new RegExp(`title="m6id ${M6ID}"`));
    assert.doesNotMatch(html, /showing the last reading/);
});

test('sidechain-supplied text is escaped', async () => {
    const { rpc } = enforcer({
        'ValidatorService/GetSidechains': { sidechains: [
            { sidechainNumber: 1, declaration: decl('<script>x</script>', '"><img src=x>') }] },
        'ValidatorService/GetSidechainProposals': {},
    });
    const html = await render(snap(await fetchDrivechain('unused', { rpc })));
    assert.doesNotMatch(html, /<script>x|<img src=x>/);
    assert.match(html, /&lt;script&gt;x/);
});

test('no card before the first reading; a stale one says so afterwards', async () => {
    assert.equal((await render({ available: false, fetched_at: null, error: 'refused', data: null })).trim(), '');
    assert.equal((await render(undefined)).trim(), '');

    const { rpc } = enforcer();
    const html = await render(snap(await fetchDrivechain('unused', { rpc }),
        { fetched_at: Math.floor(Date.now() / 1000) - 600, error: 'fetch failed' }));
    assert.match(html, /stale, 10m ago/);
    assert.match(html, /showing the last reading/);
    assert.match(html, /\(fetch failed\)/);
});

test('the monitor keeps the last good reading when the enforcer goes away', async () => {
    _resetDrivechain();
    let fail = false;
    const fetcher = async () => { if (fail) throw new Error('down'); return { marker: 1 }; };
    const stop = startDrivechainMonitor('unused', { intervalMs: 5, fetcher });
    try {
        await stop.ready;
        assert.deepEqual(currentDrivechain().data, { marker: 1 });
        assert.equal(currentDrivechain().error, null);
        fail = true;
        await new Promise(r => setTimeout(r, 40));
        const c = currentDrivechain();
        assert.equal(c.available, true);
        assert.deepEqual(c.data, { marker: 1 });
        assert.equal(c.error, 'down');
    } finally {
        stop();
        _resetDrivechain();
    }
});
