/* The confirmation pass, and the PPLNS distribution that hangs off it.
 *
 * These exist because the two most expensive bugs in the pplns work both lived
 * in this code and neither could be caught by a unit test: it was inside
 * main.c, which has main(), so nothing in it could be linked into a test
 * binary. Both were found by a three-minute regtest run instead. reconcile.c
 * is that code lifted out, with the one call that needs a network — the block
 * hash lookup — injectable.
 *
 * The bug worth the most attention here: distribution used to sit behind an
 * early return taken whenever the backend served getblockhash. That state
 * latches for the life of the process, so a pool on an ordinary bitcoind
 * confirmed its blocks, counted them past maturity, and then never
 * distributed one. Nothing looked wrong from the outside — the rows carry a
 * window, a status and the depth, and only pps_credits stays empty.
 */

#include "reconcile.h"
#include "bitcoind.h"
#include "store.h"
#include "log.h"

#include <sqlite3.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks = 0, failures = 0;
#define CHECK(cond) do {                                                      \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                         \
} while (0)

#define MAX_TEST_DBS 16
static char g_db_paths[MAX_TEST_DBS][256];
static int  g_db_count = 0;

static const char *fresh_db_path(void) {
    assert(g_db_count < MAX_TEST_DBS && "MAX_TEST_DBS too small - raise it");
    char *p = g_db_paths[g_db_count++];
    snprintf(p, 256, "/tmp/reconcile_test_%d_%d.db", (int)getpid(), g_db_count);
    unlink(p);
    char wal[300], shm[300];
    snprintf(wal, sizeof wal, "%s-wal", p);
    snprintf(shm, sizeof shm, "%s-shm", p);
    unlink(wal); unlink(shm);
    return p;
}

static void cleanup_dbs(void) {
    for (int i = 0; i < g_db_count; ++i) {
        unlink(g_db_paths[i]);
        char wal[300], shm[300];
        snprintf(wal, sizeof wal, "%.255s-wal", g_db_paths[i]);
        snprintf(shm, sizeof shm, "%.255s-shm", g_db_paths[i]);
        unlink(wal); unlink(shm);
    }
}

static int64_t scalar_i64(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    int64_t v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

/* ---- injected block-hash lookups --------------------------------------- */

/* The backend has no getblockhash. This is every enforcer, and therefore the
 * production configuration -- which is exactly why testing only against it
 * left the other branch broken for a whole commit. */
static int gbh_unsupported(void *ctx, int height, char *out, size_t out_len,
                           char *err, size_t err_len) {
    (void)ctx; (void)height; (void)out; (void)out_len; (void)err; (void)err_len;
    return BITCOIND_ERR_UNSUPPORTED;
}

/* The backend serves it, and agrees the candidate is in the chain. */
static int gbh_agrees(void *ctx, int height, char *out, size_t out_len,
                      char *err, size_t err_len) {
    (void)height; (void)err; (void)err_len;
    snprintf(out, out_len, "%s", (const char *)ctx);
    return 0;
}

/* The backend serves it and is reachable, but the hash it returns is not
 * ours: the candidate was reorged out. */
static int gbh_disagrees(void *ctx, int height, char *out, size_t out_len,
                         char *err, size_t err_len) {
    (void)ctx; (void)height; (void)err; (void)err_len;
    snprintf(out, out_len, "%s", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    return 0;
}

static int gbh_transient_calls = 0;
static int gbh_transient(void *ctx, int height, char *out, size_t out_len,
                         char *err, size_t err_len) {
    (void)ctx; (void)height; (void)out; (void)out_len;
    gbh_transient_calls++;
    snprintf(err, err_len, "connection refused");
    return -1;
}

/* ---- fixtures ----------------------------------------------------------- */

/* A pool that found one block, already confirmed and matured, with a window
 * and one worker holding all of it. Everything the distributor needs, and
 * nothing it does not. */
static void seed_matured_block(store_t *s, const char *hash) {
    for (int i = 0; i < 10; ++i) {
        assert(store_record_share_addr(s, "alice", "addr_a",
                                       1000ULL + (uint64_t)i, 5.0,
                                       0, NULL, 0, 0.0) == 0);
    }
    assert(store_record_share_addr(s, "alice", "addr_a", 1100, 0.0,
                                   1, hash, 0, 0.0) == 0);
    /* window 50 = the ten shares above; gross 100000 sats. */
    assert(store_record_block(s, 1100, 800100, hash, "alice", "addr_a",
                              100000, 0, STORE_BLOCK_PENDING, NULL, 50.0) == 0);
    assert(store_flush(s) == 0);
    assert(store_set_block_status(s, hash, STORE_BLOCK_CONFIRMED, 150, "node") == 0);
}

/* A candidate still shallow enough to be re-checked.
 *
 * Needed because a MATURED block is deliberately not a candidate:
 * store_list_unresolved_blocks stops at BLOCK_FINAL_DEPTH, so a block at 150
 * confirmations is never looked up again. Without one of these the lookup is
 * never called at all, gbh_state never moves, and a test meaning to exercise
 * the getblockhash branch silently exercises the templates one instead. */
static void seed_shallow_candidate(store_t *s, const char *hash, int height) {
    assert(store_record_block(s, 1200, height, hash, "alice", "addr_a",
                              100000, 0, STORE_BLOCK_PENDING, NULL, 0.0) == 0);
    assert(store_flush(s) == 0);
}

static store_t *open_store(const char *path) {
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof cfg.path, "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 500;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);
    return s;
}

/* ---- the tests ---------------------------------------------------------- */

/* THE regression. A backend that serves getblockhash settles statuses by the
 * node's own answer, which is the stronger check and rightly skips the
 * templates fallback. It must not also skip paying anyone.
 *
 * Before the fix this credited nothing: gbh_state latches to 1 on the first
 * successful call and every later pass returned before reaching the
 * distributor. */
static void test_distribution_runs_on_the_getblockhash_path(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_node");
    seed_shallow_candidate(s, "blk_shallow", 800240);

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_agrees,
        .get_block_hash_ctx = (void *)"blk_shallow",
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 0,
    };
    reconcile_result_t r = {0};
    reconcile_blocks_pass(&cfg, 800250, &r);

    CHECK(atomic_load(&gbh) == 1);          /* the node answered */
    CHECK(r.ran_templates_pass == 0);       /* so the fallback stayed out */
    CHECK(r.ran_distribution == 1);         /* and the miner still got paid */
    CHECK(r.blocks_distributed == 1);
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits") == 100000);
}

/* The same, down the other branch. This one always worked -- it is the path
 * every enforcer takes, and the only one the regtest e2e can reach. */
static void test_distribution_runs_on_the_templates_path(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_tmpl");
    seed_shallow_candidate(s, "blk_shallow", 800240);

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_unsupported,
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 0,
    };
    reconcile_result_t r = {0};
    reconcile_blocks_pass(&cfg, 800250, &r);

    CHECK(atomic_load(&gbh) == -1);         /* latched off, permanently */
    CHECK(r.ran_templates_pass == 1);
    CHECK(r.ran_distribution == 1);
    CHECK(r.blocks_distributed == 1);
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits") == 100000);
}

