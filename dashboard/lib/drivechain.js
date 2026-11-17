/* Drivechain status, as the overview shows it: which sidechains hold a slot
 * and whether they are being merge-mined, which proposals are in vote and
 * whether this pool is approving them, and which withdrawal bundles are
 * pending and whether this pool is backing them.
 *
 * Everything comes from the enforcer the pool builds its blocks with — the
 * validator for what is on the chain, the block producer for how this pool
 * votes — so the card reports the votes this pool actually casts rather than
 * a second copy of a policy somebody typed into the dashboard's environment.
 *
 * Polled on a timer, never per request: one pass is five RPCs plus two per
 * active sidechain, and the overview reloads every 15 seconds for every
 * visitor.
 */

import { enforcerRpc } from './enforcer.js';

const V  = 'cusf.mainchain.v1.ValidatorService';
const BP = 'cusf.mainchain.v1.BlockProducerService';

/* How many blocks back "is it being merge-mined" looks. Twelve is about two
 * hours: long enough that one unlucky gap does not read as a dead sidechain,
 * short enough that a dead one is noticed the same day. */
export const LIVENESS_WINDOW_BLOCKS = 12;

const RPC_TIMEOUT_MS = 10_000;

/* --- reading the enforcer's JSON ---------------------------------------- *
 * Field names arrive in camelCase or snake_case depending on the enforcer
 * build, and wrapper types as a bare value or as { value }, so read both —
 * the same allowance deposit-status.js makes. */
const snake = (k) => k.replace(/[A-Z]/g, c => '_' + c.toLowerCase());
function pick(o, key) {
    if (o == null || typeof o !== 'object') return undefined;
    return o[key] !== undefined ? o[key] : o[snake(key)];
}
function unwrap(v) {
    return (v != null && typeof v === 'object' && 'value' in v) ? v.value : v;
}
function num(o, key) {
    const v = unwrap(pick(o, key));
    if (v == null || v === '') return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
}
function str(o, key) {
    const v = unwrap(pick(o, key));
    return typeof v === 'string' && v.trim() !== '' ? v : null;
}
/* ConsensusHex / ReverseHex / Hex are all { hex }. */
function hex(o, key) {
    const v = pick(o, key);
    if (typeof v === 'string') return v.toLowerCase() || null;
    const h = unwrap(pick(v, 'hex'));
    return typeof h === 'string' && h !== '' ? h.toLowerCase() : null;
}
/* "ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS" -> "new-slots". Anything that is not a
 * recognised member — including UNSPECIFIED, and a number from a build that
 * emits enums as integers — is "unknown" rather than a guess. */
function policy(v, prefix, known) {
    if (typeof v !== 'string' || !v.startsWith(prefix)) return 'unknown';
    const p = v.slice(prefix.length).toLowerCase().replace(/_/g, '-');
    return known.includes(p) ? p : 'unknown';
}

function declaration(o) {
    const v0 = pick(pick(o, 'declaration'), 'v0');
    return { title: str(v0, 'title'), description: str(v0, 'description') };
}

/* --- BIP300 arithmetic --------------------------------------------------- *
 * Mirrors the enforcer (lib/validator/task/mod.rs): something passes once its
 * vote count is strictly greater than the threshold, it expires once its age
 * exceeds the max age, and it has already failed once it has missed more
 * blocks than the window can spare. A proposal for an occupied slot is held
 * to the withdrawal-bundle window, not the new-slot one. */
export function voteProgress({ votes, age, threshold, maxAge }) {
    if (votes == null || age == null || threshold == null || maxAge == null) return null;
    const needed      = threshold + 1;
    const blocksLeft  = Math.max(0, maxAge - age);
    const maxFails    = Math.max(0, maxAge - threshold);
    const fails       = Math.max(0, age - votes);
    return {
        needed,
        votes_missing:   Math.max(0, needed - votes),
        blocks_left:     blocksLeft,
        fails,
        max_fails:       maxFails,
        fails_remaining: Math.max(0, maxFails - fails),
        /* Still reachable if every remaining block votes for it. */
        can_still_pass:  votes + blocksLeft >= needed,
    };
}

/* Is this pool's block producer ACKing the proposal? true / false, or null
 * when the policy could not be read. */
export function approvingProposal(pol, proposal) {
    if (!pol || pol.ack_policy === 'unknown') return null;
    const explicit = pol.explicit_acks.some(a =>
        a.slot === proposal.slot && a.description_hash === proposal.description_hash);
    if (explicit) return true;
    if (pol.ack_policy === 'all')       return true;
    if (pol.ack_policy === 'new-slots') return !proposal.replaces_active;
    return false;
}

