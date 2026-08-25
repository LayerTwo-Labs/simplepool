/* Standalone test for src/store.c. Builds with -lsqlite3 -lpthread. */

#include "store.h"
#include "log.h"

#include <sqlite3.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_db_paths[16][256];
static int  g_db_count = 0;

static const char *fresh_db_path(void) {
    char *p = g_db_paths[g_db_count++];
    snprintf(p, 256, "/tmp/store_test_%d_%d.db", (int)getpid(), g_db_count);
    unlink(p);
    /* WAL/SHM siblings */
    char wal[300], shm[300];
    snprintf(wal, sizeof(wal), "%s-wal", p);
    snprintf(shm, sizeof(shm), "%s-shm", p);
    unlink(wal);
    unlink(shm);
    return p;
}

static void cleanup_dbs(void) {
    for (int i = 0; i < g_db_count; ++i) {
        unlink(g_db_paths[i]);
        char wal[300], shm[300];
        snprintf(wal, sizeof(wal), "%.255s-wal", g_db_paths[i]);
        snprintf(shm, sizeof(shm), "%.255s-shm", g_db_paths[i]);
        unlink(wal);
        unlink(shm);
    }
}

static int64_t scalar_i64(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* Copies into `out` because the sqlite3_stmt is finalized before returning.
 * Writes "" for SQL NULL, and returns whether the column was non-NULL — the
 * identity test needs to tell "stored blank" from "stored nothing". */
static int scalar_text(sqlite3 *db, const char *sql, char *out, size_t cap) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    int is_null = sqlite3_column_type(st, 0) == SQLITE_NULL;
    const unsigned char *v = sqlite3_column_text(st, 0);
    snprintf(out, cap, "%s", (is_null || !v) ? "" : (const char *)v);
    sqlite3_finalize(st);
    return !is_null;
}

static double scalar_dbl(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    double v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* An existing database must survive the upgrade that adds blocks_found.status.
 *
 * Every other test here starts from an empty file, so store_open() only ever
 * runs against a database it just created -- where CREATE TABLE already names
 * every current column. A deployed pool is the opposite case: the table exists
 * from an older schema and the new columns arrive only through MIGRATIONS_SQL.
 * Nothing exercised that path, so a statement in the strict schema section that
 * depends on a migrated column passes the whole suite and still fails to open a
 * real database.
 *
 * The old CREATE TABLE is written out literally rather than derived from the
 * current source, so this keeps testing the upgrade when the schema changes
 * again. */
static void test_open_upgrades_a_pre_status_database(void) {
    const char *path = fresh_db_path();

    sqlite3 *raw = NULL;
    assert(sqlite3_open(path, &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw,
        "CREATE TABLE blocks_found ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts              INTEGER NOT NULL,"
        "  height          INTEGER NOT NULL,"
        "  hash            TEXT NOT NULL,"
        "  finder_id       INTEGER,"
        "  finder_address  TEXT,"
        "  reward_sats     INTEGER,"
        "  fee_sats        INTEGER"
        ");"
        "INSERT INTO blocks_found (ts,height,hash,reward_sats,fee_sats)"
        "  VALUES (1000, 900001, 'deadbeef', 312000000, 780000);",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);

    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* ...and the upgrade must have happened, not merely not-crashed. */
    sqlite3 *chk = NULL;
    assert(sqlite3_open(path, &chk) == SQLITE_OK);
    int has_status = 0, has_index = 0, rows = 0;
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(chk, "PRAGMA table_info(blocks_found)", -1, &st, NULL) == SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 1);
        if (n && strcmp(n, "status") == 0) has_status = 1;
    }
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(chk, "PRAGMA index_list(blocks_found)", -1, &st, NULL) == SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 1);
        if (n && strcmp(n, "blocks_found_status_idx") == 0) has_index = 1;
    }
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(chk, "SELECT COUNT(*) FROM blocks_found", -1, &st, NULL) == SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW) rows = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(chk);

    assert(has_status && "migration must add blocks_found.status");
    assert(has_index  && "migration must create blocks_found_status_idx");
    assert(rows == 1  && "the pre-existing block row must survive");

    store_close(s);
    printf("  ok test_open_upgrades_a_pre_status_database\n");
}

