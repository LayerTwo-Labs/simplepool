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
} pplns_claim_t;

typedef struct {
    int64_t fee_sats;            /* the operator's cut, off the top */
    int64_t payable_sats;        /* what the payees must sum to, exactly */
    /* How many claims are worth less than payout_floor_sats and will
     * therefore be forfeited to the operator by the builder.
     *
     * Computed here rather than left for the builder to discover because the
     * operator has to be told BEFORE a block makes it real -- a count after
     * the fact reports a loss, a count now is something they can act on. The
     * builder applies the floor itself; this only predicts it, using the same
     * clamp so the two cannot disagree. */
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
 * returns them, so the remainder lands on the strongest claim.
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