/* Is this pool upvoting the bundle? An M4 carries one upvote per sidechain,
 * so under "all" only a sidechain's first pending bundle is backed — and with
 * several pending the dashboard cannot tell which the producer treats as
 * first, so that case is null rather than a guess. "known" depends on whether
 * the enforcer holds the bundle's transaction, which it does not report. */
export function backingBundle(pol, bundle, pendingInSlot) {
    if (!pol || pol.bundle_policy === 'unknown') return null;
    const explicit = pol.explicit_bundle_acks.some(a =>
        a.slot === bundle.slot && a.m6id === bundle.m6id);
    if (explicit) return true;
    switch (pol.bundle_policy) {
        case 'none':  return false;
        case 'alarm': return false;
        case 'all':   return pendingInSlot === 1 ? true : null;
        default:      return null;
    }
}

/* --- one pass ------------------------------------------------------------ */

async function activity(rpc, slot, tipHash) {
    try {
        const j = await rpc(`${V}/GetBmmHStarCommitment`, {
            block_hash:    { hex: tipHash },
            sidechain_id:  slot,
            max_ancestors: LIVENESS_WINDOW_BLOCKS - 1,
        });
        const c = pick(j, 'commitment');
        if (!c) return { checked: false, error: 'tip not found by the enforcer' };
        /* Newest first: the tip, then its ancestors. */
        const seen = [c, ...(pick(c, 'ancestorCommitments') || [])]
            .map(x => hex(x, 'commitment') != null);
        const last = seen.indexOf(true);
        return {
            checked:          true,
            blocks_examined:  seen.length,
            commitments:      seen.filter(Boolean).length,
            blocks_since_last: last === -1 ? null : last,
        };
    } catch (e) {
        return { checked: false, error: (e.message || String(e)).slice(0, 200) };
    }
}

async function bundles(rpc, slot) {
    try {
        const j = await rpc(`${V}/GetWithdrawalBundleProposals`, { sidechain_id: slot });
        return {
            queried: true,
            bundles: (pick(j, 'proposals') || []).map(p => ({
                slot,
                m6id:            hex(p, 'm6id'),
                votes:           num(p, 'voteCount'),
                proposal_height: num(p, 'proposalHeight'),
            })),
        };
    } catch (e) {
        return { queried: false, bundles: [], error: (e.message || String(e)).slice(0, 200) };
    }
}

/* Build one snapshot. Throws only when the validator itself cannot be read —
 * the chain tip, the sidechains or the proposals. Everything else degrades in
 * place: an enforcer too old for GetChainInfo or GetBlockProducerState still
 * yields the tables, just without the countdowns or the "are we voting"
 * column. `rpc` is injectable for tests. */
