/* SQLite-backed event store with a single batched writer thread.
 *
 * Producers enqueue events into a bounded ring buffer (mutex + cond).
 * The writer thread wakes either on signal or every commit_window_ms,
 * drains up to commit_max_shares events into one transaction, commits.
 *
 * Worker name -> id resolution is cached in a small open-addressing
 * hash table (16384 slots) to avoid hammering SQLite for repeats.
 */

#include "store.h"
#include "log.h"

#include <sqlite3.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* Keep in sync with schema.sql */
static const char *SCHEMA_SQL =
    "PRAGMA journal_mode = WAL;\n"
    "PRAGMA synchronous = NORMAL;\n"
    "PRAGMA foreign_keys = ON;\n"
    "CREATE TABLE IF NOT EXISTS workers ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name            TEXT UNIQUE NOT NULL,"
    "  first_seen      INTEGER NOT NULL,"
    "  last_seen       INTEGER NOT NULL,"
    "  payout_address  TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS shares ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_id   INTEGER NOT NULL REFERENCES workers(id),"
    "  ts          INTEGER NOT NULL,"
    "  difficulty  REAL NOT NULL,"
    "  is_block    INTEGER NOT NULL DEFAULT 0,"
    "  block_hash  TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS shares_ts_idx ON shares(ts);"
    "CREATE INDEX IF NOT EXISTS shares_worker_ts_idx ON shares(worker_id, ts);"
    "CREATE TABLE IF NOT EXISTS rejects ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  worker_name TEXT,"
    "  ts          INTEGER NOT NULL,"
    "  reason      TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS rejects_ts_idx ON rejects(ts);"
    "CREATE TABLE IF NOT EXISTS blocks_found ("
    "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts              INTEGER NOT NULL,"
    "  height          INTEGER NOT NULL,"
    "  hash            TEXT NOT NULL,"
    "  finder_id       INTEGER REFERENCES workers(id),"
    "  finder_address  TEXT,"
    "  reward_sats     INTEGER,"
    "  fee_sats        INTEGER"
    ");"
    "CREATE INDEX IF NOT EXISTS blocks_found_ts_idx ON blocks_found(ts);";

/* Forward-compat: ALTER existing DBs to add columns that didn't exist in
 * earlier schemas. Duplicate-column errors are silently ignored. */
static const char *MIGRATIONS_SQL[] = {
    "ALTER TABLE workers      ADD COLUMN payout_address TEXT",
    "ALTER TABLE blocks_found ADD COLUMN finder_address TEXT",
    "ALTER TABLE blocks_found ADD COLUMN reward_sats    INTEGER",
    "ALTER TABLE blocks_found ADD COLUMN fee_sats       INTEGER",
};

#define EV_SHARE   1
#define EV_REJECT  2
#define EV_BLOCK   3

#define WORKER_NAME_MAX 128
#define HASH_STR_MAX    96
#define REASON_MAX      128
#define ADDR_MAX        128

#define WORKER_CACHE_SLOTS 16384

typedef struct {
    uint8_t  kind;
    uint64_t ts_ms;
    double   difficulty;
    int      is_block;
    int      height;
    int64_t  reward_sats;       /* EV_BLOCK only */
    int64_t  fee_sats;          /* EV_BLOCK only */
    char     worker_name[WORKER_NAME_MAX];
    char     payout_address[ADDR_MAX];   /* EV_SHARE, EV_BLOCK: may be empty */
    char     hash[HASH_STR_MAX];
    char     reason[REASON_MAX];
} event_t;

typedef struct {
    char    name[WORKER_NAME_MAX];
    int64_t id;
    int     used;
} worker_slot_t;

struct store {
    sqlite3 *db;

    sqlite3_stmt *st_upsert_worker;
    sqlite3_stmt *st_get_worker;
    sqlite3_stmt *st_insert_share;
    sqlite3_stmt *st_insert_reject;
    sqlite3_stmt *st_insert_block;

    /* Ring buffer */
    event_t  *ring;
    size_t    ring_cap;
    size_t    ring_head;     /* write index */
    size_t    ring_tail;     /* read index */
    size_t    ring_count;

    pthread_mutex_t mu;
    pthread_cond_t  cv_not_empty;
    pthread_cond_t  cv_drained;     /* signaled when queue empties */

    pthread_t writer;
    int       writer_started;
    int       stop;

    int commit_window_ms;
    int commit_max_shares;

    worker_slot_t cache[WORKER_CACHE_SLOTS];

