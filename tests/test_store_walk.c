/* White-box test for the PPLNS boundary walk in store_pplns_window().
 *
 * What is being pinned: when the walk cannot PROVE it covered the window, the
 * function must return an error, never a window. The caller renders whatever
 * it returns into a coinbase and publishes it; the payment IS the block, so a
 * wrong window is mined, irreversible, and invisible afterwards.
 *
 * HOW the failure is reproduced. The interesting failures are a sqlite step
 * that fails PART WAY THROUGH the widening loop -- BUSY from the share writer
 * committing under WAL, INTERRUPT, NOMEM. Two things rule out the obvious
 * approaches: sqlite3_progress_handler fires for the store's background commit
 * thread on the same connection, so it cannot single out one statement; and an
 * authorizer runs at prepare time, before the loop. So this file includes
 * store.c as source with sqlite3_step redirected to a wrapper that fails the
 * boundary query on demand, and nothing else. That keeps the fault injection
 * inside the test binary -- production code carries no test seam. */
#include <sqlite3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tsw_step(sqlite3_stmt *st);
static void maybe_delete_oldest(sqlite3_stmt *st);

/* Redirect every sqlite3_step() inside store.c to the wrapper below. */
#define sqlite3_step tsw_step
#include "../src/store.c"
#undef sqlite3_step

/* -1 disables injection. Otherwise: let the boundary query succeed this many
 * times, then fail it. 0 fails its very first step. */
static int g_fail_qb_after = -1;
static int g_qb_steps = 0;

static int tsw_step(sqlite3_stmt *st) {
    const char *sql = sqlite3_sql(st);
    /* The boundary query, and only it. store_pplns_distribute()'s Q_WINDOW also
     * carries ROWS UNBOUNDED PRECEDING and runs on this same connection through
     * this same wrapper, so the window frame alone does not identify the
     * statement. The `MIN(id), MAX(running)` projection does, and the LIMIT
     * narrows it further; Q_WINDOW has neither. */
    int is_qb = sql && strstr(sql, "ROWS UNBOUNDED PRECEDING") != NULL
                    && strstr(sql, "LIMIT ?") != NULL
                    && strstr(sql, "MIN(id), MAX(running)") != NULL;
    if (is_qb && g_fail_qb_after >= 0 && g_qb_steps++ >= g_fail_qb_after)
        return SQLITE_INTERRUPT;
    maybe_delete_oldest(st);
    return sqlite3_step(st);
}

static char g_path[512];
static const char *fresh_path(void) {
    static int seq = 0;
    snprintf(g_path, sizeof g_path, "/tmp/sp_walk_%d_%d.db", (int)getpid(), ++seq);
    unlink(g_path);
    char side[520];
    snprintf(side, sizeof side, "%s-wal", g_path); unlink(side);
    snprintf(side, sizeof side, "%s-shm", g_path); unlink(side);
    return g_path;
}

static store_t *open_with_shares(const char *path, int nshares, double diff) {
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof cfg.path, "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 20000;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);
    for (int i = 0; i < nshares; ++i) {
        char name[32], addr[48];
        snprintf(name, sizeof name, "w%d", i % 8);
        snprintf(addr, sizeof addr, "addr_%d", i % 8);
        assert(store_record_share_addr(s, name, addr, 1000ULL + (uint64_t)i,
                                       diff, 0, NULL, 0, 0.0) == 0);
    }
    assert(store_flush(s) == 0);
    return s;
}

/* The precondition. Without it the injection tests below could pass against a
 * store_pplns_window() that never worked at all -- "correctly refused" and
 * "never returned anything" look identical from outside. */
static void test_the_walk_serves_the_configured_window_when_nothing_fails(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 200, 10.0);
    g_fail_qb_after = -1;

    store_window_entry_t win[16];
    size_t n = 0; double total = 0.0; int trunc = 0;
    char err[256] = {0};
    int rc = store_pplns_window(s, 500.0, win, 16, &n, &total, &trunc, err, sizeof err);
    assert(rc > 0);
    assert(n > 0);
    /* 500 configured, one 10-difficulty share of slack for the share that
     * crosses the boundary. NOT the table's 2000. */
    assert(total >= 500.0 && total <= 510.0);
    store_close(s);
    unlink(path);
    printf("  ok test_the_walk_serves_the_configured_window_when_nothing_fails\n");
}