static void test_basic(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);
    assert(s != NULL);

    double expected_sum = 0.0;
    for (int i = 0; i < 1000; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "worker%d", i % 10);
        double diff = 1.0 + (double)(i % 7);
        expected_sum += diff;
        rc = store_record_share(s, wname, 1000ULL + (uint64_t)i, diff, 0, NULL);
        assert(rc == 0);
    }

    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);

    int64_t nworkers = scalar_i64(db, "SELECT count(*) FROM workers");
    int64_t nshares  = scalar_i64(db, "SELECT count(*) FROM shares");
    double  sumd     = scalar_dbl(db, "SELECT sum(difficulty) FROM shares");
    assert(nworkers == 10);
    assert(nshares == 1000);
    assert(sumd > expected_sum - 0.001 && sumd < expected_sum + 0.001);

    /* Block path */
    rc = store_record_block(s, 9999, 12345, "abc123hash", "worker3",
                            "bcrt1qexampleaddr", 4950000000LL, 50000000LL,
                            STORE_BLOCK_PENDING, NULL);
    assert(rc == 0);
    rc = store_flush(s);
    assert(rc == 0);

    int64_t nblocks = scalar_i64(db, "SELECT count(*) FROM blocks_found");
    assert(nblocks == 1);

    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT b.finder_id, w.id, b.finder_address, b.reward_sats, b.fee_sats "
        "FROM blocks_found b JOIN workers w ON w.name='worker3' LIMIT 1",
        -1, &st, NULL);
    assert(rc == SQLITE_OK);
    rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW);
    int64_t finder = sqlite3_column_int64(st, 0);
    int64_t wid    = sqlite3_column_int64(st, 1);
    const unsigned char *addr_txt = sqlite3_column_text(st, 2);
    int64_t reward = sqlite3_column_int64(st, 3);
    int64_t fee    = sqlite3_column_int64(st, 4);
    assert(finder == wid);
    assert(addr_txt && strcmp((const char *)addr_txt, "bcrt1qexampleaddr") == 0);
    assert(reward == 4950000000LL);
    assert(fee == 50000000LL);
    sqlite3_finalize(st);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_basic\n");
}

static void test_rejects(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 100;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    for (int i = 0; i < 50; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "rw%d", i);
        rc = store_record_reject(s, wname, 1000 + (uint64_t)i, "low-difficulty");
        assert(rc == 0);
    }
    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);
    int64_t n = scalar_i64(db, "SELECT count(*) FROM rejects");
    assert(n == 50);
    sqlite3_close(db);
    store_close(s);
    printf("  ok test_rejects\n");
}

typedef struct {
    store_t *s;
    int      tid;
    int      n;
} thread_arg_t;

static void *thread_fn(void *arg) {
    thread_arg_t *t = (thread_arg_t *)arg;
    for (int i = 0; i < t->n; ++i) {
        char wname[32];
        snprintf(wname, sizeof(wname), "tw%d", t->tid);
        int rc = store_record_share(t->s, wname,
            10000ULL + (uint64_t)i, 2.5, 0, NULL);
        if (rc != 0) {
            /* retry briefly if queue saturated */
            nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000}, NULL);
            --i;
        }
    }
    return NULL;
}

static void test_concurrent(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 10;
    cfg.commit_max_shares = 500;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    pthread_t th[8];
    thread_arg_t args[8];
    for (int i = 0; i < 8; ++i) {
        args[i].s = s; args[i].tid = i; args[i].n = 1000;
        pthread_create(&th[i], NULL, thread_fn, &args[i]);
    }
    for (int i = 0; i < 8; ++i) pthread_join(th[i], NULL);

    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);
    int64_t n = scalar_i64(db, "SELECT count(*) FROM shares");
    assert(n == 8000);
    int64_t nw = scalar_i64(db, "SELECT count(*) FROM workers");
    assert(nw == 8);
    sqlite3_close(db);
    store_close(s);
    printf("  ok test_concurrent (8000 shares across 8 threads)\n");
}