    _Atomic uint64_t shares_queued;
    _Atomic uint64_t shares_committed;
    _Atomic uint64_t shares_dropped;
    _Atomic uint64_t rejects_queued;
    _Atomic uint64_t rejects_committed;
    _Atomic uint64_t blocks_committed;
    _Atomic uint64_t batches;
    _Atomic uint64_t pg_errors;

    /* Sequence: monotonically increasing counter of enqueued events.
     * 'committed_seq' tracks the highest sequence that has been
     * persisted. flush() waits for committed_seq >= a snapshot of
     * enqueue_seq taken at flush() entry. */
    uint64_t enqueue_seq;
    uint64_t committed_seq;
    pthread_cond_t cv_committed;
};

static size_t g_test_ring_cap = 0;

void store_test_set_ring_capacity(size_t cap) { g_test_ring_cap = cap; }

/* ---- helpers ---------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint32_t name_hash(const char *s) {
    /* FNV-1a */
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static int64_t cache_lookup(store_t *s, const char *name) {
    uint32_t h = name_hash(name) & (WORKER_CACHE_SLOTS - 1);
    for (size_t i = 0; i < WORKER_CACHE_SLOTS; ++i) {
        size_t idx = (h + i) & (WORKER_CACHE_SLOTS - 1);
        if (!s->cache[idx].used) return -1;
        if (strncmp(s->cache[idx].name, name, WORKER_NAME_MAX) == 0)
            return s->cache[idx].id;
    }
    return -1;
}

static void cache_insert(store_t *s, const char *name, int64_t id) {
    uint32_t h = name_hash(name) & (WORKER_CACHE_SLOTS - 1);
    for (size_t i = 0; i < WORKER_CACHE_SLOTS; ++i) {
        size_t idx = (h + i) & (WORKER_CACHE_SLOTS - 1);
        if (!s->cache[idx].used) {
            s->cache[idx].used = 1;
            strncpy(s->cache[idx].name, name, WORKER_NAME_MAX - 1);
            s->cache[idx].name[WORKER_NAME_MAX - 1] = '\0';
            s->cache[idx].id = id;
            return;
        }
        if (strncmp(s->cache[idx].name, name, WORKER_NAME_MAX) == 0) {
            s->cache[idx].id = id;
            return;
        }
    }
    /* full - silently drop; future lookups go to DB */
}

static int64_t resolve_worker_id(store_t *s, const char *name,
                                 const char *payout_address,
                                 uint64_t ts_ms) {
    int64_t id = cache_lookup(s, name);
    int cached = (id >= 0);

    /* Schema stores Unix seconds; callers pass milliseconds. */
    const sqlite3_int64 ts_s = (sqlite3_int64)(ts_ms / 1000);

    sqlite3_reset(s->st_upsert_worker);
    sqlite3_clear_bindings(s->st_upsert_worker);
    sqlite3_bind_text(s->st_upsert_worker, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s->st_upsert_worker, 2, ts_s);
    sqlite3_bind_int64(s->st_upsert_worker, 3, ts_s);
    if (payout_address && payout_address[0]) {
        sqlite3_bind_text(s->st_upsert_worker, 4, payout_address, -1,
                          SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(s->st_upsert_worker, 4);
    }
    int rc = sqlite3_step(s->st_upsert_worker);
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(s->st_upsert_worker, 0);
    } else if (!cached) {
        atomic_fetch_add(&s->pg_errors, 1);
        id = -1;
    }
    sqlite3_reset(s->st_upsert_worker);
    if (id >= 0 && !cached) cache_insert(s, name, id);
    return id;
}

/* ---- writer thread ---------------------------------------------------- */