/* gbh_state latches, so the branch under test is the SECOND pass and every one
 * after it -- which is what made the bug survive: the first pass, before
 * anything had answered, behaved correctly. */
static void test_distribution_survives_the_latch(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_latch");
    /* Height chosen so it is still under BLOCK_FINAL_DEPTH on BOTH passes,
     * which is what keeps gbh_state latched at 1 for the second one. */
    seed_shallow_candidate(s, "blk_shallow", 800240);

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_agrees,
        .get_block_hash_ctx = (void *)"blk_shallow",
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 0,
    };
    reconcile_result_t first = {0}, second = {0};
    reconcile_blocks_pass(&cfg, 800250, &first);
    reconcile_blocks_pass(&cfg, 800251, &second);

    CHECK(first.ran_distribution == 1);
    CHECK(second.ran_distribution == 1);    /* the pass that used to return early */
    /* Exactly once, though: crediting is additive, so a second distribution
     * would double the balance and leave no trace in the amounts. */
    CHECK(second.blocks_distributed == 0);
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits") == 100000);
}

/* A transient RPC failure leaves statuses alone -- recording a verdict we did
 * not get would be worse than waiting -- but must not stop paying out rows an
 * earlier pass already settled. A backend that kept failing this one call
 * would otherwise silently stop crediting anyone. */
static void test_a_transient_lookup_failure_still_pays(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_flaky");
    /* One shallow candidate, so the pass has something to look up. */
    assert(store_record_block(s, 1200, 800240, "blk_shallow", "alice", "addr_a",
                              100000, 0, STORE_BLOCK_PENDING, NULL, 0.0) == 0);
    assert(store_flush(s) == 0);

    gbh_transient_calls = 0;
    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_transient,
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 0,
    };
    reconcile_result_t r = {0};
    reconcile_blocks_pass(&cfg, 800250, &r);

    CHECK(gbh_transient_calls > 0);
    CHECK(atomic_load(&gbh) == 0);          /* nothing was learned either way */
    CHECK(r.ran_templates_pass == 0);       /* statuses untouched, so no fallback */
    CHECK(r.ran_distribution == 1);         /* but the ledger still moves */
    CHECK(r.blocks_distributed == 1);
    store_close(s);
}

