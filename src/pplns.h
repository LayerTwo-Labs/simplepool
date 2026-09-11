#ifndef SIMPLEPOOL_PPLNS_H
#define SIMPLEPOOL_PPLNS_H

/* Turning a PPLNS window into coinbase payees.
 *
 * Its own file for the same reason reconcile.c is: this is arithmetic that
 * decides what people are paid, and it lived inside a static function in
 * main.c where nothing could reach it. A bug here does not crash and does not
 * show up in a log -- it pays somebody the wrong amount, or pays them nothing,
 * which is the failure mode this whole rail has to be trusted not to have.
 *
 * Deliberately pure: no store, no template, no config struct, no logging. It
 * takes numbers and returns numbers, so a test can state an expected split
 * exactly instead of building a chain to find out. main.c keeps the parts
 * that genuinely need the world -- querying the window, reading the reward
 * out of the template, and saying what happened. */

#include <stddef.h>
#include <stdint.h>

#include "coinbase.h"

/* One miner's claim on the window, as the store reports it. */
typedef struct {
    const char *payout_address;
    double      difficulty;      /* this worker's share of the window */
    int64_t     worker_id;       /* for the fraction ledger; 0 if unknown */
    /* What this worker is owed from previous blocks, as a signed fraction of
     * ONE block reward. Positive: skipped before, and first in the queue for
     * a slot now. Negative: paid early out of somebody else's skipped share,
     * so it waits. Zero across a pool that has always been able to pay
     * everyone. See pplns_order_claims(). */
    double      owed_fraction;
} pplns_claim_t;

/* How many of the coinbase's payout slots are held back for whoever has been
 * waiting longest, rather than given to the largest claims.
 *
 * Not a tuning knob so much as the thing that makes the queue move at all. A
 * large miner's share of the current window is bigger than the largest debt a
 * small miner can ever accumulate, so ranking by "claim plus what you are
 * owed" still hands every slot to the same addresses, every block, for ever.
 * Measured on a production coinbase-direct pool: over 31 blocks, 279 payout
 * slots reached 34 addresses, 12 of which took 91% of them, while 88
 * addresses were paid nothing — and 28 of those cleared the payout floor
 * comfortably, so the floor was not what excluded them
 * (LayerTwo-Labs/simplepool#76).
 *
 * Reserving slots costs no coinbase bytes and changes nobody's total. It
 * changes how OFTEN people are paid, not how much. */
#define PPLNS_RESERVED_SLOT_NUMERATOR   1
#define PPLNS_RESERVED_SLOT_DENOMINATOR 4    /* a quarter of the slots */

/* Order `claims` into the sequence the coinbase should pay them in, writing
 * the permutation into `order` (indices into `claims`).
 *
 * The default is largest claim first, which puts the payout floor and the
 * byte budget on the smallest claims — the ones for whom missing a block
 * costs least. Then a fraction of the slots the coinbase is expected to have
 * room for are handed instead to the workers with the largest positive
 * `owed_fraction`, longest-waiting first.
 *
 * `expected_slots` is how many payouts the caller believes will fit. It only
 * decides how many slots are reserved. Erring LOW is safe -- a smaller
 * reservation just rotates more slowly -- and erring high is not: reserve
 * more positions than the coinbase has room for and every slot it does have
 * goes to the queue, so the largest claims are paid nothing and immediately
 * re-enter the queue themselves. A caller with several coinbase budgets in
 * play (a per-listener ceiling) must therefore size this from the TIGHTEST
 * of them.
 *
 * `amounts` is what each claim is worth, aligned with `claims`, as
 * pplns_split_window() computed it; NULL means "assume every claim is
 * payable". A claim worth less than `payout_floor_sats` (clamped up to
 * COINBASE_DUST_SATS, as everywhere else) cannot be paid by this block at any
 * position, so it is not given a reserved slot -- it would hold the slot
 * against a miner that could actually use it, and a permanently sub-floor
 * miner accumulates `owed_fraction` for ever while a byte-capped one is paid
 * and resets, so over a long enough run the miners who can NEVER be paid
 * crowd out the ones the rotation exists for (LayerTwo-Labs/simplepool#76).
 * It still appears in `order` at its normal largest-first position: the
 * permutation always covers every claim, because the ledger and the
 * redistribution both need the ones that were skipped.
 *
 * Returns 0, or negative on bad input. */