static void process_event(store_t *s, const event_t *ev) {
    if (ev->kind == EV_SHARE) {
        int64_t wid = resolve_worker_id(s, ev->worker_name,
                                        ev->payout_address, ev->ts_ms);
        if (wid < 0) {
            atomic_fetch_add(&s->pg_errors, 1);
            return;
        }
        sqlite3_reset(s->st_insert_share);
        sqlite3_clear_bindings(s->st_insert_share);
        sqlite3_bind_int64(s->st_insert_share, 1, wid);
        sqlite3_bind_int64(s->st_insert_share, 2, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_double(s->st_insert_share, 3, ev->difficulty);
        sqlite3_bind_int(s->st_insert_share, 4, ev->is_block);
        if (ev->hash[0])
            sqlite3_bind_text(s->st_insert_share, 5, ev->hash, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_share, 5);
        if (sqlite3_step(s->st_insert_share) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->shares_committed, 1);
        }
        sqlite3_reset(s->st_insert_share);
    } else if (ev->kind == EV_REJECT) {
        sqlite3_reset(s->st_insert_reject);
        sqlite3_clear_bindings(s->st_insert_reject);
        if (ev->worker_name[0])
            sqlite3_bind_text(s->st_insert_reject, 1, ev->worker_name, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_reject, 1);
        sqlite3_bind_int64(s->st_insert_reject, 2, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_text(s->st_insert_reject, 3, ev->reason, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s->st_insert_reject) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->rejects_committed, 1);
        }
        sqlite3_reset(s->st_insert_reject);
    } else if (ev->kind == EV_BLOCK) {
        int64_t finder = -1;
        if (ev->worker_name[0])
            finder = resolve_worker_id(s, ev->worker_name,
                                       ev->payout_address, ev->ts_ms);
        sqlite3_reset(s->st_insert_block);
        sqlite3_clear_bindings(s->st_insert_block);
        sqlite3_bind_int64(s->st_insert_block, 1, (sqlite3_int64)(ev->ts_ms / 1000));
        sqlite3_bind_int(s->st_insert_block, 2, ev->height);
        sqlite3_bind_text(s->st_insert_block, 3, ev->hash, -1, SQLITE_TRANSIENT);
        if (finder >= 0)
            sqlite3_bind_int64(s->st_insert_block, 4, finder);
        else
            sqlite3_bind_null(s->st_insert_block, 4);
        if (ev->payout_address[0])
            sqlite3_bind_text(s->st_insert_block, 5, ev->payout_address, -1,
                              SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(s->st_insert_block, 5);
        if (ev->reward_sats > 0)
            sqlite3_bind_int64(s->st_insert_block, 6, ev->reward_sats);
        else
            sqlite3_bind_null(s->st_insert_block, 6);
        if (ev->fee_sats > 0)
            sqlite3_bind_int64(s->st_insert_block, 7, ev->fee_sats);
        else
            sqlite3_bind_null(s->st_insert_block, 7);
        if (sqlite3_step(s->st_insert_block) != SQLITE_DONE) {
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            atomic_fetch_add(&s->blocks_committed, 1);
        }
        sqlite3_reset(s->st_insert_block);
    }
}

static void *writer_main(void *arg) {
    store_t *s = (store_t *)arg;

    event_t *batch = malloc(sizeof(event_t) * (size_t)s->commit_max_shares);
    if (!batch) {
        LOG_ERROR("store: writer batch alloc failed");
        return NULL;
    }

    pthread_mutex_lock(&s->mu);
    while (1) {
        /* Wait until: stop OR ring has events */
        while (!s->stop && s->ring_count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int wms = s->commit_window_ms;
            ts.tv_sec += wms / 1000;
            ts.tv_nsec += (long)(wms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&s->cv_not_empty, &s->mu, &ts);
            if (s->stop || s->ring_count > 0) break;
            /* timed out idle; loop */
            if (s->ring_count == 0) break;
        }

        if (s->stop && s->ring_count == 0) break;

        /* Drain up to commit_max_shares */
        size_t take = s->ring_count;
        if (take > (size_t)s->commit_max_shares) take = (size_t)s->commit_max_shares;
        if (take == 0) continue;

        for (size_t i = 0; i < take; ++i) {
            batch[i] = s->ring[s->ring_tail];
            s->ring_tail = (s->ring_tail + 1) % s->ring_cap;
        }
        s->ring_count -= take;
        uint64_t seq_after = s->enqueue_seq - (uint64_t)s->ring_count;
        pthread_mutex_unlock(&s->mu);

        /* BEGIN/COMMIT outside the producer mutex */
        char *err = NULL;
        if (sqlite3_exec(s->db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
            LOG_ERROR("store: BEGIN failed: %s", err ? err : "?");
            sqlite3_free(err);
            atomic_fetch_add(&s->pg_errors, 1);
        } else {
            for (size_t i = 0; i < take; ++i) process_event(s, &batch[i]);
            if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
                LOG_ERROR("store: COMMIT failed: %s", err ? err : "?");
                sqlite3_free(err);
                sqlite3_exec(s->db, "ROLLBACK", NULL, NULL, NULL);
                atomic_fetch_add(&s->pg_errors, 1);
            } else {
                atomic_fetch_add(&s->batches, 1);
            }
        }

        pthread_mutex_lock(&s->mu);
        s->committed_seq = seq_after;
        pthread_cond_broadcast(&s->cv_committed);
        if (s->ring_count == 0) pthread_cond_broadcast(&s->cv_drained);
    }
    pthread_mutex_unlock(&s->mu);

    free(batch);
    return NULL;
}