/* Every other mode has already paid the miner in the coinbase or priced the
 * share when it arrived. Distributing there would credit a second time. */
static void test_non_pplns_modes_never_distribute(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_solo");

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_unsupported,
        .gbh_state = &gbh, .pplns = 0, .fee_bps = 0,
    };
    reconcile_result_t r = {0};
    reconcile_blocks_pass(&cfg, 800250, &r);

    CHECK(r.ran_distribution == 0);
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COUNT(*) FROM pps_credits") == 0);
}

/* A candidate the node says is not in the chain is orphaned, and orphaned
 * blocks are not distributed however deep they are. */
static void test_a_reorged_candidate_is_orphaned_not_paid(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    /* Shallow enough to still be re-checked (< BLOCK_FINAL_DEPTH). */
    for (int i = 0; i < 10; ++i) {
        assert(store_record_share_addr(s, "alice", "addr_a",
                                       1000ULL + (uint64_t)i, 5.0,
                                       0, NULL, 0, 0.0) == 0);
    }
    assert(store_record_share_addr(s, "alice", "addr_a", 1100, 0.0,
                                   1, "blk_gone", 0, 0.0) == 0);
    assert(store_record_block(s, 1100, 800200, "blk_gone", "alice", "addr_a",
                              100000, 0, STORE_BLOCK_PENDING, NULL, 50.0) == 0);
    assert(store_flush(s) == 0);

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_disagrees,
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 0,
    };
    reconcile_result_t r = {0};
    reconcile_blocks_pass(&cfg, 800250, &r);

    CHECK(r.checked_via_node == 1);
    CHECK(r.ran_distribution == 1);         /* it runs... */
    CHECK(r.blocks_distributed == 0);       /* ...and finds nothing eligible */
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COUNT(*) FROM blocks_found WHERE status='orphaned'") == 1);
    CHECK(scalar_i64(path, "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits") == 0);
}

/* The operator fee comes off the top, the same basis points every other mode
 * uses. */
static void test_the_operator_fee_is_taken(void) {
    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    seed_matured_block(s, "blk_fee");

    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = {
        .store = s, .get_block_hash = gbh_unsupported,
        .gbh_state = &gbh, .pplns = 1, .fee_bps = 100,   /* 1% */
    };
    reconcile_blocks_pass(&cfg, 800250, NULL);
    store_close(s);
    CHECK(scalar_i64(path, "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits") == 99000);
}

/* Defensive: the pass is called from a tip watcher that may run before the
 * store is open, and a non-positive height is not a chain position. */
static void test_a_pass_with_nothing_to_do_does_nothing(void) {
    _Atomic int gbh = 0;
    reconcile_cfg_t cfg = { .store = NULL, .gbh_state = &gbh, .pplns = 1 };
    reconcile_result_t r = { .ran_distribution = 1 };
    reconcile_blocks_pass(&cfg, 800250, &r);
    CHECK(r.ran_distribution == 0);
    reconcile_blocks_pass(NULL, 800250, &r);
    CHECK(r.ran_distribution == 0);

    const char *path = fresh_db_path();
    store_t *s = open_store(path);
    cfg.store = s;
    reconcile_blocks_pass(&cfg, 0, &r);
    CHECK(r.ran_distribution == 0);
    reconcile_blocks_pass(&cfg, -1, &r);
    CHECK(r.ran_distribution == 0);
    store_close(s);
}

/* The runner reports the verdict, not the test. Letting each test print its
 * own "ok" at the end meant a test whose CHECKs had failed still announced
 * success -- the count at the bottom disagreed, but the line a reader scans
 * said ok. */
static void run(const char *name, void (*fn)(void)) {
    const int before = failures;
    fn();
    printf("  %s %s\n", failures == before ? "ok" : "FAILED", name);
}

int main(void) {
    log_init(LOG_LVL_ERROR);
    printf("running test_reconcile...\n");
    run("test_distribution_runs_on_the_getblockhash_path", test_distribution_runs_on_the_getblockhash_path);
    run("test_distribution_runs_on_the_templates_path", test_distribution_runs_on_the_templates_path);
    run("test_distribution_survives_the_latch", test_distribution_survives_the_latch);
    run("test_a_transient_lookup_failure_still_pays", test_a_transient_lookup_failure_still_pays);
    run("test_non_pplns_modes_never_distribute", test_non_pplns_modes_never_distribute);
    run("test_a_reorged_candidate_is_orphaned_not_paid", test_a_reorged_candidate_is_orphaned_not_paid);
    run("test_the_operator_fee_is_taken", test_the_operator_fee_is_taken);
    run("test_a_pass_with_nothing_to_do_does_nothing", test_a_pass_with_nothing_to_do_does_nothing);
    cleanup_dbs();
    printf("test_reconcile: %d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