static void test_drop(void) {
    /* Tiny ring; throw way more than can fit. */
    store_test_set_ring_capacity(64);
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 1000;   /* writer rarely wakes */
    cfg.commit_max_shares = 8;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    int dropped_observed = 0;
    for (int i = 0; i < 200000; ++i) {
        rc = store_record_share(s, "ww", 1000, 1.0, 0, NULL);
        if (rc < 0) dropped_observed = 1;
    }
    store_stats_t st;
    store_get_stats(s, &st);
    assert(dropped_observed);
    assert(st.shares_dropped > 0);

    /* Don't bother flushing fully -- just close (which drains). */
    store_close(s);
    store_test_set_ring_capacity(0);
    printf("  ok test_drop (dropped=%llu)\n",
           (unsigned long long)st.shares_dropped);
}

/* credited_sats must be stored per share exactly as passed, and pool_meta
 * must be readable back.
 *
 * The audit sums shares.credited_sats instead of recomputing it from a rate,
 * because the rate is derived per template and moves with difficulty. That
 * only works if the column faithfully records what was credited — including
 * 0, which is what solo mode writes since it never accrues. */
static void test_credited_sats(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* pps-classic-style: each share carries the sats it was credited and
     * the rate that produced them. difficulty=i at rate 7.0 gives i*7. */
    int64_t expected = 0;
    for (int i = 1; i <= 50; ++i) {
        int64_t credited = (int64_t)i * 7;
        expected += credited;
        assert(store_record_share_addr(s, "payer", "addr1",
                                       1000ULL + (uint64_t)i, (double)i,
                                       0, NULL, credited, 7.0) == 0);
    }
    /* solo-style: no accrual, so the column must record 0 — not be left
     * to a later recompute that would invent a credit. rate_used stays 0
     * too, which is what marks the row as "nothing to verify". */
    for (int i = 0; i < 25; ++i) {
        assert(store_record_share_addr(s, "solo", "addr2",
                                       9000ULL + (uint64_t)i, 3.0,
                                       0, NULL, 0, 0.0) == 0);
    }
    /* The legacy 6-arg helper must still work and store 0. */
    assert(store_record_share(s, "legacy", 9500, 2.0, 0, NULL) == 0);

    assert(store_record_pool_meta(s, "pps-classic", 100, "derived",
                                  2783.22, 2811.33, 100.4,
                                  111157.455, 312500000, 1700000000ULL) == 0);
    assert(store_flush(s) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);

    int64_t total = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='payer')");
    assert(total == expected);

    int64_t solo = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='solo')");
    assert(solo == 0);

    int64_t legacy = scalar_i64(db,
        "SELECT sum(credited_sats) FROM shares "
        "WHERE worker_id = (SELECT id FROM workers WHERE name='legacy')");
    assert(legacy == 0);

    /* The audit invariant. Every credited share must re-derive exactly from
     * the pair stored on its own row, with no reference to any current rate.
     * This is the property that makes the ledger checkable rather than
     * merely recorded, so it is asserted as equality, not as a tolerance. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares WHERE rate_used > 0 "
        "  AND credited_sats <> CAST(difficulty * rate_used AS INTEGER)") == 0);
    /* Non-accruing rows carry no multiplicand to check against. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares WHERE rate_used != 0 "
        "  AND worker_id IN (SELECT id FROM workers "
        "                     WHERE name IN ('solo','legacy'))") == 0);

    /* pool_meta: single row, values round-tripped. */
    assert(scalar_i64(db, "SELECT count(*) FROM pool_meta") == 1);
    assert(scalar_i64(db, "SELECT fee_bps FROM pool_meta") == 100);
    double rate = scalar_dbl(db, "SELECT rate_sats_per_diff FROM pool_meta");
    assert(rate > 2783.0 && rate < 2783.5);

    /* credited_from is stamped once and must survive later updates, so an
     * audit can tell where credited_sats became trustworthy. */
    int64_t from1 = scalar_i64(db, "SELECT credited_from FROM pool_meta");
    assert(from1 == 1700000000);
    assert(store_record_pool_meta(s, "pps-classic", 200, "override",
                                  1000.0, 2811.33, 6443.0,
                                  111157.455, 312500000, 1700009999ULL) == 0);
    int64_t from2 = scalar_i64(db, "SELECT credited_from FROM pool_meta");
    assert(from2 == 1700000000);
    assert(scalar_i64(db, "SELECT fee_bps FROM pool_meta") == 200);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_credited_sats\n");
}