/* ---- enqueue ---------------------------------------------------------- */

static int enqueue(store_t *s, const event_t *ev) {
    pthread_mutex_lock(&s->mu);
    if (s->ring_count == s->ring_cap) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    s->ring[s->ring_head] = *ev;
    s->ring_head = (s->ring_head + 1) % s->ring_cap;
    s->ring_count++;
    s->enqueue_seq++;
    pthread_cond_signal(&s->cv_not_empty);
    pthread_mutex_unlock(&s->mu);
    return 0;
}

/* ---- public API ------------------------------------------------------- */

int store_open(const store_cfg_t *cfg, store_t **out) {
    if (!cfg || !out) return -1;
    store_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;

    s->commit_window_ms = cfg->commit_window_ms > 0 ? cfg->commit_window_ms : 100;
    s->commit_max_shares = cfg->commit_max_shares > 0 ? cfg->commit_max_shares : 100;
    s->ring_cap = g_test_ring_cap > 0 ? g_test_ring_cap : 65536;
    s->ring = calloc(s->ring_cap, sizeof(event_t));
    if (!s->ring) { free(s); return -1; }

    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv_not_empty, NULL);
    pthread_cond_init(&s->cv_drained, NULL);
    pthread_cond_init(&s->cv_committed, NULL);

    int rc = sqlite3_open_v2(cfg->path, &s->db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("store: sqlite3_open(%s) failed: %s", cfg->path,
                  s->db ? sqlite3_errmsg(s->db) : "?");
        if (s->db) sqlite3_close(s->db);
        free(s->ring); free(s);
        return -2;
    }

    char *err = NULL;
    if (sqlite3_exec(s->db, SCHEMA_SQL, NULL, NULL, &err) != SQLITE_OK) {
        LOG_ERROR("store: schema apply failed: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(s->db);
        free(s->ring); free(s);
        return -3;
    }
    /* Best-effort migrations for DBs created by an older simplepool. Each
     * ALTER returns "duplicate column" on already-migrated DBs, which is
     * expected — only log other failures. */
    for (size_t i = 0; i < sizeof(MIGRATIONS_SQL) / sizeof(MIGRATIONS_SQL[0]); ++i) {
        err = NULL;
        if (sqlite3_exec(s->db, MIGRATIONS_SQL[i], NULL, NULL, &err) != SQLITE_OK) {
            if (err && !strstr(err, "duplicate column")) {
                LOG_WARN("store: migration '%s' failed: %s",
                         MIGRATIONS_SQL[i], err);
            }
            sqlite3_free(err);
        }
    }

    /* Prepared statements. The workers upsert sets payout_address on first
     * INSERT only — once set, it is immutable for that worker name. */
    static const char *Q_UPSERT =
        "INSERT INTO workers (name, first_seen, last_seen, payout_address) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  last_seen      = excluded.last_seen, "
        "  payout_address = COALESCE(workers.payout_address, excluded.payout_address) "
        "RETURNING id";
    static const char *Q_INS_SHARE =
        "INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash) "
        "VALUES (?, ?, ?, ?, ?)";
    static const char *Q_INS_REJECT =
        "INSERT INTO rejects (worker_name, ts, reason) VALUES (?, ?, ?)";
    static const char *Q_INS_BLOCK =
        "INSERT INTO blocks_found "
        "  (ts, height, hash, finder_id, finder_address, reward_sats, fee_sats) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)";

    if (sqlite3_prepare_v2(s->db, Q_UPSERT, -1, &s->st_upsert_worker, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_SHARE, -1, &s->st_insert_share, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_REJECT, -1, &s->st_insert_reject, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, Q_INS_BLOCK, -1, &s->st_insert_block, NULL) != SQLITE_OK)
    {
        LOG_ERROR("store: prepare failed: %s", sqlite3_errmsg(s->db));
        store_close(s);
        return -4;
    }

    if (pthread_create(&s->writer, NULL, writer_main, s) != 0) {
        LOG_ERROR("store: pthread_create failed: %s", strerror(errno));
        store_close(s);
        return -5;
    }
    s->writer_started = 1;

    LOG_INFO("store: opened %s (ring=%zu, window=%dms, batch=%d)",
             cfg->path, s->ring_cap, s->commit_window_ms, s->commit_max_shares);
    *out = s;
    return 0;
}