export async function fetchDrivechain(enforcerAddr, { rpc } = {}) {
    const call = rpc || ((path, body) => enforcerRpc(enforcerAddr, path, body, RPC_TIMEOUT_MS));
    const soft = (p) => p.catch(() => null);

    const [tipJ, sidechainsJ, proposalsJ, chainJ, producerJ] = await Promise.all([
        call(`${V}/GetChainTip`, {}),
        call(`${V}/GetSidechains`, {}),
        call(`${V}/GetSidechainProposals`, {}),
        soft(call(`${V}/GetChainInfo`, {})),
        soft(call(`${BP}/GetBlockProducerState`, {})),
    ]);

    const header    = pick(tipJ, 'blockHeaderInfo');
    const tipHeight = num(header, 'height');
    const tipHash   = hex(header, 'blockHash');

    const k = pick(chainJ, 'bip300Constants');
    const consensus = k ? {
        bundle_max_age:        num(k, 'withdrawalBundleMaxAge'),
        bundle_threshold:      num(k, 'withdrawalBundleInclusionThreshold'),
        new_slot_max_age:      num(k, 'unusedSidechainSlotProposalMaxAge'),
        new_slot_threshold:    num(k, 'unusedSidechainSlotActivationThreshold'),
        activation_height:     num(k, 'activationHeight'),
    } : null;

    const pol = producerJ ? {
        ack_policy: policy(pick(producerJ, 'ackPolicy'), 'ACK_ALL_PROPOSALS_POLICY_',
                           ['none', 'new-slots', 'all']),
        bundle_policy: policy(pick(producerJ, 'withdrawalBundlePolicy'), 'WITHDRAWAL_BUNDLE_POLICY_',
                              ['none', 'known', 'all', 'alarm']),
        explicit_acks: (pick(producerJ, 'explicitAcks') || []).map(a => ({
            slot: num(a, 'sidechainNumber'),
            description_hash: hex(a, 'descriptionSha256dHash'),
        })),
        explicit_bundle_acks: (pick(producerJ, 'explicitBundleAcks') || []).map(a => ({
            slot: num(a, 'sidechainNumber'),
            m6id: hex(a, 'm6id'),
        })),
    } : null;

    const active = (pick(sidechainsJ, 'sidechains') || []).map(s => ({
        slot:              num(s, 'sidechainNumber'),
        ...declaration(s),
        votes:             num(s, 'voteCount'),
        proposal_height:   num(s, 'proposalHeight'),
        activation_height: num(s, 'activationHeight'),
    })).sort((a, b) => (a.slot ?? 1e9) - (b.slot ?? 1e9));
    const activeSlots = new Set(active.map(s => s.slot));

    const perSlot = await Promise.all(active.map(async s => {
        if (s.slot == null) return [{ checked: false }, { queried: false, bundles: [] }];
        return Promise.all([
            tipHash ? activity(call, s.slot, tipHash) : { checked: false },
            bundles(call, s.slot),
        ]);
    }));
    active.forEach((s, i) => { s.activity = perSlot[i][0]; });

    const proposals = (pick(proposalsJ, 'sidechainProposals') || []).map(p => {
        const slot  = num(p, 'sidechainNumber');
        const used  = activeSlots.has(slot);
        const votes = num(p, 'voteCount');
        const proposalHeight = num(p, 'proposalHeight');
        const age   = num(p, 'proposalAge')
            ?? (tipHeight != null && proposalHeight != null ? tipHeight - proposalHeight : null);
        const out = {
            slot,
            ...declaration(p),
            description_hash: hex(p, 'descriptionSha256dHash'),
            votes,
            proposal_height:  proposalHeight,
            age,
            replaces_active:  used,
            progress: consensus ? voteProgress({
                votes, age,
                threshold: used ? consensus.bundle_threshold : consensus.new_slot_threshold,
                maxAge:    used ? consensus.bundle_max_age   : consensus.new_slot_max_age,
            }) : null,
        };
        out.approving = approvingProposal(pol, out);
        return out;
    }).sort((a, b) => (a.slot ?? 1e9) - (b.slot ?? 1e9));

    const withdrawals = active.map((s, i) => {
        const w = perSlot[i][1];
        return {
            slot:    s.slot,
            title:   s.title,
            queried: w.queried,
            error:   w.error || null,
            bundles: w.bundles.map(b => {
                const age = tipHeight != null && b.proposal_height != null
                    ? tipHeight - b.proposal_height : null;
                return {
                    ...b,
                    age,
                    progress: consensus ? voteProgress({
                        votes: b.votes, age,
                        threshold: consensus.bundle_threshold,
                        maxAge:    consensus.bundle_max_age,
                    }) : null,
                    backing: backingBundle(pol, b, w.bundles.length),
                };
            }),
        };
    });

    return {
        network: typeof pick(chainJ, 'network') === 'string'
            ? pick(chainJ, 'network').replace(/^NETWORK_/, '').toLowerCase() : null,
        tip: { height: tipHeight, hash: tipHash, timestamp: num(header, 'timestamp') },
        consensus,
        policy: pol,
        liveness_window_blocks: LIVENESS_WINDOW_BLOCKS,
        active,
        proposals,
        withdrawals,
    };
}

/* --- background monitor --------------------------------------------------- */

let snapshot = null;      /* last successful pass */
let fetchedAt = null;     /* unix seconds of that pass */
let lastError = null;     /* message of the most recent failed pass, or null */
let timer = null;

/* What the page and /api/drivechain read. `data` is null until a pass has
 * succeeded; after that a failing enforcer leaves the last good snapshot in
 * place and sets `error`, so the card ages visibly rather than vanishing. */
export function currentDrivechain() {
    return {
        available:  snapshot != null,
        fetched_at: fetchedAt,
        error:      lastError,
        data:       snapshot,
    };
}

export function startDrivechainMonitor(enforcerAddr, { intervalMs = 30_000, fetcher = fetchDrivechain } = {}) {
    let running = false;
    const run = async () => {
        if (running) return;          /* a slow enforcer must not stack passes */
        running = true;
        try {
            snapshot  = await fetcher(enforcerAddr);
            fetchedAt = Math.floor(Date.now() / 1000);
            lastError = null;
        } catch (e) {
            lastError = (e.message || String(e)).slice(0, 200);
        } finally {
            running = false;
        }
    };
    const first = run();
    timer = setInterval(run, intervalMs);
    timer.unref?.();     /* never hold the process open */
    const stop = () => { clearInterval(timer); timer = null; };
    stop.ready = first;  /* tests await the first pass */
    return stop;
}

/* Tests only: forget what a previous monitor left behind. */
export function _resetDrivechain() {
    snapshot = null; fetchedAt = null; lastError = null;
}
