/* The block confirmation pass, and the PPLNS distribution that hangs off it.
 *
 * Lifted out of main.c to be linkable without it. main.c has main(), so
 * nothing in it can be reached from a test binary, and this is the code least
 * able to afford that: both of the worst bugs in the pplns work lived here and
 * both were caught by a three-minute regtest run because there was no faster
 * way to catch them.
 *
 *   - the pass only ran when the tip jumped two or more blocks, because it
 *     compared the new tip against the previous TEMPLATE height (tip + 1). On
 *     an ordinary one-block advance it never ran at all, so blocks stayed
 *     'pending' forever and pplns credited nobody.
 *   - distribution sat behind an early return taken whenever the backend
 *     served getblockhash. That state latches for the life of the process, so
 *     a pool on an ordinary bitcoind confirmed its blocks, counted them past
 *     maturity, and then never distributed one. Nothing looked wrong: only
 *     pps_credits stayed empty.
 *
 * Both are decisions about which of two confirmation mechanisms runs and what
 * still has to happen afterwards — no network needed to test either, once the
 * one call that does need a network is injectable. That is what block_hash_fn
 * is for.
 */
#ifndef SIMPLEPOOL_RECONCILE_H
#define SIMPLEPOOL_RECONCILE_H

#include "store.h"

#include <stdatomic.h>
#include <stddef.h>

/* Maturity, and the depth beyond which a candidate is not re-checked.
 *
 * 100 because that is when a coinbase output becomes spendable. Crediting
 * earlier would create a balance the pool genuinely cannot fund — which is the
 * reserve requirement PPLNS exists to remove, reintroduced by accident. It
 * also makes orphan handling a non-question: a block 100 deep is not coming
 * back out of the chain, so there is no credit to reverse and no need for a
 * reversal path that would otherwise have to exist. */
#define PPLNS_MATURITY_CONFS 100
#define BLOCK_FINAL_DEPTH    100

/* Bounded work per tick: re-checking every candidate over RPC on every tip
 * would be one call per settled row per tick. */
#define RECONCILE_MAX_PER_TICK 16

/* Look up the canonical hash at `height`. Returns 0 on success,
 * BITCOIND_ERR_UNSUPPORTED if the backend has no getblockhash at all, or any
 * other non-zero for a transient failure. In production this wraps
 * bitcoind_get_block_hash; a test supplies its own. */
typedef int (*block_hash_fn)(void *ctx, int height,
                             char *out, size_t out_len,
                             char *err, size_t err_len);

/* What the pass is allowed to do, and to what. */
typedef struct {
    store_t      *store;
    block_hash_fn get_block_hash;
    void         *get_block_hash_ctx;
    /* Tri-state, and it LATCHES: 0 = not yet asked, 1 = the backend serves
     * getblockhash, -1 = it does not and never will. Owned by the caller so it
     * survives across passes. */
    _Atomic int  *gbh_state;
    /* Distribution only happens in the pplns modes; every other mode has
     * either already paid the miner in the coinbase or priced the share when
     * it arrived. */
    int           pplns;
    int           fee_bps;
} reconcile_cfg_t;

/* What the pass did, for logging and for tests to assert against. */
typedef struct {
    int checked_via_node;     /* candidates settled from getblockhash */
    int ran_templates_pass;   /* the fallback ran */
    int ran_distribution;     /* store_pplns_distribute was called */
    int blocks_distributed;
    int worker_credits;
    int distribute_failed;    /* the call returned an error */
} reconcile_result_t;

/* One confirmation pass at `tip_height`. Safe to call with a NULL store or a
 * non-positive height; it does nothing and reports nothing. */
void reconcile_blocks_pass(const reconcile_cfg_t *cfg, int tip_height,
                           reconcile_result_t *out);

#endif /* SIMPLEPOOL_RECONCILE_H */