void store_close(store_t *s) {
    if (!s) return;
    if (s->writer_started) {
        pthread_mutex_lock(&s->mu);
        s->stop = 1;
        pthread_cond_broadcast(&s->cv_not_empty);
        pthread_mutex_unlock(&s->mu);
        pthread_join(s->writer, NULL);
    }
    if (s->st_upsert_worker) sqlite3_finalize(s->st_upsert_worker);
    if (s->st_get_worker)    sqlite3_finalize(s->st_get_worker);
    if (s->st_insert_share)  sqlite3_finalize(s->st_insert_share);
    if (s->st_insert_reject) sqlite3_finalize(s->st_insert_reject);
    if (s->st_insert_block)  sqlite3_finalize(s->st_insert_block);
    if (s->db) sqlite3_close(s->db);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv_not_empty);
    pthread_cond_destroy(&s->cv_drained);
    pthread_cond_destroy(&s->cv_committed);
    free(s->ring);
    free(s);
}

int store_record_share(store_t *s, const char *worker_name,
                       uint64_t ts_ms, double difficulty,
                       int is_block, const char *block_hash_or_null)
{
    return store_record_share_addr(s, worker_name, NULL, ts_ms, difficulty,
                                   is_block, block_hash_or_null);
}

int store_record_share_addr(store_t *s, const char *worker_name,
                            const char *payout_address,
                            uint64_t ts_ms, double difficulty,
                            int is_block, const char *block_hash_or_null)
{
    if (!s || !worker_name) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_SHARE;
    ev.ts_ms = ts_ms;
    ev.difficulty = difficulty;
    ev.is_block = is_block;
    strncpy(ev.worker_name, worker_name, WORKER_NAME_MAX - 1);
    if (payout_address)
        strncpy(ev.payout_address, payout_address, ADDR_MAX - 1);
    if (block_hash_or_null) {
        strncpy(ev.hash, block_hash_or_null, HASH_STR_MAX - 1);
    }
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    atomic_fetch_add(&s->shares_queued, 1);
    return 0;
}

int store_record_reject(store_t *s, const char *worker_name,
                        uint64_t ts_ms, const char *reason)
{
    if (!s || !reason) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_REJECT;
    ev.ts_ms = ts_ms;
    if (worker_name) strncpy(ev.worker_name, worker_name, WORKER_NAME_MAX - 1);
    strncpy(ev.reason, reason, REASON_MAX - 1);
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    atomic_fetch_add(&s->rejects_queued, 1);
    return 0;
}

int store_record_block(store_t *s, uint64_t ts_ms, int height,
                       const char *hash, const char *finder_name,
                       const char *finder_address,
                       int64_t reward_sats, int64_t fee_sats)
{
    if (!s || !hash) return -1;
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = EV_BLOCK;
    ev.ts_ms = ts_ms;
    ev.height = height;
    ev.reward_sats = reward_sats;
    ev.fee_sats = fee_sats;
    strncpy(ev.hash, hash, HASH_STR_MAX - 1);
    if (finder_name) strncpy(ev.worker_name, finder_name, WORKER_NAME_MAX - 1);
    if (finder_address)
        strncpy(ev.payout_address, finder_address, ADDR_MAX - 1);
    if (enqueue(s, &ev) != 0) {
        atomic_fetch_add(&s->shares_dropped, 1);
        return -1;
    }
    return 0;
}

int store_flush(store_t *s) {
    if (!s) return -1;
    uint64_t target;
    pthread_mutex_lock(&s->mu);
    target = s->enqueue_seq;
    pthread_cond_signal(&s->cv_not_empty);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;

    while (s->committed_seq < target) {
        int rc = pthread_cond_timedwait(&s->cv_committed, &s->mu, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mu);
            return -1;
        }
    }
    pthread_mutex_unlock(&s->mu);

    /* Avoid unused-warning suppression */
    (void)now_ms;
    return 0;
}

void store_get_stats(store_t *s, store_stats_t *out) {
    if (!s || !out) return;
    out->shares_queued    = atomic_load(&s->shares_queued);
    out->shares_committed = atomic_load(&s->shares_committed);
    out->shares_dropped   = atomic_load(&s->shares_dropped);
    out->rejects_queued   = atomic_load(&s->rejects_queued);
    out->rejects_committed= atomic_load(&s->rejects_committed);
    out->blocks_committed = atomic_load(&s->blocks_committed);
    out->batches          = atomic_load(&s->batches);
    out->pg_errors        = atomic_load(&s->pg_errors);
}
