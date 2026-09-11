/* The block confirmation pass. See reconcile.h for why it is its own file. */

#include "reconcile.h"
#include "bitcoind.h"
#include "log.h"

/* strcasecmp lives in <strings.h> on glibc and in <string.h> on macOS, so the
 * macOS build compiles happily without the first and Linux does not. Include
 * both rather than the one this machine happens to need. */
#include <string.h>
#include <strings.h>

void reconcile_blocks_pass(const reconcile_cfg_t *cfg, int tip_height,
                           reconcile_result_t *out)
{
    reconcile_result_t r = {0};
    if (out) *out = r;
    if (!cfg || !cfg->store || tip_height <= 0) return;

    /* Whether this pass has already settled candidate statuses, so the
     * templates fallback below must not run and overwrite them. NOT a reason
     * to skip the distribution at the end -- see there. */
    int settled = 0;

    if (cfg->gbh_state && atomic_load(cfg->gbh_state) >= 0 && cfg->get_block_hash) {
        store_block_candidate_t cands[RECONCILE_MAX_PER_TICK];
        int n = store_list_unresolved_blocks(cfg->store, tip_height,
                                             BLOCK_FINAL_DEPTH, cands,
                                             RECONCILE_MAX_PER_TICK);
        for (int i = 0; i < n; ++i) {
            char have[80] = {0};
            char gerr[256] = {0};
            int rc = cfg->get_block_hash(cfg->get_block_hash_ctx,
                                         cands[i].height, have, sizeof have,
                                         gerr, sizeof gerr);
            if (rc == BITCOIND_ERR_UNSUPPORTED) {
                atomic_store(cfg->gbh_state, -1);
                LOG_INFO("backend does not serve getblockhash — confirming "
                         "blocks from the observed chain of template tips "
                         "instead");
                break;
            }
            if (rc != 0) {
                /* Transient. Leave the rows alone and retry on the next tip
                 * rather than recording a verdict we did not get. Statuses
                 * are untouched, so the templates pass must not run either --
                 * but this is not a reason to stop paying: distribution reads
                 * only rows settled by an earlier pass, and a backend that
                 * kept failing this one call would otherwise silently stop
                 * crediting anyone. */
                LOG_WARN("getblockhash(%d) failed: %s", cands[i].height, gerr);
                settled = 1;
                break;
            }
            atomic_store(cfg->gbh_state, 1);
            int match = strcasecmp(have, cands[i].hash) == 0;
            store_set_block_status(cfg->store, cands[i].hash,
                                   match ? STORE_BLOCK_CONFIRMED
                                         : STORE_BLOCK_ORPHANED,
                                   match ? tip_height - cands[i].height + 1 : 0,
                                   "node");
            r.checked_via_node++;
            if (!match) {
                LOG_WARN("block %s at height %d is no longer in the chain — "
                         "marked orphaned", cands[i].hash, cands[i].height);
            }
        }
        /* getblockhash is the node's own answer, so where the backend has
         * one it wins outright and the templates fallback below is skipped
         * -- running both would have the weaker check overwrite the stronger
         * one's verdict and its checked_via. */
        if (atomic_load(cfg->gbh_state) > 0) settled = 1;
    }

    if (!settled) {
        int confirmed = 0, orphaned = 0, pending = 0;
        r.ran_templates_pass = 1;
        if (store_reconcile_blocks_from_templates(cfg->store, tip_height,
                                                  &confirmed, &orphaned,
                                                  &pending) == 0) {
            LOG_DEBUG("block reconcile: confirmed=%d orphaned=%d pending=%d",
                      confirmed, orphaned, pending);
        }
    }

    /* PPLNS pays out here rather than at block-find time, because this is the
     * only place that knows a block is still in the chain and how deep. A
     * distribution is the last irreversible step in the pipeline: crediting is
     * additive and there is no negative share, so anything credited from a
     * block that later turns out not to be ours cannot be taken back. Running
     * it off the confirmation pass, gated on maturity, means it only ever sees
     * blocks that are 100 deep — by which point "still in the chain" has
     * stopped being a question.
     *
     * ⚠️ This must stay on the function's single exit path, reached however
     * the statuses above were settled. It used to sit behind an early return
     * taken whenever the backend served getblockhash — and since that state
     * latches on for the life of the process, a pool on a getblockhash-capable
     * node confirmed its blocks, counted them past 100 deep, and then never
     * distributed one. Nothing looked wrong: the rows carry a window, a
     * status and the depth, and only pps_credits stays empty. Do not add a
     * `return` above this without moving it.
     *
     * test_reconcile.c pins it from both directions. */
    /* Settle the coinbase-direct fraction ledger on the same pass, and for
     * the same reason: this is the only place that knows whether a block is
     * still in the chain. A block whose coinbase skipped somebody moves them
     * up the queue -- but only if that block actually stood. Orphaned ones
     * have their staged rows discarded, because their coinbase paid nobody
     * and rotated nobody.
     *
     * Runs whatever the mode, because a pool switched away from
     * pplns-coinbase still has rows to settle or discard from when it was. */
    {
        int applied = 0, discarded = 0;
        char ferr[256] = {0};
        if (store_settle_block_fractions(cfg->store, &applied, &discarded,
                                         ferr, sizeof ferr) < 0) {
            LOG_WARN("pplns-coinbase: could not settle the payout queue: %s — "
                     "the staged rows stay and the next tip retries",
                     ferr[0] ? ferr : "unknown");
        } else if (applied || discarded) {
            LOG_INFO("pplns-coinbase: payout queue settled for %d confirmed "
                     "block(s); discarded %d orphaned", applied, discarded);
        }
        r.fractions_applied = applied;
        r.fractions_discarded = discarded;
    }

    if (cfg->pplns) {
        int blocks = 0, workers = 0;
        char derr[256] = {0};
        r.ran_distribution = 1;
        int rc = store_pplns_distribute(cfg->store, PPLNS_MATURITY_CONFS,
                                        cfg->fee_bps, &blocks, &workers,
                                        derr, sizeof derr);
        if (rc < 0) {
            r.distribute_failed = 1;
            LOG_WARN("pplns distribution failed: %s — nothing was credited, "
                     "the block stays undistributed and the next tip retries",
                     derr[0] ? derr : "unknown");
        } else if (blocks > 0) {
            LOG_INFO("pplns: distributed %d matured block(s) across %d "
                     "worker credit(s)", blocks, workers);
        }
        r.blocks_distributed = blocks;
        r.worker_credits = workers;
    }

    if (out) *out = r;
}