int pplns_order_claims(const pplns_claim_t *claims, size_t n_claims,
                       size_t expected_slots,
                       const coinbase_payee_t *amounts,
                       int64_t payout_floor_sats,
                       size_t *order);

typedef struct {
    int64_t fee_sats;            /* the operator's cut, off the top */
    int64_t payable_sats;        /* what the payees must sum to, exactly */
    /* How many claims are worth less than payout_floor_sats and will
     * therefore be skipped by the builder, their value going to the miners it
     * could pay. (Not to the operator: that was the rule until #76, and the
     * long note at the redistribution in coinbase.c has the measurement that
     * ended it.)
     *
     * Computed here rather than left for the builder to discover because the
     * operator has to be told BEFORE a block makes it real -- a count after
     * the fact reports a loss, a count now is something they can act on. The
     * builder applies the floor itself; this only predicts it, using the same
     * clamp so the two cannot disagree.
     *
     * When it reaches n_claims the builder will refuse the window outright:
     * it has nobody to pay. A caller must not publish a template it predicts
     * that for -- the refusal lands per connection, per job, and the pool
     * simply stops serving work. See attach_pplns_window() in main.c. */
    size_t  below_floor;
} pplns_split_t;

/* Divide `reward_sats` across `claims` in proportion to difficulty.
 *
 * The fee comes off the top exactly as every coinbase builder computes it,
 * including the dust rule -- a fee below COINBASE_DUST_SATS is dropped rather
 * than emitted as an unrelayable output. The remaining payable amount is
 * split by difficulty share, and the payees are then guaranteed to sum to it
 * EXACTLY: truncating division leaves a few sats over, and they go to the
 * largest claim rather than being dropped, because a coinbase that pays out
 * less than it may forfeits the difference to nobody.
 *
 * `claims` must be ordered largest-difficulty-first, as store_pplns_window()
 * returns them, so the truncation remainder lands on the strongest claim.
 * Split FIRST and reorder afterwards: pplns_order_claims() needs these amounts
 * to know which claims the floor will drop, and a reordering does not change
 * a single one of them -- only which index carries the remainder. Splitting
 * the reordered claims instead put the remainder on whoever the queue had
 * promoted, which is at most a satoshi per claim but is also not what this
 * says it does.
 *
 * Nothing downstream may assume out[0] is the largest payee once the caller
 * has reordered: coinbase.c does not, and scans for the largest surviving
 * output when it redistributes.
 *
 * `total_diff` is the window's total as the store reported it, passed in
 * rather than re-summed here.
 *
 * It covers EXACTLY the claims in `claims`, truncation included: when
 * store_pplns_window() cannot fit the whole window it drops the tail from the
 * total as well as from the entries, so the survivors divide the block between
 * them rather than funding an output that is never created. This comment used
 * to say the opposite -- that the total still counted truncated rows -- which
 * store.h and store.c both contradict (LayerTwo-Labs/simplepool#76). Believing
 * the old version would make a denominator larger than the claims sum, every
 * payee would be shorted, and the whole shortfall would land on out[0] via the
 * remainder rule: the largest miner silently absorbing everyone else's. The
 * `assigned > payable` check below catches the opposite error only.
 *
 * Returns 0 on success, negative on error (errbuf populated). */
int pplns_split_window(int64_t reward_sats, int fee_bps, int have_operator,
                       const pplns_claim_t *claims, size_t n_claims,
                       double total_diff, int64_t payout_floor_sats,
                       coinbase_payee_t *out, size_t cap,
                       pplns_split_t *res, char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_PPLNS_H */