/* Path 3: the boundary query fails on its FIRST step, so cutoff_id is never
 * assigned. Before the fix it stayed 0, the payout query became `sh.id >= 0`,
 * and the whole table was served as the window -- unbounded, and four times
 * what was configured here. */
static void test_a_walk_that_fails_immediately_does_not_serve_the_whole_table(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 200, 10.0);

    store_window_entry_t win[16];
    size_t n = 12345; double total = -1.0; int trunc = 0;
    char err[256] = {0};

    g_fail_qb_after = 0; g_qb_steps = 0;
    int rc = store_pplns_window(s, 500.0, win, 16, &n, &total, &trunc, err, sizeof err);
    g_fail_qb_after = -1;

    if (rc >= 0) {
        fprintf(stderr, "  FAIL: served rc=%d n=%zu total=%.0f "
                        "(configured window 500, whole table 2000)\n", rc, n, total);
        assert(rc < 0 && "a walk that never read a row must not serve a window");
    }
    assert(n == 0);
    /* The reason must name the failure, not paper over it. An earlier version
     * appended sqlite3_errmsg() unconditionally, so an exit that was not a
     * step failure reported the literal string "not an error". */
    assert(err[0] != '\0');
    assert(strstr(err, "did not cover") != NULL);
    assert(strstr(err, "not an error") == NULL);
    printf("      reason: %s\n", err);
    store_close(s);
    unlink(path);
    printf("  ok test_a_walk_that_fails_immediately_does_not_serve_the_whole_table\n");
}

/* Path 1: the walk widens, then the second step fails. cutoff_id then holds
 * the FIRST iteration's boundary -- known to be short of the window, because
 * falling short is the only reason a second iteration happens at all. */
static void test_a_walk_that_fails_after_widening_does_not_serve_a_short_window(void) {
    const char *path = fresh_path();
    /* 6000 shares x 10 = 60,000. A 50,000 window needs 5,000 rows, past the
     * 4,096 first batch, so the walk must widen exactly once. */
    store_t *s = open_with_shares(path, 6000, 10.0);

    /* Precondition: prove the walk really does widen here, by checking the
     * un-injected answer needs more than the first batch. */
    g_fail_qb_after = -1;
    store_window_entry_t ref[16];
    size_t rn = 0; double rtotal = 0.0; int rt = 0; char rerr[256] = {0};
    assert(store_pplns_window(s, 50000.0, ref, 16, &rn, &rtotal, &rt, rerr, sizeof rerr) > 0);
    assert(rtotal >= 50000.0 && rtotal <= 50010.0);

    store_window_entry_t win[16];
    size_t n = 12345; double total = -1.0; int trunc = 0; char err[256] = {0};
    g_fail_qb_after = 1; g_qb_steps = 0;      /* first batch ok, widened step fails */
    int rc = store_pplns_window(s, 50000.0, win, 16, &n, &total, &trunc, err, sizeof err);
    g_fail_qb_after = -1;

    if (rc >= 0) {
        fprintf(stderr, "  FAIL: served rc=%d n=%zu total=%.0f after a truncated "
                        "widening (configured 50000)\n", rc, n, total);
        assert(rc < 0 && "a walk cut short mid-widening must not serve a window");
    }
    assert(n == 0);
    store_close(s);
    unlink(path);
    printf("  ok test_a_walk_that_fails_after_widening_does_not_serve_a_short_window\n");
}

/* Rows deleted while the walk runs must not stop it terminating.
 *
 * An earlier version of this fix answered "have I read the whole table?" with
 * a MIN(id) taken ONCE before the loop. Deleting the oldest row lifts the real
 * table minimum above that stale value, so the comparison can never come true;
 * with a window wider than what is left, neither can the covered test, and the
 * loop ran 27 times -- each iteration an unbounded scan and sort of the whole
 * table, which is the exact pathology this batching exists to remove -- until
 * `batch` overflowed. Answering from the batch's own row count instead is what
 * makes this terminate, because it describes the rows actually read. */
static int g_delete_on_qb_step = 0;
static sqlite3 *g_delete_db = NULL;

static void maybe_delete_oldest(sqlite3_stmt *st) {
    const char *sql = sqlite3_sql(st);
    if (!g_delete_on_qb_step || !g_delete_db) return;
    if (!sql || strstr(sql, "MIN(id), MAX(running)") == NULL) return;
    g_delete_on_qb_step = 0;
    sqlite3_exec(g_delete_db,
                 "DELETE FROM shares WHERE id = (SELECT MIN(id) FROM shares)",
                 NULL, NULL, NULL);
}