/* Pool identity: written once at startup, and the only source the dashboard
 * has for what this pool is. Two properties matter beyond the round trip.
 *
 * First, it must not collide with the per-template pool_meta write — they
 * share the id=1 row and run in either order, so each must leave the other's
 * columns alone, including the write-once credited_from stamp.
 *
 * Second, solo mode must store pool_btc_address as NULL rather than "", so a
 * reader can tell "no pool wallet in this mode" from "operator configured a
 * blank one". */
static void test_pool_identity(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* Identity first, template second — the startup order. */
    assert(store_record_pool_identity(s, "signet", "node", "/simplepool/",
                                      "tb1qoperator", "tb1qpoolwallet",
                                      "[{\"port\":3334,\"label\":\"\","
                                      "\"min_diff\":1,\"initial_diff\":1}]") == 0);
    assert(store_record_pool_meta(s, "pps-classic", 100, "derived",
                                  2783.22, 2811.33, 100.4,
                                  111157.455, 312500000, 1700000000ULL) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);

    char buf[128];
    assert(scalar_i64(db, "SELECT count(*) FROM pool_meta") == 1);
    scalar_text(db, "SELECT network FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "signet") == 0);
    scalar_text(db, "SELECT network_source FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "node") == 0);
    scalar_text(db, "SELECT coinbase_tag FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "/simplepool/") == 0);
    scalar_text(db, "SELECT operator_address FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "tb1qoperator") == 0);
    scalar_text(db, "SELECT pool_btc_address FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "tb1qpoolwallet") == 0);

    /* The template write must not have disturbed identity, and identity must
     * not have pre-empted the write-once credited_from stamp. */
    assert(scalar_i64(db, "SELECT fee_bps FROM pool_meta") == 100);
    assert(scalar_i64(db, "SELECT credited_from FROM pool_meta") == 1700000000);

    /* updated_at means "when the rate was last refreshed". Identity is
     * written once at startup, so re-writing it must not touch that — else a
     * stalled template path would keep looking alive. */
    int64_t seen = scalar_i64(db, "SELECT updated_at FROM pool_meta");
    assert(store_record_pool_identity(s, "regtest", "inferred", "/other/",
                                      "bcrt1qop", NULL, NULL) == 0);
    assert(scalar_i64(db, "SELECT updated_at FROM pool_meta") == seen);

    /* Solo mode: NULL, not "". */
    assert(scalar_text(db, "SELECT pool_btc_address FROM pool_meta",
                       buf, sizeof buf) == 0);
    scalar_text(db, "SELECT network FROM pool_meta", buf, sizeof buf);
    assert(strcmp(buf, "regtest") == 0);
    /* And still no collateral damage to the rate half. */
    assert(scalar_i64(db, "SELECT credited_from FROM pool_meta") == 1700000000);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_pool_identity\n");
}

/* rate_history is the provenance half of the audit: it must append when the
 * rate moves, stay quiet when it doesn't, and hold rows that re-derive from
 * their own inputs. */
static void test_rate_history(void) {
    const char *path = fresh_db_path();

    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    const double  net_diff = 111157.455354832;
    const int64_t value    = 312500000;
    const int     fee_bps  = 100;
    double gross = (double)value / net_diff;
    double rate  = gross * (1.0 - (double)fee_bps / 10000.0);

    assert(store_record_rate(s, "derived", rate, gross, fee_bps,
                             net_diff, value, 1700000000ULL) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 1);

    /* Re-publishing an unchanged rate must not append — otherwise the table
     * grows once per template poll rather than once per actual change. */
    for (int i = 0; i < 10; ++i) {
        assert(store_record_rate(s, "derived", rate, gross, fee_bps,
                                 net_diff, value, 1700000100ULL + (uint64_t)i) == 0);
    }
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 1);

    /* A moved block value is a new rate and must append. */
    int64_t value2 = 312500141;
    double  gross2 = (double)value2 / net_diff;
    double  rate2  = gross2 * (1.0 - (double)fee_bps / 10000.0);
    assert(store_record_rate(s, "derived", rate2, gross2, fee_bps,
                             net_diff, value2, 1700000200ULL) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 2);

    /* So is a changed source at an otherwise identical rate. */
    assert(store_record_rate(s, "override", rate2, gross2, fee_bps,
                             net_diff, value2, 1700000300ULL) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM rate_history") == 3);

    /* Every logged rate must follow from its own recorded inputs. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM rate_history WHERE ABS(rate_sats_per_diff"
        "  - (block_value_sats*1.0/network_difficulty)"
        "    *(1-fee_bps/10000.0)) > 1e-9") == 0);

    /* And a share credited at one of those rates must be traceable to it —
     * exact equality, because it is the same double on both sides. */
    assert(store_record_share_addr(s, "w", "addr", 1700000400000ULL, 2.0,
                                   0, NULL, (int64_t)(2.0 * rate2), rate2) == 0);
    assert(store_flush(s) == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM shares s WHERE s.rate_used > 0 AND NOT EXISTS ("
        "  SELECT 1 FROM rate_history r"
        "   WHERE r.rate_sats_per_diff = s.rate_used)") == 0);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_rate_history\n");
}

/* Template history: one row per materially distinct template. Repeat polls
 * fold into the row they match instead of appending — the block value moves
 * on nearly every poll, so keying on it grew the table by thousands of rows a
 * day, all of them fee churn at a height already recorded. */
static void test_template_history(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    store_template_t t;
    memset(&t, 0, sizeof t);
    t.ts_s = 1700000000; t.height = 977817;
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000ab";
    t.bits = "1a3839e6"; t.network_difficulty = 298383.4976083073;
    t.coinbase_value_sats = 312500500; t.tx_count = 1; t.tx_fees_sats = 500;
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    t.longpoll = 1; t.rate_sats_per_diff = 1036.8368;

    assert(store_record_template(s, &t) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);

    assert(scalar_i64(db, "SELECT polls FROM templates") == 1);
    assert(scalar_i64(db, "SELECT last_seen FROM templates") == 1700000000);

    /* Re-publishing the same work must not append, even as time moves on. */
    for (int i = 0; i < 10; ++i) {
        t.ts_s += 30;
        assert(store_record_template(s, &t) == 0);
    }
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);

    /* Folded, not discarded: ts stays first-seen, so the row spans the window
     * the template was actually mined over. */
    assert(scalar_i64(db, "SELECT polls FROM templates")     == 11);
    assert(scalar_i64(db, "SELECT ts FROM templates")        == 1700000000);
    assert(scalar_i64(db, "SELECT last_seen FROM templates") == 1700000300);

    /* Fee churn at the same tip is the common case and must not append — but
     * the row has to carry the latest numbers, not the first ones. */
    t.coinbase_value_sats = 312501999; t.tx_count = 42; t.tx_fees_sats = 1999;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 1);
    assert(scalar_i64(db, "SELECT coinbase_value_sats FROM templates") == 312501999);
    assert(scalar_i64(db, "SELECT tx_count FROM templates")            == 42);
    assert(scalar_i64(db, "SELECT tx_fees_sats FROM templates")        == 1999);

    /* A new tip is new work. */
    t.height = 977818;
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000cd";
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 2);

    /* Switching template source decides whether blocks can carry sidechain
     * commitments at all, so it opens a row even mid-height — folding that
     * transition away would hide the exact regression this table is for. */
    t.source = "bitcoind"; t.cb_spendable = 0; t.cb_op_returns = 0;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 3);
    assert(scalar_i64(db,
        "SELECT cb_op_returns FROM templates ORDER BY id DESC LIMIT 1") == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM templates WHERE source='enforcer'") == 2);

    /* Losing a sidechain commitment while still on the enforcer is the same
     * class of regression, and is invisible from every other column. */
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    assert(store_record_template(s, &t) == 0);
    t.cb_op_returns = 1;
    assert(store_record_template(s, &t) == 0);
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 5);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_template_history\n");
}

/* A concurrent writer must not cost shares.
 *
 * This reproduces a real incident: a maintenance script took the write lock
 * for a moment, and because the connection had no busy_timeout the writer
 * thread got SQLITE_BUSY instantly. Its batch was already out of the ring, so
 * accepted shares — already acknowledged to the miner — were logged and
 * discarded. Holding the lock here for longer than one commit window forces
 * exactly that race. */
static void test_commit_survives_a_locked_db(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms  = 20;    /* wake often, so we hit the lock */
    cfg.commit_max_shares = 10;    /* several batches, not one */
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* A second connection grabs the write lock, as `sqlite3 < script.sql`
     * would. */
    sqlite3 *hog = NULL;
    assert(sqlite3_open(path, &hog) == SQLITE_OK);
    assert(sqlite3_exec(hog, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);

    const int N = 40;
    for (int i = 0; i < N; ++i) {
        char w[32];
        snprintf(w, sizeof(w), "miner%d", i % 4);
        assert(store_record_share(s, w, 1700000000000ULL + i, 1.0, 0, NULL) == 0);
    }

    /* Hold it well past several commit windows, then let go. */
    struct timespec hold = { .tv_sec = 0, .tv_nsec = 300L * 1000000L };
    nanosleep(&hold, NULL);
    assert(sqlite3_exec(hog, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(hog);

    assert(store_flush(s) == 0);

    store_stats_t st;
    store_get_stats(s, &st);
    assert(st.events_lost == 0);
    assert(st.shares_dropped == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    /* Every accepted share is on the ledger. Before the fix this came back
     * short, with the shortfall visible only as an ERROR line. */
    assert(scalar_i64(db, "SELECT count(*) FROM shares") == N);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_commit_survives_a_locked_db\n");
}

/* Retention keeps the table bounded. Pruning runs when a new row is opened
 * and is driven off the template's own clock, so this is deterministic. */
static void test_template_retention(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.templates_retention_days = 7;
    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    store_template_t t;
    memset(&t, 0, sizeof t);
    t.prev_hash = "00000000000000000000000000000000000000000000000000000000000000ab";
    t.bits = "1a3839e6"; t.network_difficulty = 298383.4976083073;
    t.coinbase_value_sats = 312500500; t.tx_count = 1; t.tx_fees_sats = 500;
    t.source = "enforcer"; t.cb_spendable = 1; t.cb_op_returns = 2;
    t.longpoll = 1; t.rate_sats_per_diff = 1036.8368;

    /* 30 distinct templates, one day apart. */
    for (int i = 0; i < 30; ++i) {
        t.ts_s  = 1700000000 + (int64_t)i * 86400;
        t.height = 977817 + i;
        assert(store_record_template(s, &t) == 0);
    }

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    /* The 7-day window holds the row just written plus the seven inside the
     * cutoff; everything older is gone. */
    assert(scalar_i64(db, "SELECT count(*) FROM templates") == 8);
    assert(scalar_i64(db, "SELECT MIN(height) FROM templates") == 977817 + 22);
    assert(scalar_i64(db, "SELECT MAX(height) FROM templates") == 977817 + 29);

    /* 0 means keep everything. */
    store_close(s);
    store_cfg_t keep;
    memset(&keep, 0, sizeof(keep));
    snprintf(keep.path, sizeof(keep.path), "%s", fresh_db_path());
    keep.templates_retention_days = 0;
    store_t *s2 = NULL;
    assert(store_open(&keep, &s2) == 0);
    for (int i = 0; i < 30; ++i) {
        t.ts_s  = 1700000000 + (int64_t)i * 86400;
        t.height = 977817 + i;
        assert(store_record_template(s2, &t) == 0);
    }
    sqlite3 *db2 = NULL;
    assert(sqlite3_open(keep.path, &db2) == SQLITE_OK);
    assert(scalar_i64(db2, "SELECT count(*) FROM templates") == 30);

    sqlite3_close(db2);
    sqlite3_close(db);
    store_close(s2);
    printf("  ok test_template_retention\n");
}

/* Block-candidate accounting. A share meeting network difficulty is only a
 * candidate: submitblock refuses stale, duplicate and high-hash ones
 * routinely, and on a low-difficulty chain that is nearly all of them. Every
 * row used to be written as a found block with its full reward, which is what
 * disabled the solvency check — it sums reward_sats across the table. */
static void test_block_candidate_status(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    int rc = store_open(&cfg, &s);
    assert(rc == 0);

    /* Accepted by the node: recorded, but only as pending. Nothing on the
     * submit path is allowed to claim a block is in the chain. */
    rc = store_record_block(s, 1000, 800001, "hash_accepted", "w1",
                            "bcrt1qaddr", 5000000000LL, 0,
                            STORE_BLOCK_PENDING, NULL);
    assert(rc == 0);

    /* Refused by the node: recorded so the refusal is visible, but as
     * 'rejected' — never counted, and carrying the node's reason. */
    rc = store_record_block(s, 1001, 800001, "hash_rejected", "w1",
                            "bcrt1qaddr", 5000000000LL, 0,
                            STORE_BLOCK_REJECTED, "inconclusive");
    assert(rc == 0);

    /* A coinbase height of zero cannot exist. Refused outright rather than
     * filed at a height no chain has. */
    rc = store_record_block(s, 1002, 0, "hash_zero_height", "w1",
                            "bcrt1qaddr", 5000000000LL, 0,
                            STORE_BLOCK_PENDING, NULL);
    assert(rc != 0);

    rc = store_flush(s);
    assert(rc == 0);

    sqlite3 *db = NULL;
    rc = sqlite3_open(path, &db);
    assert(rc == SQLITE_OK);

    assert(scalar_i64(db, "SELECT count(*) FROM blocks_found") == 2);
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE hash='hash_zero_height'") == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE status='pending'") == 1);
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE status='rejected'") == 1);
    /* Nothing may be born confirmed. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE status='confirmed'") == 0);

    char err[128] = {0};
    int had = scalar_text(db,
        "SELECT submit_error FROM blocks_found WHERE hash='hash_rejected'",
        err, sizeof err);
    assert(had && strcmp(err, "inconclusive") == 0);
    /* An accepted candidate has no error to carry. */
    had = scalar_text(db,
        "SELECT submit_error FROM blocks_found WHERE hash='hash_accepted'",
        err, sizeof err);
    assert(!had);

    /* This is the number the solvency check would sum. A rejected candidate
     * must contribute nothing to it. */
    assert(scalar_i64(db,
        "SELECT COALESCE(SUM(reward_sats),0) FROM blocks_found "
        "WHERE status='confirmed'") == 0);

    /* The stats counter follows the same rule: refused candidates are not
     * blocks, so the shutdown line does not report them as such. */
    store_stats_t st = {0};
    store_get_stats(s, &st);
    assert(st.blocks_committed == 1);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_block_candidate_status\n");
}

/* Reconciliation against the observed chain of tips, which is the only path
 * available when the backend serves nothing but getblocktemplate and
 * submitblock — the CUSF enforcer a drivechain pool must point at.
 *
 * A template building height H+1 with prev_hash X is the node saying its tip
 * at H was X. So a candidate at H is the chain's iff the newest observation
 * at H+1 names it. */
static void test_reconcile_from_templates(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* Two competing candidates at the same height — expected on a
     * low-difficulty chain, and both rows must survive. */
    assert(store_record_block(s, 1000, 800001, "hash_win", "w1", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    assert(store_record_block(s, 1001, 800001, "hash_lose", "w2", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    /* A candidate whose next height was never observed. Unverifiable, so it
     * must stay pending — and pending is never revenue. */
    assert(store_record_block(s, 1002, 800004, "hash_unseen", "w1", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    assert(store_flush(s) == 0);

    store_template_t t = {
        .ts_s = 2000, .height = 800002, .prev_hash = "hash_win",
        .bits = "1d00ffff", .network_difficulty = 1.0,
        .coinbase_value_sats = 5000000000LL, .tx_count = 1,
        .tx_fees_sats = 0, .source = "enforcer", .cb_spendable = 1,
        .cb_op_returns = 2, .longpoll = 1, .rate_sats_per_diff = 0.0,
    };
    assert(store_record_template(s, &t) == 0);

    int confirmed = 0, orphaned = 0, pending = 0;
    assert(store_reconcile_blocks_from_templates(s, 800005, &confirmed,
                                                 &orphaned, &pending) == 0);
    assert(confirmed == 1);
    assert(orphaned == 1);
    assert(pending == 1);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    char st[32] = {0};
    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='hash_win'",
                st, sizeof st);
    assert(strcmp(st, "confirmed") == 0);
    /* tip 800005, block at 800001 → 5 confirmations. */
    assert(scalar_i64(db,
        "SELECT confirmations FROM blocks_found WHERE hash='hash_win'") == 5);
    scalar_text(db, "SELECT checked_via FROM blocks_found WHERE hash='hash_win'",
                st, sizeof st);
    assert(strcmp(st, "tips") == 0);

    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='hash_lose'",
                st, sizeof st);
    assert(strcmp(st, "orphaned") == 0);
    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='hash_unseen'",
                st, sizeof st);
    assert(strcmp(st, "pending") == 0);

    /* Both candidates at 800001 are still on record. Losing a race is not a
     * reason to forget the work was done. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE height=800001") == 2);

    /* A reorg: a later template at the same height now builds on someone
     * else. The confirmed block must be demoted, not left paid. */
    store_template_t t2 = t;
    t2.ts_s = 3000;
    t2.prev_hash = "hash_lose";
    t2.bits = "1d00fffe";   /* different work, so it opens its own row */
    assert(store_record_template(s, &t2) == 0);
    assert(store_reconcile_blocks_from_templates(s, 800006, &confirmed,
                                                 &orphaned, &pending) == 0);
    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='hash_win'",
                st, sizeof st);
    assert(strcmp(st, "orphaned") == 0);
    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='hash_lose'",
                st, sizeof st);
    assert(strcmp(st, "confirmed") == 0);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_reconcile_from_templates\n");
}

/* The unique index has to survive a table that already holds duplicates —
 * as a plain migration it would fail and be swallowed as a warning, leaving
 * it absent on exactly the databases that needed it. */
static void test_block_hash_index_after_dedupe(void) {
    const char *path = fresh_db_path();
    store_cfg_t cfg = {0};
    snprintf(cfg.path, sizeof(cfg.path), "%s", path);
    cfg.commit_window_ms = 20;
    cfg.commit_max_shares = 200;

    store_t *s = NULL;
    assert(store_open(&cfg, &s) == 0);

    /* The same solution recorded twice — the stratum dedupe ring is in
     * memory, so a restart can do this. */
    assert(store_record_block(s, 1000, 800001, "dup_hash", "w1", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    assert(store_record_block(s, 1001, 800001, "dup_hash", "w1", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    /* Distinct competing candidates must NOT be collapsed. */
    assert(store_record_block(s, 1002, 800001, "other_hash", "w2", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    assert(store_flush(s) == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(scalar_i64(db, "SELECT count(*) FROM blocks_found") == 3);

    /* A verdict reached on the duplicate must not be lost when it is
     * collapsed away. */
    assert(store_set_block_status(s, "dup_hash", STORE_BLOCK_CONFIRMED, 3,
                                  "tips") == 0);
    assert(sqlite3_exec(db, "UPDATE blocks_found SET status='pending' "
                            "WHERE id=(SELECT MIN(id) FROM blocks_found "
                            "           WHERE hash='dup_hash')",
                        NULL, NULL, NULL) == SQLITE_OK);

    assert(store_finalize_block_hash_index(s) == 0);

    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE hash='dup_hash'") == 1);
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE hash='other_hash'") == 1);
    char st[32] = {0};
    scalar_text(db, "SELECT status FROM blocks_found WHERE hash='dup_hash'",
                st, sizeof st);
    assert(strcmp(st, "confirmed") == 0);
    /* The index actually exists — the whole point. */
    assert(scalar_i64(db,
        "SELECT count(*) FROM sqlite_master WHERE type='index' "
        "  AND name='blocks_found_hash_idx'") == 1);

    /* And it now holds: a re-found hash cannot create a second row, and the
     * OR IGNORE means it does not fail the batch either. */
    assert(store_record_block(s, 1003, 800001, "dup_hash", "w1", "addr",
                              5000000000LL, 0, STORE_BLOCK_PENDING, NULL) == 0);
    assert(store_flush(s) == 0);
    assert(scalar_i64(db,
        "SELECT count(*) FROM blocks_found WHERE hash='dup_hash'") == 1);

    sqlite3_close(db);
    store_close(s);
    printf("  ok test_block_hash_index_after_dedupe\n");
}

int main(void) {
    log_init(2 /* WARN */);
    printf("running test_store...\n");
    test_basic();
    test_rejects();
    test_concurrent();
    test_drop();
    test_credited_sats();
    test_pool_identity();
    test_rate_history();
    test_template_history();
    test_template_retention();
    test_commit_survives_a_locked_db();
    test_block_candidate_status();
    test_reconcile_from_templates();
    test_block_hash_index_after_dedupe();
    test_open_upgrades_a_pre_status_database();
    cleanup_dbs();
    printf("all tests passed\n");
    return 0;
}
