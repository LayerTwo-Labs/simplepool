/* The window -> payees split. See pplns.h for why it is its own file. */

#include "pplns.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *errbuf, size_t errlen, const char *msg) {
    if (errbuf && errlen) snprintf(errbuf, errlen, "%s", msg);
}

int pplns_split_window(int64_t reward_sats, int fee_bps, int have_operator,
                       const pplns_claim_t *claims, size_t n_claims,
                       double total_diff, int64_t payout_floor_sats,
                       coinbase_payee_t *out, size_t cap,
                       pplns_split_t *res, char *errbuf, size_t errlen)
{
    pplns_split_t r;
    memset(&r, 0, sizeof r);
    if (res) *res = r;

    if (!claims || !out || n_claims == 0 || n_claims > cap) {
        set_err(errbuf, errlen, "no claims to split, or more than will fit");
        return -1;
    }
    if (!(total_diff > 0.0)) {
        set_err(errbuf, errlen, "window has no difficulty to divide by");
        return -1;
    }
    if (reward_sats <= 0) {
        set_err(errbuf, errlen, "the block pays nothing to divide");
        return -1;
    }

    /* Same fee rule as every coinbase builder, dust included. Duplicated
     * deliberately rather than shared: the builder REFUSES a split that does
     * not sum to reward-minus-fee, so this has to compute the identical
     * number, and a test that pins them together is worth more than a shared
     * helper that hides the coupling. test_pplns.c asserts the agreement. */
    int64_t fee = 0;
    if (have_operator && fee_bps > 0) {
        int64_t f = (reward_sats * (int64_t)fee_bps) / 10000;
        if (f >= COINBASE_DUST_SATS) fee = f;
    }
    int64_t payable = reward_sats - fee;
    if (payable <= 0) {
        set_err(errbuf, errlen, "nothing left after the operator fee");
        return -1;
    }

    int64_t assigned = 0;
    for (size_t i = 0; i < n_claims; ++i) {
        if (!(claims[i].difficulty >= 0.0)) {
            set_err(errbuf, errlen, "a claim has no difficulty");
            return -1;
        }
        out[i].address = claims[i].payout_address;
        out[i].sats = (int64_t)((double)payable *
                                (claims[i].difficulty / total_diff));
        if (out[i].sats < 0) {
            set_err(errbuf, errlen, "a claim divided to a negative amount");
            return -1;
        }
        assigned += out[i].sats;
    }

    /* The payees have to sum to `payable` EXACTLY or the builder refuses.
     *
     * Two ways they might not. Truncating division always leaves a few sats
     * short, and those go to the largest claim -- claims arrive
     * largest-first, so that is out[0]. And a caller that passed a total
     * smaller than the claims actually sum to would overshoot, which is not a
     * rounding artefact but a broken window: refuse rather than quietly
     * paying out more than the block holds. */
    if (assigned > payable) {
        set_err(errbuf, errlen,
                "claims exceed the window total they were divided by");
        return -1;
    }
    if (assigned < payable) out[0].sats += payable - assigned;

    /* Predict what the floor will forfeit, using the builder's own clamp so
     * the warning cannot disagree with the payment. */
    int64_t floor_sats = payout_floor_sats < COINBASE_DUST_SATS
                       ? COINBASE_DUST_SATS : payout_floor_sats;
    for (size_t i = 0; i < n_claims; ++i) {
        if (out[i].sats < floor_sats) r.below_floor++;
    }

    r.fee_sats = fee;
    r.payable_sats = payable;
    if (res) *res = r;
    return 0;
}