static void test_a_row_deleted_during_the_walk_still_terminates(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 50, 10.0);   /* 500 total difficulty */

    store_window_entry_t win[16];
    size_t n = 0; double total = 0.0; int trunc = 0; char err[256] = {0};

    g_delete_db = s->db;
    g_delete_on_qb_step = 1;
    /* A window far wider than the history, so "covered" can never end the
     * walk and only the end-of-table test can. */
    int rc = store_pplns_window(s, 1e9, win, 16, &n, &total, &trunc, err, sizeof err);
    g_delete_on_qb_step = 0; g_delete_db = NULL;

    /* It must come back -- with the remaining history, or with an error. What
     * it must not do is spin. */
    assert(rc >= 0);
    assert(n > 0);
    assert(total > 0.0 && total < 1e9);
    store_close(s);
    unlink(path);
    printf("  ok test_a_row_deleted_during_the_walk_still_terminates"
           " (rc=%d n=%zu total=%.0f)\n", rc, n, total);
}

/* A pool younger than its own window is NOT an error -- it is the documented
 * case and must still pay across everything it has. Conflating it with a
 * truncated read would take a new pool offline on day one. */

static void test_a_pool_younger_than_its_window_still_returns_a_window(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 20, 10.0);
    g_fail_qb_after = -1;

    store_window_entry_t win[16];
    size_t n = 0; double total = 0.0; int trunc = 0; char err[256] = {0};
    int rc = store_pplns_window(s, 1e9, win, 16, &n, &total, &trunc, err, sizeof err);
    assert(rc > 0);
    assert(n > 0);
    assert(total > 0.0 && total < 1e9);
    store_close(s);
    unlink(path);
    printf("  ok test_a_pool_younger_than_its_window_still_returns_a_window\n");
}

static void test_an_empty_table_is_not_an_error(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 0, 10.0);
    g_fail_qb_after = -1;

    store_window_entry_t win[4];
    size_t n = 999; double total = -1.0; int trunc = 0; char err[256] = {0};
    int rc = store_pplns_window(s, 500.0, win, 4, &n, &total, &trunc, err, sizeof err);
    assert(rc == 0);
    assert(n == 0);
    assert(total == 0.0);
    store_close(s);
    unlink(path);
    printf("  ok test_an_empty_table_is_not_an_error\n");
}

/* The seam must fail the boundary statement and NOTHING else.
 *
 * An earlier attempt at this suite injected through a global interrupt budget,
 * which killed the payout query too -- so its tests went red for a reason that
 * had nothing to do with the walk, and would have gone red against the fixed
 * code as well. Every other test here returns before the payout query is
 * reached, so none of them can catch a matcher that is too wide. This one arms
 * the injection with a budget the walk never spends: if the matcher caught the
 * payout query, its steps would spend that budget and be interrupted. */
static void test_the_injection_seam_does_not_reach_the_payout_query(void) {
    const char *path = fresh_path();
    store_t *s = open_with_shares(path, 200, 10.0);
    /* Armed, but the walk covers 500 in one step and the budget is five. */
    g_fail_qb_after = 5;
    g_qb_steps = 0;

    store_window_entry_t win[16];
    size_t n = 0; double total = 0.0; int trunc = 0; char err[256] = {0};
    int rc = store_pplns_window(s, 500.0, win, 16, &n, &total, &trunc, err, sizeof err);
    assert(rc > 0);
    assert(n > 0);
    assert(total >= 500.0 && total <= 510.0);
    /* One statement was ever matched, and it stepped once: the boundary query.
     * The payout query ran through the same wrapper and was left alone. */
    assert(g_qb_steps == 1);
    store_close(s);
    unlink(path);
    printf("  ok test_the_injection_seam_does_not_reach_the_payout_query"
           " (qb_steps=%d)\n", g_qb_steps);
}

int main(void) {
    test_the_walk_serves_the_configured_window_when_nothing_fails();
    test_a_walk_that_fails_immediately_does_not_serve_the_whole_table();
    test_a_walk_that_fails_after_widening_does_not_serve_a_short_window();
    test_a_row_deleted_during_the_walk_still_terminates();
    test_a_pool_younger_than_its_window_still_returns_a_window();
    test_an_empty_table_is_not_an_error();
    test_the_injection_seam_does_not_reach_the_payout_query();
    printf("test_store_walk: all tests passed\n");
    return 0;
}
