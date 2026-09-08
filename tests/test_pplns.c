/* The window -> payees split.
 *
 * This arithmetic decides what miners are paid, and until it was lifted into
 * pplns.c it lived in a static function in main.c where nothing could reach
 * it. Nothing here needs a chain: the point of the extraction is that an
 * expected split can be stated exactly instead of mined for.
 *
 * The cases that matter are the MIXED windows -- claims of very different
 * sizes, where some clear the payout floor and some do not. Those are exactly
 * the windows the regtest harness cannot produce (the regtest window holds
 * about two shares, because share difficulty is clamped to network
 * difficulty), so this is the only place the forfeit path is exercised
 * against numbers somebody chose.
 */

#include "../src/pplns.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define A "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"
#define B "bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3"
#define C "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2"
#define D "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy"

static int64_t sum_payees(const coinbase_payee_t *p, size_t n) {
    int64_t t = 0;
    for (size_t i = 0; i < n; ++i) t += p[i].sats;
    return t;
}

/* The whole block is spent, always. A coinbase that pays out less than it may
 * forfeits the difference to NOBODY -- not to the operator, not to a later
 * block, it is simply destroyed -- so the split must account for every
 * satoshi or the builder is right to refuse it. */
static void test_the_split_always_spends_the_whole_block(void) {
    const pplns_claim_t claims[] = {
        { A, 700.0, 0, 0.0 }, { B, 200.0, 0, 0.0 }, { C, 99.0, 0, 0.0 }, { D, 1.0, 0, 0.0 },
    };
    coinbase_payee_t out[4];
    pplns_split_t r;
    char err[256] = {0};
    CHECK(pplns_split_window(5000000000LL, 100, 1, claims, 4, 1000.0, 546,
                             out, 4, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 50000000LL);
    CHECK(r.payable_sats == 4950000000LL);
    CHECK(sum_payees(out, 4) == r.payable_sats);
    CHECK(sum_payees(out, 4) + r.fee_sats == 5000000000LL);
    printf("ok: the split spends the block exactly\n");
}

/* Proportional to difficulty, and in the order the store hands them over. */
static void test_each_claim_gets_its_difficulty_share(void) {
    const pplns_claim_t claims[] = { { A, 750.0, 0, 0.0 }, { B, 250.0, 0, 0.0 } };
    coinbase_payee_t out[2];
    pplns_split_t r;
    char err[256] = {0};
    CHECK(pplns_split_window(4000000000LL, 0, 0, claims, 2, 1000.0, 546,
                             out, 2, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 0);
    CHECK(out[0].sats == 3000000000LL);
    CHECK(out[1].sats == 1000000000LL);
    CHECK(strcmp(out[0].address, A) == 0);
    CHECK(strcmp(out[1].address, B) == 0);
    printf("ok: each claim gets its difficulty share\n");
}

/* Truncating division always leaves a few sats over. They go to the LARGEST
 * claim -- claims arrive largest-first -- rather than being dropped, because
 * dropping them would underpay the block and the builder would refuse it. */
static void test_the_rounding_remainder_goes_to_the_largest_claim(void) {
    /* Three equal claims of a reward that does NOT divide by three.
     * 100,000,002 does, which is how the first draft of this test passed
     * while asserting the wrong thing. */
    const pplns_claim_t claims[] = { { A, 1.0, 0, 0.0 }, { B, 1.0, 0, 0.0 }, { C, 1.0, 0, 0.0 } };
    coinbase_payee_t out[3];
    pplns_split_t r;
    char err[256] = {0};
    CHECK(pplns_split_window(100000000LL, 0, 0, claims, 3, 3.0, 546,
                             out, 3, &r, err, sizeof err) == 0);
    CHECK(sum_payees(out, 3) == 100000000LL);
    /* 33,333,333 each leaves 1 over, and it goes to the first. */
    CHECK(out[0].sats == 33333334LL);
    CHECK(out[1].sats == 33333333LL);
    CHECK(out[2].sats == 33333333LL);
    printf("ok: the rounding remainder lands on the largest claim\n");
}

/* THE MIXED WINDOW. A few big miners and a tail of small ones, which is what
 * a real pool looks like and what the regtest harness cannot build. The small
 * ones fall under the floor and will be forfeited -- and the split has to say
 * so BEFORE a block makes it real, because a count after the fact reports a
 * loss while a count now is something an operator can act on. */
static void test_a_mixed_window_predicts_who_the_floor_will_drop(void) {
    /* 3.125 BTC, no fee, so the arithmetic is exact and checkable by hand.
     * The tail claims are 1 part in 10 million: 312,500,000 * 1e-7 = 31 sats,
     * comfortably under the 546-sat dust floor. */
    const pplns_claim_t claims[] = {
        { A, 6000000.0, 0, 0.0 }, { B, 3999997.0, 0, 0.0 },
        { C, 2.0, 0, 0.0 }, { D, 1.0, 0, 0.0 },
    };
    coinbase_payee_t out[4];
    pplns_split_t r;
    char err[256] = {0};
    CHECK(pplns_split_window(312500000LL, 0, 0, claims, 4, 10000000.0, 546,
                             out, 4, &r, err, sizeof err) == 0);

    /* 60% of 312,500,000 is 187,500,000 on the nose -- but the other three
     * claims each truncate DOWN, leaving one satoshi over, and the remainder
     * rule puts it on the largest claim. So 187,500,001 is correct and
     * 187,500,000 would mean a satoshi had been destroyed. Asserting the
     * tidy-looking number here would have been asserting a bug. */
    CHECK(out[0].sats == 187500001LL);
    CHECK(out[1].sats == 124999906LL);           /* 39.99997%, truncated */
    CHECK(out[2].sats == 62LL);                  /* 2 parts in 1e7 */
    CHECK(out[3].sats == 31LL);                  /* 1 part in 1e7 */
    /* Both tail claims are under the floor, and the split says two. */
    CHECK(r.below_floor == 2);
    /* And the block is still fully spent -- the forfeit happens in the
     * builder, not here; this stage must not quietly drop anyone. */
    CHECK(sum_payees(out, 4) == 312500000LL);
    printf("ok: a mixed window predicts the two claims the floor will drop\n");
}

/* Raising the floor forfeits more of the window, and the prediction tracks
 * it. This is the knob an operator turns to trade coinbase bytes against how
 * small a miner they will serve, so it has to mean what it says. */
static void test_raising_the_floor_drops_more_claims(void) {
    const pplns_claim_t claims[] = {
        { A, 50.0, 0, 0.0 }, { B, 30.0, 0, 0.0 }, { C, 15.0, 0, 0.0 }, { D, 5.0, 0, 0.0 },
    };
    coinbase_payee_t out[4];
    pplns_split_t r;
    char err[256] = {0};
    /* 100,000,000 sats over 100 difficulty: 50M / 30M / 15M / 5M. */
    struct { int64_t floor; size_t expect; } CASES[] = {
        { 546,      0 },   /* everyone clears the dust limit */
        { 6000000,  1 },   /* the 5M claim goes */
        { 20000000, 2 },   /* and the 15M */
        { 40000000, 3 },   /* and the 30M; only the biggest is paid */
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
        CHECK(pplns_split_window(100000000LL, 0, 0, claims, 4, 100.0,
                                 CASES[i].floor, out, 4, &r,
                                 err, sizeof err) == 0);
        CHECK(r.below_floor == CASES[i].expect);
        /* Whatever the floor, the split itself still spends the block. */
        CHECK(sum_payees(out, 4) == 100000000LL);
    }
    printf("ok: raising the floor drops more claims, and says how many\n");
}

/* A floor below the dust limit is clamped UP to it, exactly as the builder
 * clamps it. If these two disagreed the pool would warn about one number and
 * pay by another, which is worse than not warning at all. */
static void test_the_floor_prediction_uses_the_builders_clamp(void) {
    const pplns_claim_t claims[] = { { A, 999.0, 0, 0.0 }, { B, 1.0, 0, 0.0 } };
    coinbase_payee_t out[2];
    pplns_split_t r;
    char err[256] = {0};
    /* The small claim comes to 100 sats: under 546, over 1. */
    CHECK(pplns_split_window(100000LL, 0, 0, claims, 2, 1000.0, 1,
                             out, 2, &r, err, sizeof err) == 0);
    CHECK(out[1].sats == 100LL);
    CHECK(r.below_floor == 1);      /* clamped to 546, not honoured as 1 */
    printf("ok: a sub-dust floor is clamped up, as the builder clamps it\n");
}

/* The exact boundaries, which is where a prediction and a payment come apart.
 *
 * Both rules here are "<" or ">=" in the builder, and a test built only from
 * comfortably-inside values cannot tell those from "<=" and ">". That matters
 * per miner: a claim worth exactly the floor IS paid, so predicting it as
 * forfeited would warn an operator about somebody who is about to be paid --
 * and the reverse would leave a miner unwarned about earning nothing. */
static void test_the_floor_and_dust_boundaries_are_exact(void) {
    coinbase_payee_t out[2];
    pplns_split_t r;
    char err[256] = {0};

    /* A claim worth EXACTLY the floor clears it. 546 parts in 1,000,000 of
     * 1,000,000 sats is 546 sats on the nose. */
    const pplns_claim_t at_floor[] = { { A, 999454.0, 0, 0.0 }, { B, 546.0, 0, 0.0 } };
    CHECK(pplns_split_window(1000000LL, 0, 0, at_floor, 2, 1000000.0, 546,
                             out, 2, &r, err, sizeof err) == 0);
    CHECK(out[1].sats == 546LL);
    CHECK(r.below_floor == 0);          /* exactly at the floor is PAID */

    /* One satoshi under it is not. */
    const pplns_claim_t under[] = { { A, 999455.0, 0, 0.0 }, { B, 545.0, 0, 0.0 } };
    CHECK(pplns_split_window(1000000LL, 0, 0, under, 2, 1000000.0, 546,
                             out, 2, &r, err, sizeof err) == 0);
    CHECK(out[1].sats == 545LL);
    CHECK(r.below_floor == 1);

    /* The fee's dust boundary, the same way. 1% of 54,600 is 546 exactly,
     * which is payable; 1% of 54,500 is 545, which is dust and dropped. */
    const pplns_claim_t one[] = { { A, 1.0, 0, 0.0 } };
    CHECK(pplns_split_window(54600LL, 100, 1, one, 1, 1.0, 546,
                             out, 1, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 546LL);
    CHECK(pplns_split_window(54500LL, 100, 1, one, 1, 1.0, 546,
                             out, 1, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 0LL);           /* 545 is dust: dropped entirely */
    CHECK(out[0].sats == 54500LL);      /* and the miner takes all of it */

    /* And the builder agrees about both, which is the point -- these two
     * compute the fee independently and one refuses the other's answer. */
    coinbase_parts_t parts;
    char berr[256] = {0};
    coinbase_payee_t whole[] = { { A, 54500LL } };
    CHECK(coinbase_build_window(800000, 54500LL, whole, 1, A, 100, NULL,
                                NULL, 4, 8, 0, 546, &parts, NULL,
                                berr, sizeof berr) == 0);
    coinbase_parts_free(&parts);
    printf("ok: the floor and dust boundaries are exact, and the builder agrees\n");
}

/* The fee rule has to match the builders' EXACTLY, dust rule included: they
 * refuse a split that does not sum to reward-minus-fee, so a disagreement
 * here means no coinbase renders at all. */
static void test_the_fee_matches_what_the_builder_will_expect(void) {
    const pplns_claim_t claims[] = { { A, 1.0, 0, 0.0 } };
    coinbase_payee_t out[1];
    pplns_split_t r;
    char err[256] = {0};

    CHECK(pplns_split_window(5000000000LL, 100, 1, claims, 1, 1.0, 546,
                             out, 1, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 50000000LL);

    /* A fee that would be dust is dropped entirely, not emitted as an
     * unrelayable output -- and the miner takes the whole reward. */
    CHECK(pplns_split_window(50000LL, 100, 1, claims, 1, 1.0, 546,
                             out, 1, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 0);                 /* 1% of 50,000 = 500 < 546 */
    CHECK(out[0].sats == 50000LL);

    /* No operator configured: no fee, whatever fee_bps says. */
    CHECK(pplns_split_window(5000000000LL, 100, 0, claims, 1, 1.0, 546,
                             out, 1, &r, err, sizeof err) == 0);
    CHECK(r.fee_sats == 0);
    printf("ok: the fee rule matches the builder's, dust included\n");
}

/* The split must be ACCEPTABLE TO THE BUILDER, which is the only fee test
 * that really counts.
 *
 * coinbase_build_window() computes the fee itself and refuses a split whose
 * payees do not sum to reward-minus-that-fee. So a one-satoshi disagreement
 * between these two -- floor vs ceiling division, say -- is not an off-by-one
 * in a report, it is a coinbase that never renders: refused on every
 * connection, on every job, with the pool quietly serving no work at all.
 *
 * Asserting the fee against a constant cannot see that, because the constants
 * anyone picks by hand tend to divide evenly. This drives the real builder
 * with rewards whose fee does NOT, so floor and ceiling differ and only the
 * matching rule survives. */
static void test_the_builder_accepts_what_the_splitter_produces(void) {
    /* Rewards chosen so reward*fee_bps/10000 has a remainder. */
    static const int64_t REWARDS[] = {
        5000000001LL, 312500007LL, 1000000003LL, 99999999LL, 654321LL,
    };
    static const int FEES[] = { 0, 1, 100, 250, 1000 };
    const pplns_claim_t claims[] = {
        { A, 7.0, 0, 0.0 }, { B, 3.0, 0, 0.0 }, { C, 1.0, 0, 0.0 },
    };
    for (size_t i = 0; i < sizeof REWARDS / sizeof REWARDS[0]; ++i) {
        for (size_t j = 0; j < sizeof FEES / sizeof FEES[0]; ++j) {
            coinbase_payee_t out[3];
            pplns_split_t r;
            char err[256] = {0};
            if (pplns_split_window(REWARDS[i], FEES[j], 1, claims, 3, 11.0,
                                   546, out, 3, &r, err, sizeof err) < 0) {
                continue;      /* refused for a stated reason; not our case */
            }
            /* The real builder, with the real fee rule, on the real split. */
            coinbase_parts_t parts;
            char berr[256] = {0};
            int rc = coinbase_build_window(800000, REWARDS[i], out, 3,
                                           A, FEES[j], NULL, "/sp/", 4, 8,
                                           0, 546, &parts, NULL,
                                           berr, sizeof berr);
            if (rc != 0) {
                printf("FAIL: reward=%lld fee_bps=%d — the builder refused "
                       "the splitter's own output: %s\n",
                       (long long)REWARDS[i], FEES[j], berr);
                failures++;
                continue;
            }
            coinbase_parts_free(&parts);
        }
    }
    printf("ok: the builder accepts every split the splitter produces\n");
}

/* A total smaller than the claims actually sum to is a broken window, not a
 * rounding artefact: dividing by it would pay out MORE than the block holds.
 * Refuse rather than hand the builder a split it will reject on every
 * connection. */
static void test_a_window_total_that_is_too_small_is_refused(void) {
    const pplns_claim_t claims[] = { { A, 60.0, 0, 0.0 }, { B, 60.0, 0, 0.0 } };
    coinbase_payee_t out[2];
    char err[256] = {0};
    CHECK(pplns_split_window(100000000LL, 0, 0, claims, 2, 100.0,
                             546, out, 2, NULL, err, sizeof err) < 0);
    CHECK(strstr(err, "exceed") != NULL);
    printf("ok: a window total smaller than its claims is refused\n");
}

static void test_the_degenerate_inputs_are_refused(void) {
    const pplns_claim_t claims[] = { { A, 1.0, 0, 0.0 } };
    coinbase_payee_t out[2];
    char err[256] = {0};
    CHECK(pplns_split_window(1000, 0, 0, NULL, 1, 1.0, 546, out, 2, NULL, err, sizeof err) < 0);
    CHECK(pplns_split_window(1000, 0, 0, claims, 0, 1.0, 546, out, 2, NULL, err, sizeof err) < 0);
    CHECK(pplns_split_window(1000, 0, 0, claims, 1, 0.0, 546, out, 2, NULL, err, sizeof err) < 0);
    CHECK(pplns_split_window(0,    0, 0, claims, 1, 1.0, 546, out, 2, NULL, err, sizeof err) < 0);
    /* More claims than the caller's array holds. */
    CHECK(pplns_split_window(1000, 0, 0, claims, 3, 1.0, 546, out, 2, NULL, err, sizeof err) < 0);
    printf("ok: degenerate inputs are refused, not divided\n");
}

/* ---- payment order ------------------------------------------------------ */

/* With nobody owed anything, the order is exactly what it was before the
 * ledger existed: largest claim first, so the floor and the byte budget fall
 * on the smallest. A pool that has always been able to pay everyone must not
 * behave differently for having gained a ledger it never uses. */
static void test_with_nothing_owed_the_order_is_largest_first(void) {
    pplns_claim_t c[5];
    double sizes[] = { 10, 50, 30, 5, 20 };
    for (int i = 0; i < 5; ++i) {
        c[i].payout_address = A; c[i].worker_id = i + 1;
        c[i].difficulty = sizes[i]; c[i].owed_fraction = 0.0;
    }
    size_t order[5];
    CHECK(pplns_order_claims(c, 5, 5, order) == 0);
    CHECK(c[order[0]].difficulty == 50);
    CHECK(c[order[1]].difficulty == 30);
    CHECK(c[order[2]].difficulty == 20);
    CHECK(c[order[3]].difficulty == 10);
    CHECK(c[order[4]].difficulty == 5);
    printf("ok: with nothing owed, the order is largest-first\n");
}

/* THE FAILURE THIS EXISTS FOR.
 *
 * Ranking by "claim plus what you are owed" does not move the queue: a large
 * miner's share of the current window is bigger than the largest debt a small
 * miner can ever build, so the same addresses take the same slots for ever.
 * Reserving slots outright is what fixes it, and this asserts that a
 * long-waiting small miner reaches a slot even when every large miner in the
 * window outweighs it many times over. */
static void test_a_long_waiting_small_miner_reaches_a_slot(void) {
    enum { N = 20, SLOTS = 4 };
    pplns_claim_t c[N];
    for (int i = 0; i < N; ++i) {
        c[i].payout_address = A; c[i].worker_id = i + 1;
        c[i].difficulty = 1000.0 / (i + 1);      /* i=0 is by far the largest */
        c[i].owed_fraction = 0.0;
    }
    /* The smallest miner in the window, owed a little from being skipped. Its
     * claim is 1/20th of the largest; no additive ranking would ever promote
     * it. */
    c[N - 1].owed_fraction = 0.004;

    size_t order[N];
    CHECK(pplns_order_claims(c, N, SLOTS, order) == 0);
    /* One slot of four is reserved, and it goes to the waiting miner. */
    CHECK(order[0] == N - 1);
    /* The rest of the slots still go to the largest claims, in order, so the
     * bulk of the block is not handed to the tail. */
    CHECK(order[1] == 0);
    CHECK(order[2] == 1);
    CHECK(order[3] == 2);
    printf("ok: a long-waiting small miner reaches a reserved slot\n");
}

/* Reserved slots are a minority of the coinbase, always. The biggest claims
 * are also the ones whose omission wastes the most block, so a rotation that
 * could take every slot would be worse than the problem it solves. */
static void test_the_reservation_never_takes_every_slot(void) {
    enum { N = 8 };
    pplns_claim_t c[N];
    for (int i = 0; i < N; ++i) {
        c[i].payout_address = A; c[i].worker_id = i + 1;
        c[i].difficulty = 100.0 - i;
        c[i].owed_fraction = 1.0;        /* everyone is owed something */
    }
    size_t order[N];
    for (size_t slots = 1; slots <= N; ++slots) {
        CHECK(pplns_order_claims(c, N, slots, order) == 0);
        /* Every claim appears exactly once, whatever the reservation did. */
        int seen[N] = {0};
        for (size_t i = 0; i < N; ++i) { CHECK(order[i] < N); seen[order[i]]++; }
        for (size_t i = 0; i < N; ++i) CHECK(seen[i] == 1);
        /* And at least one slot is still decided by claim size. */
        size_t reserved = (slots * PPLNS_RESERVED_SLOT_NUMERATOR)
                        / PPLNS_RESERVED_SLOT_DENOMINATOR;
        CHECK(reserved < slots);
    }
    printf("ok: the reservation never takes every slot\n");
}

/* A negative balance means "paid early, out of someone else's skipped share",
 * so it waits rather than jumping the queue. Only positive balances qualify
 * for a reserved slot. */
static void test_being_paid_early_does_not_win_a_reserved_slot(void) {
    enum { N = 6 };
    pplns_claim_t c[N];
    for (int i = 0; i < N; ++i) {
        c[i].payout_address = A; c[i].worker_id = i + 1;
        c[i].difficulty = 10.0 * (N - i);
        c[i].owed_fraction = -0.5;      /* everyone has been paid early */
    }
    size_t order[N];
    CHECK(pplns_order_claims(c, N, 4, order) == 0);
    /* Nobody is owed, so nothing is reserved and it is pure largest-first. */
    for (size_t i = 0; i < N; ++i) CHECK(order[i] == i);
    printf("ok: a negative balance does not win a reserved slot\n");
}

/* Randomised conservation check.
 *
 * The hand-written cases above pin splits somebody chose; this asserts the
 * invariant that has to hold for every split there is, across shapes nobody
 * thought to write down. It is the whole block or it is a bug: paid + fee ==
 * reward, exactly, with no float drift and no negative payee. */
static void test_conservation_holds_for_random_windows(void) {
    srand(20260908);
    for (int iter = 0; iter < 20000; ++iter) {
        pplns_claim_t claims[16];
        coinbase_payee_t out[16];
        size_t n = 1 + (size_t)(rand() % 16);
        double total = 0.0;
        for (size_t i = 0; i < n; ++i) {
            /* Wildly different magnitudes on purpose: a realistic window has
             * a few large miners and a long tail, and that is where a
             * proportional split loses satoshis if it is going to. */
            double d = (double)(rand() % 1000000) / (double)(1 + rand() % 1000);
            claims[i].payout_address = A;
            claims[i].worker_id = (int64_t)i + 1;
            claims[i].owed_fraction = 0.0;
            claims[i].difficulty = d;
            total += d;
        }
        /* Largest-first, as the store returns them. */
        for (size_t i = 0; i + 1 < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (claims[j].difficulty > claims[i].difficulty) {
                    pplns_claim_t t = claims[i]; claims[i] = claims[j]; claims[j] = t;
                }
        if (!(total > 0.0)) continue;

        int64_t reward = 546 + (int64_t)(rand() % 5000000000LL);
        int fee_bps = rand() % 1001;
        int have_op = rand() % 2;
        pplns_split_t r;
        char err[256] = {0};
        int rc = pplns_split_window(reward, fee_bps, have_op, claims, n, total,
                                    546, out, 16, &r, err, sizeof err);
        if (rc < 0) continue;      /* refusals are a valid answer; see above */

        int64_t paid = 0;
        for (size_t i = 0; i < n; ++i) {
            CHECK(out[i].sats >= 0);
            paid += out[i].sats;
        }
        if (paid + r.fee_sats != reward) {
            printf("FAIL: iter %d n=%zu reward=%lld paid=%lld fee=%lld\n",
                   iter, n, (long long)reward, (long long)paid,
                   (long long)r.fee_sats);
            failures++;
            return;
        }
    }
    printf("ok: conservation holds across 20000 random windows\n");
}

int main(void) {
    test_the_split_always_spends_the_whole_block();
    test_each_claim_gets_its_difficulty_share();
    test_the_rounding_remainder_goes_to_the_largest_claim();
    test_a_mixed_window_predicts_who_the_floor_will_drop();
    test_raising_the_floor_drops_more_claims();
    test_the_floor_prediction_uses_the_builders_clamp();
    test_the_fee_matches_what_the_builder_will_expect();
    test_the_floor_and_dust_boundaries_are_exact();
    test_the_builder_accepts_what_the_splitter_produces();
    test_a_window_total_that_is_too_small_is_refused();
    test_the_degenerate_inputs_are_refused();
    test_with_nothing_owed_the_order_is_largest_first();
    test_a_long_waiting_small_miner_reaches_a_slot();
    test_the_reservation_never_takes_every_slot();
    test_being_paid_early_does_not_win_a_reserved_slot();
    test_conservation_holds_for_random_windows();
    if (failures) { printf("test_pplns: %d FAILED\n", failures); return 1; }
    printf("test_pplns: all tests passed\n");
    return 0;
}
