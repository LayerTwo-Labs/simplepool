/* Stratum V1 server: TCP listener + thread-per-connection.
 *
 * Wire format: newline-delimited JSON-RPC 2.0. Methods handled:
 *   mining.subscribe, mining.authorize, mining.submit
 *
 * Concurrency: an rwlock guards `current_job`. set_job swaps the pointer
 * under a write lock and pushes the previous job into a small ring of
 * "recent jobs" kept alive ~60s for late submits. Connection threads take
 * read locks for notify/submit lookups.
 *
 * Jobs are reference counted. Anything that reads a job only while holding
 * the lock that guards its slot (send_current_notify, current_net_diff) needs
 * nothing more; find_job hands out a counted reference because a submit
 * outlives the lock, and the tip watcher frees jobs from under it otherwise.
 *
 * Vardiff adjusts each connection's difficulty toward cfg.vardiff_target_spm
 * shares/minute, clamped so the share target never exceeds the network
 * target (see vardiff_maybe_retarget).
 */

#define _POSIX_C_SOURCE 200809L
#include "stratum.h"
#include "coinbase.h"
#include "share.h"
#include "log.h"
#include "thunder.h"
#include "cjson/cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE_BYTES 16384
#define DEDUPE_RING    1024
/* Server-wide ring, so it has to cover every live connection's recent
 * submissions rather than just one's. */
#define SHARE_DEDUPE_RING 16384
/* Matches store.c's REASON_MAX so a submitblock reason survives the trip to
 * the DB intact rather than being truncated twice. */
#define REASON_TEXT_MAX   128
#define RECENT_JOBS    8
#define RECENT_JOB_TTL_MS 60000
/* Per-connection record of the difficulty each job went out under. Only jobs
 * find_job() can still resolve are ever submitted against -- the current one
 * plus RECENT_JOBS retired -- so anything past that is unreachable. Sized
 * above it so the entry is still there when the submit arrives. */
#define JOB_DIFF_RING  16

/* BIP320 reserved version-rolling bits (ASICBoost). Advertised in
 * mining.configure; only these block-header version bits may be rolled by a
 * miner, and a per-connection mask (this ANDed with the client's request) is
 * applied to every submitted version. */
#define VERSION_ROLLING_MASK 0x1fffe000u

/* ============================================================== job ===== */

struct stratum_job {
    char     job_id[32];
    int32_t  version;
    uint8_t  prev_hash_le[32];

    /* Template-level inputs for per-connection coinbase rendering. */
    int64_t  value_sats;
    char    *wc_hex;          /* witness commitment hex, owned, may be NULL */
    /* Server-provided coinbase (BIP22 "coinbasetxn"), owned, may be NULL. When
     * set, the per-connection coinbase is built from this rather than from
     * scratch; coinbase_has_witness says whether to re-attach the witness
     * reserved value when assembling a found block. */
    char    *coinbasetxn_hex;
    int      coinbase_has_witness;
    size_t   en1_size;
    size_t   en2_size;

    uint8_t (*merkle_branches)[32];
    size_t   branch_count;
    uint32_t nbits;
    uint32_t ntime;
    uint8_t  network_target_be[32];
    uint32_t height;

    char   **tx_hex_list;   /* owned */
    size_t   tx_count;

    uint64_t created_ms;    /* for retention ring */

    /* References held. The server holds one for current_job and one for each
     * ring slot; a submit handler holds one for as long as it is reading the
     * job. Destroyed when the last is dropped.
     *
     * Without this a submit read a job the tip watcher had already freed:
     * find_job() released its lock before returning the pointer, and
     * retire_job() frees on every new template. On a chain where templates
     * arrive several times a second the ring turns over in seconds, so the
     * window was wide open — it produced blocks_found rows carrying a freed
     * job's height and value (0, 2, 550 and rewards of 1.29M BTC on the
     * production pool), and a garbage network_target_be can make any hash
     * look like a solved block. */
    _Atomic int refs;
};

stratum_job_t *stratum_job_new(
    const char *job_id,
    int32_t version,
    const uint8_t prev_hash_le[32],
    int64_t value_sats,
    const char *witness_commitment_hex,
    size_t en1_size, size_t en2_size,
    const uint8_t (*merkle_branches)[32], size_t branch_count,
    uint32_t nbits, uint32_t ntime,
    const uint8_t network_target_be[32],
    uint32_t height,
    const char *const *tx_hex_list, size_t tx_count,
    const char *coinbasetxn_hex, int coinbase_has_witness)
{
    stratum_job_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    /* Set before anything can `goto fail`: the failure path releases, and a
     * count of 0 there would decrement past zero and leak instead of free. */
    atomic_init(&j->refs, 1);
    snprintf(j->job_id, sizeof(j->job_id), "%s", job_id ? job_id : "");
    j->version = version;
    if (prev_hash_le) memcpy(j->prev_hash_le, prev_hash_le, 32);
    j->value_sats = value_sats;
    j->en1_size   = en1_size;
    j->en2_size   = en2_size;
    j->coinbase_has_witness = coinbase_has_witness;
    if (witness_commitment_hex && *witness_commitment_hex) {
        j->wc_hex = strdup(witness_commitment_hex);
        if (!j->wc_hex) goto fail;
    }
    if (coinbasetxn_hex && *coinbasetxn_hex) {
        j->coinbasetxn_hex = strdup(coinbasetxn_hex);
        if (!j->coinbasetxn_hex) goto fail;
    }
    if (branch_count) {
        j->merkle_branches = calloc(branch_count, sizeof(*j->merkle_branches));
        if (!j->merkle_branches) goto fail;
        memcpy(j->merkle_branches, merkle_branches, branch_count * 32);
        j->branch_count = branch_count;
    }
    j->nbits = nbits;
    j->ntime = ntime;
    if (network_target_be) memcpy(j->network_target_be, network_target_be, 32);
    j->height = height;
    if (tx_count && tx_hex_list) {
        j->tx_hex_list = calloc(tx_count, sizeof(char *));
        if (!j->tx_hex_list) goto fail;
        for (size_t i = 0; i < tx_count; ++i) {
            j->tx_hex_list[i] = tx_hex_list[i] ? strdup(tx_hex_list[i]) : strdup("");
            if (!j->tx_hex_list[i]) goto fail;
        }
        j->tx_count = tx_count;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    j->created_ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    return j;
fail:
    stratum_job_free(j);
    return NULL;
}

/* Take a reference. Callers must hold whichever lock protects the pointer
 * they are reading it from, so the job cannot be destroyed between the load
 * and the increment. */
static void stratum_job_retain(stratum_job_t *j) {
    if (j) atomic_fetch_add_explicit(&j->refs, 1, memory_order_relaxed);
}

/* Drop a reference; destroys on the last one. Named `free` because that is
 * what every existing caller means by it — the server's ring slot, the
 * current-job slot, and main.c's error paths each hold exactly one. */
void stratum_job_free(stratum_job_t *j) {
    if (!j) return;
    if (atomic_fetch_sub_explicit(&j->refs, 1, memory_order_acq_rel) != 1) return;
    atomic_thread_fence(memory_order_acquire);
    free(j->wc_hex);
    free(j->coinbasetxn_hex);
    free(j->merkle_branches);
    if (j->tx_hex_list) {
        for (size_t i = 0; i < j->tx_count; ++i) free(j->tx_hex_list[i]);
        free(j->tx_hex_list);
    }
    free(j);
}

/* ============================================================ server ==== */

struct stratum_server {
    stratum_cfg_t cfg;

    /* One entry per bound port. Each carries the difficulty policy the
     * connections it accepts inherit, so the port a miner dials decides what
     * difficulty it is served at. */
    struct stratum_listener_slot {
        stratum_server_t  *srv;
        stratum_listener_t pol;
        int                fd;
        pthread_t          thr;
        int                thr_started;
    } listeners[STRATUM_MAX_LISTENERS];
    int  listener_count;
    atomic_int  stop;
    atomic_int  conn_count;
    /* Seeded from the clock at startup (so values differ across restarts)
     * and incremented per subscribe, which is what makes each connection's
     * extranonce1 distinct. Do not mix it with the clock again at use. */
    atomic_uint extranonce1_seq;

    /* Server-wide share dedupe, keyed on the resulting block-header hash.
     * The per-connection ring in stratum_conn cannot catch a duplicate that
     * arrives on a *different* connection, and two connections handed the
     * same extranonce1 render identical coinbases — so the same nonce
     * yields the same hash on both, and PPS would credit it twice. Keying
     * on the final hash makes the check independent of how the submission
     * was framed (job id, extranonce2, version rolling). */
    pthread_mutex_t share_dedupe_lock;
    uint64_t        share_dedupe[SHARE_DEDUPE_RING];
    size_t          share_dedupe_head;

    pthread_rwlock_t job_lock;
    stratum_job_t   *current_job;          /* protected by job_lock */
    stratum_job_t   *recent[RECENT_JOBS];  /* small retention ring */
    size_t           recent_head;
    pthread_mutex_t  recent_lock;

    /* List of live connections — for broadcasting notify on job swap. */
    pthread_mutex_t  conns_lock;
    struct stratum_conn *conns_head;
};

struct stratum_conn {
    stratum_server_t *server;
    int fd;                  /* -1 in tests */
    pthread_t thr;
    int thr_started;

    uint8_t  extranonce1[STRATUM_EXTRANONCE1_SIZE];
    double   difficulty;

    /* Difficulty policy inherited from the listener this connection was
     * accepted on, resolved once at accept time so nothing downstream has to
     * know which port it came in on. Seeded from the server-wide defaults,
     * which is what a test connection and the default listener both get. */
    double   pol_initial_diff;
    double   pol_vardiff_min;
    double   pol_vardiff_max;
    int      pol_port;
    char     pol_label[32];
    int      subscribed;
    int      authorized;
    uint32_t version_mask;         /* negotiated version-rolling bits; 0 = off */
    char     worker_name[129];     /* full stratum username (sanitized) */
    char     payout_address[128];  /* validated bech32/base58 */

    /* Per-connection coinbase, rendered against the current job using
     * payout_address (miner) + cfg.operator_address (fee). Refreshed any
     * time we hand out a new notify for a job id we haven't rendered
     * coinbase for yet. */
    uint8_t *cb1;
    size_t   cb1_len;
    uint8_t *cb2;
    size_t   cb2_len;
    char     cb_for_job_id[32];

    /* Dedupe ring. Each entry is a small hash of
     * (job_id|en2|ntime|nonce|version). */
    uint64_t dedupe[DEDUPE_RING];
    size_t   dedupe_head;

    /* Vardiff window state — counts accepted shares since vd_window_start_ms.
     * Every cfg.vardiff_window_sec the rate is compared to vardiff_target_spm
     * and `difficulty` is multiplied/divided to converge on the target.
     *
     * vd_window_min_achieved is the smallest difficulty any share in the
     * window actually achieved (HUGE_VAL until the first share lands). It
     * detects a miner whose own local difficulty floor sits far above what
     * we assigned it — see vardiff_maybe_retarget. */
    uint64_t vd_window_start_ms;
    uint32_t vd_window_shares;
    double   vd_window_min_achieved;

    /* The difficulty each job was notified to this connection under, so a
     * submit is judged at the difficulty that was in force for THAT job
     * rather than whatever the connection has drifted to since. A miner
     * applies a set_difficulty on a later job, so shares for jobs already in
     * hand keep arriving at the difficulty those jobs went out under, and on
     * a slow chain they keep arriving well past the vardiff window.
     *
     * Written by whichever thread sends the notify — the connection's own
     * thread on authorize, the job-swap thread on broadcast — and read by
     * the connection thread on submit, so it carries its own lock. */
    pthread_mutex_t  jobdiff_lock;
    struct { char job_id[32]; double difficulty; } job_diffs[JOB_DIFF_RING];
    size_t   job_diffs_head;

    /* Submit rate limiting. A fixed one-second window: cheaper than a token
     * bucket and the burst it lets through is one window's worth, which at
     * these magnitudes is the same protection. Measured on the monotonic
     * clock -- these are intervals, and an NTP step must not hand a
     * connection a fresh window or freeze it in one.
     *
     * rl_limited counts what has been refused since the last time we said so.
     * Reporting every refusal would put the flood straight into the reject
     * table -- tens of thousands of rows a second describing one condition --
     * so the count is folded into a single periodic report instead. */
    uint64_t rl_window_start_ms;
    uint32_t rl_window_count;
    uint32_t rl_limited;        /* refused since the last report */
    uint64_t rl_limited_total;  /* refused over the connection's life */
    uint64_t rl_reported_ms;

    /* Monotonic timestamp of the most recent recv() that got any bytes.
     * The conn thread checks this against cfg.idle_timeout_sec after each
     * SO_RCVTIMEO wake-up so silent connections are reaped. */
    uint64_t last_activity_ms;

    pthread_mutex_t write_lock;

    struct stratum_conn *next;  /* server->conns_head linked list */
};

static void conn_clear_coinbase(stratum_conn_t *c) {
    free(c->cb1); c->cb1 = NULL; c->cb1_len = 0;
    free(c->cb2); c->cb2 = NULL; c->cb2_len = 0;
    c->cb_for_job_id[0] = '\0';
}

/* ----------------------------------------------------- helpers ---------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Decode hex into out (exactly outlen bytes). Returns 0 on success. */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
    if (!hex) return -1;
    size_t hl = strlen(hex);
    if (hl != outlen * 2) return -1;
    for (size_t i = 0; i < outlen; ++i) {
        int hi = hex_nib(hex[2 * i]);
        int lo = hex_nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Decode an arbitrary-length hex string. Returns malloc'd buffer, *outlen
 * set, or NULL on error. */
static uint8_t *hex_to_bytes_alloc(const char *hex, size_t *outlen) {
    if (!hex) return NULL;
    size_t hl = strlen(hex);
    if (hl % 2) return NULL;
    size_t n = hl / 2;
    uint8_t *out = malloc(n ? n : 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; ++i) {
        int hi = hex_nib(hex[2 * i]);
        int lo = hex_nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *outlen = n;
    return out;
}

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = H[(bytes[i] >> 4) & 0xf];
        out[2 * i + 1] = H[bytes[i] & 0xf];
    }
    out[2 * n] = '\0';
}

/* Parse a hex u32, big-endian semantics: e.g. "5f5e1000" -> 0x5f5e1000. */
static int parse_u32_hex(const char *hex, uint32_t *out) {
    uint8_t b[4];
    if (hex_to_bytes(hex, b, 4) != 0) return -1;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
    return 0;
}

static void sanitize_worker(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < outlen; ++i) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t fnv1a_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- output buffer helpers ---- */

static int buf_append(char **buf, size_t *len, const char *s, size_t n) {
    char *nb = realloc(*buf, *len + n + 1);
    if (!nb) return -1;
    *buf = nb;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int buf_append_json_line(char **buf, size_t *len, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return -1;
    int rc = buf_append(buf, len, s, strlen(s));
    if (rc == 0) rc = buf_append(buf, len, "\n", 1);
    free(s);
    return rc;
}

/* Is PPS accrual currently suspended? While it is, work handed to this pool
 * earns nothing, so the pool says so rather than banking it silently. */
static int pps_gated(const stratum_server_t *s) {
    return s->cfg.pps_enabled && s->cfg.pps_refuse_shares_below_min &&
           s->cfg.pps_gate &&
           atomic_load_explicit(s->cfg.pps_gate, memory_order_relaxed) != 0;
}

#define PPS_GATED_MSG \
    "pool is not crediting shares right now: network difficulty is below " \
    "the minimum this pool will pay PPS at. Point your miner elsewhere " \
    "until it retargets."

/* ---- job retention ring ---- */

static void retire_job(stratum_server_t *s, stratum_job_t *j) {
    if (!j) return;
    pthread_mutex_lock(&s->recent_lock);
    /* Sweep expired. */
    uint64_t cutoff = mono_ms();
    for (size_t i = 0; i < RECENT_JOBS; ++i) {
        if (s->recent[i] && cutoff - s->recent[i]->created_ms > RECENT_JOB_TTL_MS) {
            stratum_job_free(s->recent[i]);
            s->recent[i] = NULL;
        }
    }
    /* Free whatever is in the slot we are about to overwrite. */
    if (s->recent[s->recent_head]) {
        stratum_job_free(s->recent[s->recent_head]);
    }
    s->recent[s->recent_head] = j;
    s->recent_head = (s->recent_head + 1) % RECENT_JOBS;
    pthread_mutex_unlock(&s->recent_lock);
}

/* Find a job by id in the current slot or the recent ring.
 *
 * Returns a COUNTED reference: the caller owns it and must
 * stratum_job_free() it. The retain happens under the same lock that guards
 * the slot, so the job cannot be destroyed between finding it and claiming
 * it. Returning a borrowed pointer here — the previous behaviour — meant a
 * submit could still be reading a job that retire_job() had freed on the tip
 * watcher thread, which is a use-after-free in a network-facing path. */
static stratum_job_t *find_job(stratum_server_t *s, const char *job_id) {
    if (!job_id) return NULL;
    /* current */
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *cur = s->current_job;
    if (cur && strcmp(cur->job_id, job_id) == 0) {
        stratum_job_retain(cur);
        pthread_rwlock_unlock(&s->job_lock);
        return cur;
    }
    pthread_rwlock_unlock(&s->job_lock);
    /* recent */
    pthread_mutex_lock(&s->recent_lock);
    for (size_t i = 0; i < RECENT_JOBS; ++i) {
        if (s->recent[i] && strcmp(s->recent[i]->job_id, job_id) == 0) {
            stratum_job_t *r = s->recent[i];
            stratum_job_retain(r);
            pthread_mutex_unlock(&s->recent_lock);
            return r;
        }
    }
    pthread_mutex_unlock(&s->recent_lock);
    return NULL;
}

/* ---- notify payload ---- */

/* Build a mining.notify params array. cb1/cb2 are supplied separately
 * because they are rendered per-connection (each miner's coinbase pays
 * that miner's payout_address). */
static cJSON *make_notify_params(const stratum_job_t *j,
                                 const uint8_t *cb1, size_t cb1_len,
                                 const uint8_t *cb2, size_t cb2_len,
                                 int clean_jobs) {
    cJSON *p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateString(j->job_id));

    /* prev_hash: mining.notify uses the stratum convention where the 32-byte
     * hash is sent with each 4-byte word byte-reversed (word order preserved).
     * prev_hash_le holds the header-internal little-endian bytes, so we
     * word-swap before emitting; the miner word-swaps again to recover the
     * exact bytes that go into the block header. Sending the raw little-endian
     * bytes makes standard ASICs hash the wrong header (every share rejected
     * as "low difficulty"). */
    char hex[65];
    uint8_t prev_ws[32];
    for (int wi = 0; wi < 8; ++wi)
        for (int bi = 0; bi < 4; ++bi)
            prev_ws[wi * 4 + bi] = j->prev_hash_le[wi * 4 + 3 - bi];
    bytes_to_hex(prev_ws, 32, hex);
    cJSON_AddItemToArray(p, cJSON_CreateString(hex));

    char *cb1_hex = malloc(cb1_len * 2 + 1);
    char *cb2_hex = malloc(cb2_len * 2 + 1);
    if (!cb1_hex || !cb2_hex) {
        free(cb1_hex); free(cb2_hex); cJSON_Delete(p); return NULL;
    }
    bytes_to_hex(cb1, cb1_len, cb1_hex);
    bytes_to_hex(cb2, cb2_len, cb2_hex);
    cJSON_AddItemToArray(p, cJSON_CreateString(cb1_hex));
    cJSON_AddItemToArray(p, cJSON_CreateString(cb2_hex));
    free(cb1_hex); free(cb2_hex);

    cJSON *branches = cJSON_CreateArray();
    for (size_t i = 0; i < j->branch_count; ++i) {
        bytes_to_hex(j->merkle_branches[i], 32, hex);
        cJSON_AddItemToArray(branches, cJSON_CreateString(hex));
    }
    cJSON_AddItemToArray(p, branches);

    char vhex[9], thex[9], nhex[9];
    snprintf(vhex, sizeof(vhex), "%08x", (uint32_t)j->version);
    snprintf(thex, sizeof(thex), "%08x", j->nbits);
    snprintf(nhex, sizeof(nhex), "%08x", j->ntime);
    cJSON_AddItemToArray(p, cJSON_CreateString(vhex));
    cJSON_AddItemToArray(p, cJSON_CreateString(thex));
    cJSON_AddItemToArray(p, cJSON_CreateString(nhex));
    cJSON_AddItemToArray(p, cJSON_CreateBool(clean_jobs ? 1 : 0));
    return p;
}

/* Render a fresh coinbase for `c` against `job` using c->payout_address
 * and the server's operator_address / fee_bps / coinbase_tag. Caches into
 * c->cb1/cb2 keyed by job->job_id. Returns 0 ok, negative on error. */
static int conn_render_coinbase(stratum_server_t *s, stratum_conn_t *c,
                                const stratum_job_t *job) {
    if (!c->authorized || c->payout_address[0] == '\0') return -1;
    if (c->cb_for_job_id[0] && strcmp(c->cb_for_job_id, job->job_id) == 0) {
        return 0; /* cached */
    }
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int rc;
    if (s->cfg.pps_enabled) {
        /* PPS-classic: every miner's coinbase is identical, paying the
         * pool's BTC wallet for the net-of-fee reward and the operator
         * address for the fee. The operator later moves accumulated BTC
         * into Thunder via the admin dashboard's deposit action; per-miner
         * accounting happens off-chain via pps_credits. */
        if (job->coinbasetxn_hex) {
            rc = coinbase_build_from_template(job->coinbasetxn_hex,
                                              s->cfg.pool_btc_address,
                                              s->cfg.operator_address, s->cfg.fee_bps,
                                              s->cfg.coinbase_tag,
                                              job->en1_size, job->en2_size,
                                              &parts, NULL, NULL, NULL, err, sizeof err);
        } else {
            rc = coinbase_build_split(job->height, job->value_sats,
                                      s->cfg.pool_btc_address,
                                      s->cfg.operator_address, s->cfg.fee_bps,
                                      job->wc_hex, s->cfg.coinbase_tag,
                                      job->en1_size, job->en2_size,
                                      &parts, NULL, NULL, err, sizeof err);
        }
    } else if (job->coinbasetxn_hex) {
        /* Backend dictated the coinbase (e.g. CUSF enforcer): build from it,
         * redirecting the reward output to this miner and preserving the
         * mandatory commitment outputs. The witness commitment is already in
         * the server's coinbase, so job->wc_hex is not used here. */
        rc = coinbase_build_from_template(job->coinbasetxn_hex,
                                          c->payout_address,
                                          s->cfg.operator_address, s->cfg.fee_bps,
                                          s->cfg.coinbase_tag,
                                          job->en1_size, job->en2_size,
                                          &parts, NULL, NULL, NULL, err, sizeof err);
    } else {
        rc = coinbase_build_split(job->height, job->value_sats,
                                  c->payout_address,
                                  s->cfg.operator_address, s->cfg.fee_bps,
                                  job->wc_hex, s->cfg.coinbase_tag,
                                  job->en1_size, job->en2_size,
                                  &parts, NULL, NULL, err, sizeof err);
    }
    if (rc < 0) {
        LOG_WARN("stratum: coinbase render failed for %s: %s",
                 c->worker_name, err);
        return -1;
    }
    free(c->cb1); free(c->cb2);
    c->cb1 = parts.cb1; c->cb1_len = parts.cb1_len;
    c->cb2 = parts.cb2; c->cb2_len = parts.cb2_len;
    snprintf(c->cb_for_job_id, sizeof c->cb_for_job_id, "%s", job->job_id);
    return 0;
}

static int emit_notification(char **buf, size_t *len, const char *method, cJSON *params) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "id", cJSON_CreateNull());
    cJSON_AddStringToObject(obj, "method", method);
    cJSON_AddItemToObject(obj, "params", params);
    int rc = buf_append_json_line(buf, len, obj);
    cJSON_Delete(obj);
    return rc;
}

static int emit_response(char **buf, size_t *len, cJSON *id, cJSON *result, cJSON *err) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(obj, "result", result ? result : cJSON_CreateNull());
    cJSON_AddItemToObject(obj, "error",  err    ? err    : cJSON_CreateNull());
    int rc = buf_append_json_line(buf, len, obj);
    cJSON_Delete(obj);
    return rc;
}

static cJSON *make_error(int code, const char *msg) {
    cJSON *e = cJSON_CreateArray();
    cJSON_AddItemToArray(e, cJSON_CreateNumber(code));
    cJSON_AddItemToArray(e, cJSON_CreateString(msg));
    cJSON_AddItemToArray(e, cJSON_CreateNull());
    return e;
}

/* ---- varint for block assembly ---- */

static void varint_append(uint8_t **buf, size_t *cap, size_t *len, uint64_t n) {
    /* ensure 9 bytes */
    if (*len + 9 > *cap) {
        size_t nc = (*cap ? *cap * 2 : 64);
        while (nc < *len + 9) nc *= 2;
        uint8_t *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    uint8_t *p = *buf + *len;
    if (n < 0xfd) { p[0] = (uint8_t)n; *len += 1; return; }
    if (n <= 0xffff) {
        p[0] = 0xfd; p[1] = (uint8_t)(n & 0xff); p[2] = (uint8_t)((n >> 8) & 0xff);
        *len += 3; return;
    }
    if (n <= 0xffffffffULL) {
        p[0] = 0xfe;
        for (int i = 0; i < 4; ++i) p[1 + i] = (uint8_t)((n >> (8 * i)) & 0xff);
        *len += 5; return;
    }
    p[0] = 0xff;
    for (int i = 0; i < 8; ++i) p[1 + i] = (uint8_t)((n >> (8 * i)) & 0xff);
    *len += 9;
}

static void bytes_append(uint8_t **buf, size_t *cap, size_t *len, const uint8_t *src, size_t n) {
    if (*len + n > *cap) {
        size_t nc = (*cap ? *cap * 2 : 64);
        while (nc < *len + n) nc *= 2;
        uint8_t *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
}

/* ---- core message handler --------------------------------------------- */

static void send_set_difficulty(char **buf, size_t *len, double diff) {
    cJSON *p = cJSON_CreateArray();
    cJSON_AddItemToArray(p, cJSON_CreateNumber(diff));
    emit_notification(buf, len, "mining.set_difficulty", p);
}

/* Network difficulty of the current job, or 0 when no job is set. */
static double current_net_diff(stratum_server_t *s) {
    double d = 0.0;
    pthread_rwlock_rdlock(&s->job_lock);
    if (s->current_job) d = target_to_diff(s->current_job->network_target_be);
    pthread_rwlock_unlock(&s->job_lock);
    return d;
}

/* How often a connection that is over its submit ceiling says so, rather than
 * once per refused share. */
#define RL_REPORT_INTERVAL_MS 10000

/* Has this connection used up its submits for the current second?
 *
 * Called before anything expensive, so a flood costs a JSON parse and a reply
 * instead of a coinbase render and four SHA256 passes. Returns non-zero when
 * the submit must be refused, and counts it for the periodic report. */
static int submit_rate_exceeded(stratum_server_t *s, stratum_conn_t *c,
                                uint64_t now_mono) {
    int limit = s->cfg.max_submits_per_sec;
    if (limit <= 0) return 0;
    if (now_mono - c->rl_window_start_ms >= 1000) {
        c->rl_window_start_ms = now_mono;
        c->rl_window_count = 0;
    }
    if (c->rl_window_count < (uint32_t)limit) {
        c->rl_window_count++;
        return 0;
    }
    c->rl_limited++;
    c->rl_limited_total++;
    return 1;
}

/* Say once per RL_REPORT_INTERVAL_MS that this connection is over its ceiling,
 * carrying the count of everything refused since the last time. One line and
 * one reject row per interval, whatever the rate -- the alternative writes the
 * flood into the database that exists to account for shares.
 *
 * The message names the difficulty, because that is the actual fault: a
 * connection only reaches this rate when what it was assigned is far below
 * what its hashrate warrants, and vardiff is still climbing towards it. */
static void submit_rate_report(stratum_server_t *s, stratum_conn_t *c,
                               uint64_t now_mono) {
    if (c->rl_limited == 0) return;
    if (c->rl_reported_ms != 0 &&
        now_mono - c->rl_reported_ms < RL_REPORT_INTERVAL_MS) return;

    LOG_WARN("stratum: %s is over the submit ceiling of %d/s — %u submit(s) "
             "refused since the last report, %llu on this connection. At "
             "difficulty %g its hashrate is producing more shares than the "
             "pool will take; vardiff is raising it",
             c->worker_name, s->cfg.max_submits_per_sec, c->rl_limited,
             (unsigned long long)c->rl_limited_total, c->difficulty);
    if (s->cfg.on_reject) {
        char msg[192];
        snprintf(msg, sizeof msg,
                 "submitting too fast: %u refused at over %d/s (%llu total)",
                 c->rl_limited, s->cfg.max_submits_per_sec,
                 (unsigned long long)c->rl_limited_total);
        /* Wall clock here, not the monotonic value the interval is measured
         * with: this one is a timestamp that gets stored and read back. */
        s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(), msg);
    }
    c->rl_limited = 0;
    c->rl_reported_ms = now_mono;
}

/* Record the difficulty `job_id` went out to this connection under. Called
 * from the notify path, which is the moment the agreement is struck: the
 * miner has already applied every set_difficulty we sent before this notify,
 * and will not apply the next one until the notify after it.
 *
 * An id already in the ring is updated in place rather than duplicated. That
 * only happens when the same job is notified twice — a client that authorizes
 * again — and there the freshest value is the right one, because the miner
 * restarts on the difficulty it holds now. */
static void conn_record_job_difficulty(stratum_conn_t *c, const char *job_id,
                                       double difficulty) {
    if (!c || !job_id || !job_id[0] || difficulty <= 0.0) return;
    pthread_mutex_lock(&c->jobdiff_lock);
    for (size_t i = 0; i < JOB_DIFF_RING; ++i) {
        if (strcmp(c->job_diffs[i].job_id, job_id) == 0) {
            c->job_diffs[i].difficulty = difficulty;
            pthread_mutex_unlock(&c->jobdiff_lock);
            return;
        }
    }
    size_t slot = c->job_diffs_head;
    snprintf(c->job_diffs[slot].job_id, sizeof c->job_diffs[slot].job_id,
             "%s", job_id);
    c->job_diffs[slot].difficulty = difficulty;
    c->job_diffs_head = (slot + 1) % JOB_DIFF_RING;
    pthread_mutex_unlock(&c->jobdiff_lock);
}

/* The difficulty `job_id` went out under, or 0.0 if this connection was never
 * told about that job — which means it cannot be mining it, so the caller
 * falls back to the connection's current difficulty. */
static double conn_job_difficulty(stratum_conn_t *c, const char *job_id) {
    double d = 0.0;
    if (!c || !job_id || !job_id[0]) return 0.0;
    pthread_mutex_lock(&c->jobdiff_lock);
    for (size_t i = 0; i < JOB_DIFF_RING; ++i) {
        if (strcmp(c->job_diffs[i].job_id, job_id) == 0) {
            d = c->job_diffs[i].difficulty;
            break;
        }
    }
    pthread_mutex_unlock(&c->jobdiff_lock);
    return d;
}

/* How many shares a window needs before its minimum achieved difficulty is
 * treated as evidence of the miner's own floor rather than small-sample
 * noise, how far above the assigned difficulty that floor has to sit before
 * we act, and how close under the observed floor we then aim. */
#define VD_FLOOR_MIN_SAMPLES 5
#define VD_FLOOR_TRIGGER     4.0
#define VD_FLOOR_BACKOFF     0.95

/* Vardiff: every cfg.vardiff_window_sec, look at how many shares the
 * connection submitted in that window and rescale its difficulty so the
 * rate converges on cfg.vardiff_target_spm shares/minute. Called from
 * handle_submit() after each accepted share.
 *
 * Conservative algorithm:
 *   ratio = observed_spm / target_spm
 *   if  ratio in [0.5, 2.0] → leave it (avoid jitter)
 *   else                    → new_diff = old_diff * ratio, clamped
 *
 * The rate loop alone cannot correct a miner that enforces its own local
 * difficulty floor above the difficulty we assigned it. Below that floor the
 * miner's submission rate does not depend on our difficulty at all — it
 * submits whatever beats its own target — so the loop has no gradient to
 * follow, and any share rate that happens to land inside the deadband is a
 * fixed point. A miner floored at 256 while assigned 1 therefore sits at 1
 * forever, and since a share is credited at the difficulty we assigned
 * (share_diff = c->difficulty in handle_submit), the pool books 1/256th of
 * the work it actually received: a hashrate estimate 256x low, and on a
 * pps-classic pool a payout 256x short.
 *
 * So alongside the rate loop, watch the difficulty the shares actually
 * achieve. Every share in the window achieving far more than we asked for is
 * a direct measurement of that floor, with none of the rate loop's
 * ambiguity, so raise the assigned difficulty to just under it.
 *
 * Aim just under the observed minimum — but only just, because the two ways
 * of missing the floor are not symmetric. Land above it and the accounting
 * stays exact: the miner keeps submitting everything that beats its own
 * floor, the pool accepts the fraction that also beats the assigned
 * difficulty, and crediting each of those at the assigned value books exactly
 * the work performed. Land below it and every share is accepted but credited
 * at less than it achieved, which is the under-crediting this whole check
 * exists to remove, just at a smaller factor. Overshooting costs the miner
 * rejected submissions; undershooting costs it money.
 *
 * So a small backoff, not a generous one. The observed minimum already sits
 * above the floor — a share clearing floor D achieves D/u for u uniform on
 * (0,1], so the smallest of n of them lands near D*(n+1)/n — which is what
 * leaves the result hovering around the floor instead of well under it.
 * Correcting that overshoot away would center the estimate and, by the
 * asymmetry above, credit worse.
 *
 * VD_FLOOR_TRIGGER is what stops this from ratcheting. Assigning above the
 * floor does raise the next window's observed minimum, but the next retarget
 * only fires if that minimum still clears four times the assigned difficulty,
 * which after a correct jump it does not. It is also what a well-matched
 * miner has to clear on every share in a window to trip this by chance, which
 * at five shares is about one window in thirty thousand — and costs it only a
 * difficulty briefly set too high, which the rate loop then walks back down
 * and which credits it correctly meanwhile.
 *
 * Note this deliberately does not count rejected submissions toward the
 * window. A stale or unknown-job reject is not work at the assigned
 * difficulty, and a low-difficulty reject is by definition work that missed
 * it; feeding either into the rate would push the difficulty up on miners
 * whose real problem is job latency.
 *
 * Always emits a single mining.set_difficulty when diff changes. The
 * client picks it up for the next job notify; we don't force a re-notify
 * because every job already carries the difficulty it went out under
 * (conn_record_job_difficulty), so shares for jobs the miner already holds
 * stay acceptable at that difficulty for as long as the job itself lives. */
static void vardiff_maybe_retarget(stratum_server_t *s, stratum_conn_t *c,
                                   uint64_t now,
                                   char **buf, size_t *len)
{
    if (!s->cfg.vardiff_enabled) return;
    if (c->vd_window_start_ms == 0) {
        c->vd_window_start_ms = now;
        c->vd_window_shares = 0;
        c->vd_window_min_achieved = HUGE_VAL;
        return;
    }
    uint64_t elapsed_ms = now - c->vd_window_start_ms;
    uint64_t window_ms  = (uint64_t)s->cfg.vardiff_window_sec * 1000ULL;
    if (elapsed_ms < window_ms) return;

    /* Observed shares per minute over this window. */
    double observed_spm = ((double)c->vd_window_shares * 60000.0) /
                          (double)elapsed_ms;
    double target_spm = s->cfg.vardiff_target_spm;
    double ratio = observed_spm / target_spm;

    double old_diff = c->difficulty;
    double new_diff = old_diff;
    if (ratio > 2.0 || ratio < 0.5) {
        new_diff = old_diff * ratio;
        /* Cap each adjustment to a 4x step to avoid wild swings on small
         * windows. */
        if (new_diff > old_diff * 4.0) new_diff = old_diff * 4.0;
        if (new_diff < old_diff / 4.0) new_diff = old_diff / 4.0;
    }

    /* Every share this window cleared a difficulty far above the one we
     * assigned: the miner is filtering locally at a floor of its own. Raise
     * to just under the floor we measured. Uncapped by the 4x step above —
     * that cap damps an extrapolation from a share rate, whereas this is a
     * value we watched every share in the window exceed.
     *
     * The comparison is against old_diff, the difficulty actually in force
     * while these shares were mined, not against the rate loop's proposal.
     * An accepted share always achieves at least the difficulty it was
     * accepted under, so the window minimum is always >= old_diff; testing
     * it against a proposal the rate loop has just cut by 4x would fire on
     * every such cut and pin the difficulty of every miner that legitimately
     * slowed down. */
    int from_floor = 0;
    if (c->vd_window_shares >= VD_FLOOR_MIN_SAMPLES &&
        isfinite(c->vd_window_min_achieved) &&
        c->vd_window_min_achieved > old_diff * VD_FLOOR_TRIGGER) {
        double floor_diff = c->vd_window_min_achieved * VD_FLOOR_BACKOFF;
        if (floor_diff > new_diff) {
            new_diff = floor_diff;
            from_floor = 1;
        }
    }

    if (new_diff != old_diff) {
        if (new_diff < c->pol_vardiff_min) new_diff = c->pol_vardiff_min;
        if (c->pol_vardiff_max > 0.0 && new_diff > c->pol_vardiff_max) {
            new_diff = c->pol_vardiff_max;
        }
        /* Never raise the share difficulty above the network difficulty:
         * the miner discards hashes above the stratum target locally, so a
         * share target harder than the network target throws away valid
         * blocks before the pool ever sees them. This clamp wins over
         * vardiff_min/max — it bites on low-difficulty networks where an
         * ASIC's vardiff otherwise climbs orders of magnitude past the
         * chain difficulty. */
        double net_diff = current_net_diff(s);
        if (net_diff > 0.0 && new_diff > net_diff) new_diff = net_diff;
    }

    double window_floor = c->vd_window_min_achieved;

    /* Reset the window regardless of whether we changed diff. */
    c->vd_window_start_ms = now;
    c->vd_window_shares = 0;
    c->vd_window_min_achieved = HUGE_VAL;

    if (new_diff != old_diff) {
        c->difficulty = new_diff;
        if (from_floor) {
            LOG_INFO("stratum: vardiff %s: %.0f -> %.0f (miner floor: every "
                     "share in the window cleared difficulty %.0f, %.1f spm "
                     "observed)",
                     c->worker_name, old_diff, new_diff, window_floor,
                     observed_spm);
        } else {
            LOG_INFO("stratum: vardiff %s: %.0f -> %.0f (%.1f spm observed, %.1f target)",
                     c->worker_name, old_diff, new_diff, observed_spm, target_spm);
        }
        send_set_difficulty(buf, len, new_diff);
    }
}

/* Send mining.notify for the current job to a specific connection, using
 * that connection's rendered coinbase. Skips silently if the conn is not
 * yet authorized (we have no payout address to render against). */
static void send_current_notify(stratum_server_t *s, stratum_conn_t *c,
                                char **buf, size_t *len, int clean) {
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *cur = s->current_job;
    if (cur && conn_render_coinbase(s, c, cur) == 0) {
        cJSON *p = make_notify_params(cur, c->cb1, c->cb1_len,
                                      c->cb2, c->cb2_len, clean);
        if (p) {
            emit_notification(buf, len, "mining.notify", p);
            /* Only once the notify is really going out: an unsent job is one
             * the miner cannot submit against, and recording it would put a
             * stale difficulty in the ring under a live id. */
            conn_record_job_difficulty(c, cur->job_id, c->difficulty);
        }
    }
    pthread_rwlock_unlock(&s->job_lock);
}

static int handle_subscribe(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            char **buf, size_t *len) {
    /* Take extranonce1 straight from the server counter, which is seeded
     * from the clock at startup and incremented once per subscribe.
     *
     * It must be unique across every live connection: identical extranonce1
     * means identical coinbases, so two connections mine the same header and
     * find the same hash from the same nonce — wasting half their hashrate
     * and double-crediting the share. XOR-ing the counter with the clock
     * again here (the previous approach) destroyed that guarantee: it
     * collides whenever the delta in the clock equals the delta in the
     * counter, e.g. an even seq at an even millisecond and the next seq one
     * millisecond later both yield the same value. A miner opening several
     * connections at once hits that case routinely. */
    unsigned seq = atomic_fetch_add(&s->extranonce1_seq, 1);
    uint32_t mix = (uint32_t)seq;
    c->extranonce1[0] = (uint8_t)(mix >> 24);
    c->extranonce1[1] = (uint8_t)(mix >> 16);
    c->extranonce1[2] = (uint8_t)(mix >> 8);
    c->extranonce1[3] = (uint8_t)mix;
    c->subscribed = 1;

    char ex1_hex[STRATUM_EXTRANONCE1_SIZE * 2 + 1];
    bytes_to_hex(c->extranonce1, sizeof c->extranonce1, ex1_hex);

    cJSON *result = cJSON_CreateArray();
    cJSON *subs = cJSON_CreateArray();
    cJSON *sd = cJSON_CreateArray();
    cJSON_AddItemToArray(sd, cJSON_CreateString("mining.set_difficulty"));
    cJSON_AddItemToArray(sd, cJSON_CreateString("sd"));
    cJSON_AddItemToArray(subs, sd);
    cJSON *sn = cJSON_CreateArray();
    cJSON_AddItemToArray(sn, cJSON_CreateString("mining.notify"));
    cJSON_AddItemToArray(sn, cJSON_CreateString("sn"));
    cJSON_AddItemToArray(subs, sn);
    cJSON_AddItemToArray(result, subs);
    cJSON_AddItemToArray(result, cJSON_CreateString(ex1_hex));
    /* The miner sizes its extranonce2 sweep off this number, and the coinbase
     * we render for it reserves exactly this many bytes. handle_submit
     * enforces the agreement. */
    cJSON_AddItemToArray(result, cJSON_CreateNumber(STRATUM_EXTRANONCE2_SIZE));

    return emit_response(buf, len, id, result, NULL);
}

/* mining.configure (BIP310). Only the version-rolling extension is supported.
 * params = [ [extension names...], { extension parameters... } ]. We negotiate
 * the version-rolling mask as (client mask AND our BIP320 mask) and report it
 * back; other requested extensions are silently left unacknowledged. */
static int handle_configure(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            cJSON *params, char **buf, size_t *len) {
    (void)s;
    cJSON *exts = NULL, *args = NULL;
    if (cJSON_IsArray(params)) {
        exts = cJSON_GetArrayItem(params, 0);
        args = cJSON_GetArrayItem(params, 1);
    }

    int wants_vr = 0;
    if (cJSON_IsArray(exts)) {
        int n = cJSON_GetArraySize(exts);
        for (int i = 0; i < n; ++i) {
            cJSON *e = cJSON_GetArrayItem(exts, i);
            if (cJSON_IsString(e) &&
                strcmp(e->valuestring, "version-rolling") == 0) {
                wants_vr = 1;
            }
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (wants_vr) {
        /* Client mask defaults to "roll everything" when omitted; we clamp it
         * to the bits we actually allow. */
        uint32_t client_mask = 0xffffffffu;
        if (cJSON_IsObject(args)) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(args,
                                                        "version-rolling.mask");
            uint32_t parsed;
            if (cJSON_IsString(m) && parse_u32_hex(m->valuestring, &parsed) == 0) {
                client_mask = parsed;
            }
        }
        c->version_mask = client_mask & VERSION_ROLLING_MASK;

        char mask_hex[9];
        snprintf(mask_hex, sizeof mask_hex, "%08x", c->version_mask);
        cJSON_AddItemToObject(result, "version-rolling", cJSON_CreateTrue());
        cJSON_AddStringToObject(result, "version-rolling.mask", mask_hex);
        LOG_INFO("stratum: version-rolling negotiated, mask=%s", mask_hex);
    }

    return emit_response(buf, len, id, result, NULL);
}

static int handle_authorize(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                            cJSON *params, char **buf, size_t *len) {
    const char *worker = NULL;
    if (cJSON_IsArray(params) && cJSON_GetArraySize(params) >= 1) {
        cJSON *w = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsString(w)) worker = w->valuestring;
    }
    if (!worker) {
        cJSON *err = make_error(24, "missing worker name");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Username format: <address>[.<rig_label>]. The address part must be
     * a valid bech32 (P2WPKH) or base58check (P2PKH / P2SH) Bitcoin
     * address; the optional label is a free-form rig identifier. */
    const char *dot = strchr(worker, '.');
    size_t addr_len = dot ? (size_t)(dot - worker) : strlen(worker);
    if (addr_len == 0 || addr_len >= sizeof(c->payout_address)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, worker, now_ms(),
                             "stratum username must start with a bitcoin address");
        }
        cJSON *err = make_error(24,
            "stratum username must be <bitcoin_address>[.<rig_label>]");
        return emit_response(buf, len, id, NULL, err);
    }
    /* Refuse before taking the address: the miner learns at connect time,
     * which is the only point at which they can still do something about it. */
    if (pps_gated(s)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, worker, now_ms(),
                             "pps accrual suspended (difficulty below floor)");
        }
        cJSON *err = make_error(24, PPS_GATED_MSG);
        return emit_response(buf, len, id, NULL, err);
    }

    memcpy(c->payout_address, worker, addr_len);
    c->payout_address[addr_len] = '\0';

    char    derr[128] = {0};
    if (s->cfg.pps_enabled) {
        /* Thunder address: 20-byte hash160 in plain base58. The
         * 's<n>_<base58>_<hex6>' deposit-format wrapper is rejected (see
         * thunder.c). We don't need the decoded bytes here — the coinbase
         * pays the pool's BTC wallet, not the miner — but we validate so a
         * typo'd username can't accrue unpayable PPS. */
        uint8_t th[20];
        if (thunder_address_decode(c->payout_address, th, derr, sizeof derr) < 0) {
            if (s->cfg.on_reject) {
                char rmsg[192];
                snprintf(rmsg, sizeof rmsg, "invalid thunder address: %s", derr);
                s->cfg.on_reject(s->cfg.ctx, worker, now_ms(), rmsg);
            }
            c->payout_address[0] = '\0';
            char emsg[192];
            snprintf(emsg, sizeof emsg,
                     "invalid thunder address in stratum username: %s", derr);
            cJSON *err = make_error(24, emsg);
            return emit_response(buf, len, id, NULL, err);
        }
    } else {
        uint8_t spk[64];
        size_t  spk_len = 0;
        if (coinbase_address_to_script(c->payout_address, spk, sizeof spk,
                                       &spk_len, derr, sizeof derr) < 0) {
            if (s->cfg.on_reject) {
                char rmsg[192];
                snprintf(rmsg, sizeof rmsg, "invalid payout address: %s", derr);
                s->cfg.on_reject(s->cfg.ctx, worker, now_ms(), rmsg);
            }
            c->payout_address[0] = '\0';
            char emsg[192];
            snprintf(emsg, sizeof emsg,
                     "invalid payout address in stratum username: %s", derr);
            cJSON *err = make_error(24, emsg);
            return emit_response(buf, len, id, NULL, err);
        }
    }

    sanitize_worker(worker, c->worker_name, sizeof(c->worker_name));
    c->authorized = 1;
    if (c->difficulty <= 0) c->difficulty = c->pol_initial_diff;
    /* Same clamp as vardiff: a starting difficulty above the network
     * difficulty would make the miner discard valid blocks locally. */
    double net_diff = current_net_diff(s);
    if (net_diff > 0.0 && c->difficulty > net_diff) c->difficulty = net_diff;
    /* Arm vardiff window for this connection. */
    c->vd_window_start_ms = now_ms();
    c->vd_window_shares = 0;
    c->vd_window_min_achieved = HUGE_VAL;

    /* respond true */
    emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
    /* Then push initial set_difficulty + notify (renders this conn's
     * coinbase against the current job using its payout address). */
    send_set_difficulty(buf, len, c->difficulty);
    send_current_notify(s, c, buf, len, 1);
    return 0;
}

static int dedupe_check_and_add(stratum_conn_t *c, const char *jid,
                                const char *en2, const char *ntime,
                                const char *nonce, uint32_t version) {
    char key[256];
    snprintf(key, sizeof(key), "%s|%s|%s|%s|%08x",
             jid ? jid : "", en2 ? en2 : "", ntime ? ntime : "", nonce ? nonce : "",
             version);
    uint64_t h = fnv1a(key);
    for (size_t i = 0; i < DEDUPE_RING; ++i) {
        if (c->dedupe[i] == h) return 1;
    }
    c->dedupe[c->dedupe_head] = h;
    c->dedupe_head = (c->dedupe_head + 1) % DEDUPE_RING;
    return 0;
}

/* Server-wide dedupe on the assembled header hash. Returns 1 if this exact
 * hash has already been credited on any connection, else records it and
 * returns 0. Called after the header is built, so it catches duplicates the
 * per-connection ring structurally cannot: a resubmission on a reconnected
 * or parallel connection, or the same work reframed under a different job
 * id. Two identical hashes represent one solution and must be paid once. */
static int share_dedupe_check_and_add(stratum_server_t *s,
                                      const uint8_t hash_be[32]) {
    uint64_t h = fnv1a_bytes(hash_be, 32);
    int dup = 0;
    pthread_mutex_lock(&s->share_dedupe_lock);
    for (size_t i = 0; i < SHARE_DEDUPE_RING; ++i) {
        if (s->share_dedupe[i] == h) { dup = 1; break; }
    }
    if (!dup) {
        s->share_dedupe[s->share_dedupe_head] = h;
        s->share_dedupe_head = (s->share_dedupe_head + 1) % SHARE_DEDUPE_RING;
    }
    pthread_mutex_unlock(&s->share_dedupe_lock);
    return dup;
}

/* Build full block hex from job + coinbase + nonce/ntime. Returns malloc'd
 * NUL-terminated string, or NULL on OOM. */
static char *assemble_block_hex(const stratum_job_t *j,
                                const uint8_t *coinbase_tx, size_t cb_len,
                                const uint8_t header[80]) {
    /* header(80) | varint(1+tx_count) | coinbase | concat(template_txs raw) */
    size_t tx_count = j->tx_count + 1; /* +1 coinbase */
    uint8_t *block = NULL;
    size_t cap = 0, len = 0;
    bytes_append(&block, &cap, &len, header, 80);
    varint_append(&block, &cap, &len, tx_count);
    if (j->coinbase_has_witness && cb_len >= 8) {
        /* coinbase_tx is the legacy serialization:
         *   version(4) | inputs | outputs | locktime(4)
         * The block's coinbase must carry its witness so the segwit
         * commitment validates. Re-serialize in segwit form: insert the
         * marker+flag after the version and the single-input witness (one
         * 32-byte reserved value, all zero — matching the commitment the
         * backend computed) just before the locktime. */
        static const uint8_t marker_flag[2] = { 0x00, 0x01 };
        static const uint8_t witness[34]    = { 0x01, 0x20 }; /* 1 item, 32 bytes, all zero */
        bytes_append(&block, &cap, &len, coinbase_tx, 4);                 /* version */
        bytes_append(&block, &cap, &len, marker_flag, 2);
        bytes_append(&block, &cap, &len, coinbase_tx + 4, cb_len - 8);    /* inputs + outputs */
        bytes_append(&block, &cap, &len, witness, sizeof witness);
        bytes_append(&block, &cap, &len, coinbase_tx + cb_len - 4, 4);    /* locktime */
    } else {
        bytes_append(&block, &cap, &len, coinbase_tx, cb_len);
    }
    for (size_t i = 0; i < j->tx_count; ++i) {
        size_t txn = 0;
        uint8_t *txb = hex_to_bytes_alloc(j->tx_hex_list[i], &txn);
        if (!txb) { free(block); return NULL; }
        bytes_append(&block, &cap, &len, txb, txn);
        free(txb);
    }
    char *out = malloc(len * 2 + 1);
    if (!out) { free(block); return NULL; }
    bytes_to_hex(block, len, out);
    free(block);
    return out;
}

/* The body of mining.submit, with `job` guaranteed live for the duration.
 *
 * Split from handle_submit so the reference find_job() hands back is released
 * on exactly one path. This function has nine exits, and a release on each is
 * a leak — or a double free — waiting for the next edit. */
static int submit_with_job(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                           cJSON *params, stratum_job_t *job,
                           const char *en2, const char *ntime,
                           const char *nonce, char **buf, size_t *len) {
    /* Version rolling (BIP310): the optional 6th submit param carries the
     * version the miner actually hashed. Keep the job's version bits outside
     * the negotiated mask and take the miner's bits inside it; with no param
     * (or no negotiation) this leaves job->version unchanged. We fall back to
     * the standard BIP320 mask if a version arrives without prior configure,
     * so miners that roll by default still verify correctly. */
    int32_t submit_version = job->version;
    if (cJSON_GetArraySize(params) >= 6) {
        cJSON *v = cJSON_GetArrayItem(params, 5);
        uint32_t rolled = 0;
        if (!cJSON_IsString(v) || parse_u32_hex(v->valuestring, &rolled) != 0) {
            cJSON *err = make_error(20, "bad version hex");
            return emit_response(buf, len, id, NULL, err);
        }
        uint32_t mask = c->version_mask ? c->version_mask : VERSION_ROLLING_MASK;
        submit_version =
            (int32_t)(((uint32_t)job->version & ~mask) | (rolled & mask));
    }

    /* job->job_id rather than the submitted string: find_job matched them
     * exactly, and the job is the one thing here guaranteed to outlive the
     * call. */
    if (dedupe_check_and_add(c, job->job_id, en2, ntime, nonce,
                             (uint32_t)submit_version)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "duplicate share");
        }
        cJSON *err = make_error(22, "duplicate share");
        return emit_response(buf, len, id, NULL, err);
    }

    uint32_t ntime_v, nonce_v;
    if (parse_u32_hex(ntime, &ntime_v) != 0 || parse_u32_hex(nonce, &nonce_v) != 0) {
        cJSON *err = make_error(20, "bad ntime/nonce hex");
        return emit_response(buf, len, id, NULL, err);
    }

    size_t en2_len = 0;
    uint8_t *en2_bytes = hex_to_bytes_alloc(en2, &en2_len);
    if (!en2_bytes) {
        cJSON *err = make_error(20, "bad extranonce2 hex");
        return emit_response(buf, len, id, NULL, err);
    }

    /* The extranonce2 must be exactly the width we advertised on subscribe
     * and reserved when rendering this job's cb1. cb1 ends with the scriptSig
     * length varint, which was computed as en1_size + en2_size; splicing in a
     * different width produces a coinbase whose declared scriptSig length
     * disagrees with the bytes that follow it. That transaction is invalid,
     * so any block built on it is rejected by the network -- but its header
     * still hashes, so without this check the share would look fine and be
     * credited. Reject instead of silently mining garbage: a miner that
     * ignored the advertised size needs to hear about it. */
    if (en2_len != job->en2_size) {
        free(en2_bytes);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "wrong extranonce2 size");
        }
        LOG_WARN("submit from %s: extranonce2 is %zu bytes, expected %zu "
                 "(miner ignored the size from mining.subscribe)",
                 c->worker_name, en2_len, job->en2_size);
        cJSON *err = make_error(20, "wrong extranonce2 size");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Render this connection's coinbase for `job` if not cached. The
     * cache is keyed on job_id; submits against an older job retired into
     * the recent ring will rebuild on demand. */
    if (conn_render_coinbase(s, c, job) < 0) {
        free(en2_bytes);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "coinbase render failed");
        }
        cJSON *err = make_error(25, "coinbase render failed");
        return emit_response(buf, len, id, NULL, err);
    }

    /* coinbase = cb1 || ex1 || ex2 || cb2 */
    size_t en1_len = sizeof c->extranonce1;
    size_t cb_len = c->cb1_len + en1_len + en2_len + c->cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) { free(en2_bytes); return -1; }
    size_t off = 0;
    memcpy(cb + off, c->cb1, c->cb1_len);        off += c->cb1_len;
    memcpy(cb + off, c->extranonce1, en1_len);   off += en1_len;
    memcpy(cb + off, en2_bytes, en2_len);        off += en2_len;
    memcpy(cb + off, c->cb2, c->cb2_len);   off += c->cb2_len;
    free(en2_bytes);

    uint8_t cb_txid_le[32];
    dsha256(cb, cb_len, cb_txid_le);

    uint8_t merkle_root_le[32];
    merkle_root_from_branches(cb_txid_le,
                              (const uint8_t (*)[32])job->merkle_branches,
                              job->branch_count, merkle_root_le);

    uint8_t header[80];
    build_header(submit_version, job->prev_hash_le, merkle_root_le,
                 ntime_v, job->nbits, nonce_v, header);

    uint8_t hash_be[32];
    hash_header(header, hash_be);

    /* Reject before any crediting: this hash is one solution regardless of
     * which connection produced it or how the submission was framed. */
    if (share_dedupe_check_and_add(s, hash_be)) {
        free(cb);
        LOG_INFO("stratum: reject from worker '%s' - Reason: duplicate share "
                 "(hash already credited)", c->worker_name);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "duplicate share");
        }
        cJSON *err = make_error(22, "duplicate share");
        return emit_response(buf, len, id, NULL, err);
    }

    /* Judge the share at the difficulty THIS job went out under, not at
     * whatever the connection has drifted to since. A miner applies a
     * set_difficulty on the next job it is notified, so every share for a job
     * already in its hands was mined against the difficulty in force when
     * that job was sent -- and on a slow chain those keep arriving long after
     * the retarget. Reading the difficulty back off the job is what makes
     * that exact, and holds across any number of retargets in between.
     *
     * A connection that was never notified of the job cannot be mining it;
     * fall back to its current difficulty rather than invent one. */
    double assigned_diff = conn_job_difficulty(c, job->job_id);
    if (assigned_diff <= 0.0) assigned_diff = c->difficulty;

    uint8_t worker_target[32];
    worker_diff_to_target(assigned_diff, worker_target);

    char sent_hash_hex[65] = {0};
    char worker_target_hex[65] = {0};
    char network_target_hex[65] = {0};

    // Convert the 32-byte big-endian fields into readable strings
    bytes_to_hex(hash_be, 32, sent_hash_hex);
    bytes_to_hex(worker_target, 32, worker_target_hex);
    bytes_to_hex(job->network_target_be, 32, network_target_hex);

    LOG_INFO("stratum: [SUBMIT CHECK] Worker: %s\n"
             "  -> Sent Hash:     %s\n"
             "  -> Worker Target: %s\n"
             "  -> Network Tgt:   %s\n"
             "  -> Version:       job=%08x rolled=%08x mask=%08x",
             c->worker_name, sent_hash_hex, worker_target_hex, network_target_hex,
             (uint32_t)job->version, (uint32_t)submit_version, c->version_mask);

    uint64_t ts_now   = now_ms();
    int is_block      = be32_cmp(hash_be, job->network_target_be) <= 0;
    int meets_worker  = be32_cmp(hash_be, worker_target) < 0;
    double share_diff = assigned_diff;

    /* Spec says a set_difficulty takes effect on the next job, but plenty of
     * firmware applies it to work already in hand. When the retarget went
     * *down* that produces shares below what this job was sent at, which are
     * honest work performed exactly as instructed. Take them at the lower
     * value -- a share is only ever credited at a difficulty it actually met.
     * A retarget that went up needs no such allowance: those shares clear the
     * job's difficulty on their own. */
    if (!meets_worker && c->difficulty < assigned_diff) {
        uint8_t current_target[32];
        worker_diff_to_target(c->difficulty, current_target);
        if (be32_cmp(hash_be, current_target) < 0) {
            meets_worker = 1;
            share_diff = c->difficulty;
        }
    }

    /* The network-target verdict must win over the share-difficulty reject:
     * when the share target is harder than the network target (low-difficulty
     * networks), a hash can be a valid block while failing the share check —
     * it has to be submitted, never rejected. */
    if (!is_block && !meets_worker) {
	LOG_INFO("stratum: reject from worker '%s' - Reason: low difficulty (Sent Hash > Worker Target)", c->worker_name);
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "low difficulty");
        }
        free(cb);
        cJSON *err = make_error(23, "low difficulty");
        return emit_response(buf, len, id, NULL, err);
    }

    char block_hash_hex[65] = {0};
    /* Whether the node took the candidate. Only meaningful when is_block.
     * Defaults to accepted so a server with no on_block hook (tests) behaves
     * as before; every real path assigns it from the submission. */
    int  block_accepted = 1;
    char submit_err[REASON_TEXT_MAX] = {0};
    if (is_block) {
        bytes_to_hex(hash_be, 32, block_hash_hex);
        if (!meets_worker) {
            /* Credit the share at the difficulty it provably met. */
            share_diff = target_to_diff(job->network_target_be);
            LOG_INFO("stratum: hash from '%s' beats the network target but not "
                     "the share target — submitting block", c->worker_name);
        }
        char *block_hex = assemble_block_hex(job, cb, cb_len, header);
        if (block_hex) {
            if (s->cfg.on_block) {
                int rc = s->cfg.on_block(s->cfg.ctx, block_hex,
                                         submit_err, sizeof submit_err);
                block_accepted = (rc == 0);
            }
            free(block_hex);
        } else {
            /* Nothing was submitted, so nothing can have been accepted.
             * Falling through as "found" here would file a block the node
             * was never even shown. */
            block_accepted = 0;
            snprintf(submit_err, sizeof submit_err, "block assembly failed");
        }
        if (!block_accepted) {
            LOG_WARN("stratum: candidate from '%s' at height %u was not "
                     "accepted: %s", c->worker_name, job->height, submit_err);
        }
    }
    free(cb);

    if (s->cfg.on_share) {
        /* Always pass the actual share hash so the dashboard can show the
         * hash of every share (and the user can eyeball its leading zeros
         * to gauge how lucky each share was). When is_block, this string
         * also IS the block hash; otherwise it's a 'just-a-share' hash. */
        s->cfg.on_share(s->cfg.ctx, c->worker_name, c->payout_address,
                        ts_now, share_diff, is_block, sent_hash_hex);
    }
    /* Tick vardiff: count this accepted share toward the window, and track
     * the difficulty it actually achieved. Reading the hash as a target
     * gives exactly that: how much harder than difficulty 1 this solution
     * was. The running minimum is what exposes a miner filtering at a local
     * floor above its assigned difficulty. */
    c->vd_window_shares++;
    double achieved = target_to_diff(hash_be);
    if (achieved < c->vd_window_min_achieved) {
        c->vd_window_min_achieved = achieved;
    }
    vardiff_maybe_retarget(s, c, now_ms(), buf, len);
    if (is_block && s->cfg.on_block_found) {
        int64_t fee_sats = 0;
        if (s->cfg.fee_bps > 0 && s->cfg.operator_address[0]) {
            fee_sats = (job->value_sats * (int64_t)s->cfg.fee_bps) / 10000;
            if (fee_sats < 546) fee_sats = 0; /* matches coinbase dust rule */
        }
        int64_t reward_sats = job->value_sats - fee_sats;
        s->cfg.on_block_found(s->cfg.ctx, c->worker_name,
                              c->payout_address, ts_now, job->height,
                              block_hash_hex, reward_sats, fee_sats,
                              block_accepted, submit_err);
    }
    return emit_response(buf, len, id, cJSON_CreateTrue(), NULL);
}

static int handle_submit(stratum_server_t *s, stratum_conn_t *c, cJSON *id,
                         cJSON *params, char **buf, size_t *len) {
    if (!c->authorized) {
        cJSON *err = make_error(24, "unauthorized");
        return emit_response(buf, len, id, NULL, err);
    }
    /* Before the params are even looked at: past the ceiling this share is
     * not going to be validated, so nothing beyond the reply should be spent
     * on it. */
    uint64_t rl_now = mono_ms();
    if (submit_rate_exceeded(s, c, rl_now)) {
        submit_rate_report(s, c, rl_now);
        cJSON *err = make_error(20, "submitting too fast");
        return emit_response(buf, len, id, NULL, err);
    }
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 5) {
        cJSON *err = make_error(20, "bad params");
        return emit_response(buf, len, id, NULL, err);
    }
    const char *worker = cJSON_GetArrayItem(params, 0)->valuestring;
    const char *jid    = cJSON_GetArrayItem(params, 1)->valuestring;
    const char *en2    = cJSON_GetArrayItem(params, 2)->valuestring;
    const char *ntime  = cJSON_GetArrayItem(params, 3)->valuestring;
    const char *nonce  = cJSON_GetArrayItem(params, 4)->valuestring;
    (void)worker;

    /* A miner that authorized before the gate closed is still connected and
     * still hashing. Accepting those shares would bank work the pool has
     * already decided not to pay for. */
    if (pps_gated(s)) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "pps accrual suspended (difficulty below floor)");
        }
        cJSON *err = make_error(24, PPS_GATED_MSG);
        return emit_response(buf, len, id, NULL, err);
    }

    /* Counted reference: the tip watcher may retire and free this job while
     * the submit below is still reading it. */
    stratum_job_t *job = find_job(s, jid);
    if (!job) {
        if (s->cfg.on_reject) {
            s->cfg.on_reject(s->cfg.ctx, c->worker_name, now_ms(),
                             "stale or unknown job");
        }
        cJSON *err = make_error(21, "stale or unknown job");
        return emit_response(buf, len, id, NULL, err);
    }

    int rc = submit_with_job(s, c, id, params, job, en2, ntime, nonce, buf, len);
    stratum_job_free(job);
    return rc;
}

int stratum_handle_message(stratum_server_t *s, stratum_conn_t *c,
                           const char *line, char **out_buf, size_t *out_len)
{
    if (!line) return -1;
    if (strlen(line) > MAX_LINE_BYTES) return -1;
    cJSON *root = cJSON_Parse(line);
    if (!root) return -1;
    cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return -1;
    }
    int rc = 0;
    if (strcmp(method->valuestring, "mining.configure") == 0) {
        rc = handle_configure(s, c, id, params, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.subscribe") == 0) {
        rc = handle_subscribe(s, c, id, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.authorize") == 0) {
        rc = handle_authorize(s, c, id, params, out_buf, out_len);
    } else if (strcmp(method->valuestring, "mining.submit") == 0) {
        rc = handle_submit(s, c, id, params, out_buf, out_len);
    } else {
        cJSON *err = make_error(20, "unknown method");
        rc = emit_response(out_buf, out_len, id, NULL, err);
    }
    cJSON_Delete(root);
    return rc;
}

/* ---- conn lifecycle (test helpers + thread) --------------------------- */

/* Take the listener's difficulty policy onto the connection. A field the
 * listener left at 0 keeps the server-wide default already on the conn --
 * that is what makes a listener able to override only min_diff, say, without
 * having to restate everything else. */
static void conn_apply_listener(stratum_conn_t *c,
                                const stratum_listener_t *pol) {
    if (!c || !pol) return;
    if (pol->initial_diff > 0.0) c->pol_initial_diff = pol->initial_diff;
    if (pol->vardiff_min  > 0.0) c->pol_vardiff_min  = pol->vardiff_min;
    if (pol->vardiff_max  > 0.0) c->pol_vardiff_max  = pol->vardiff_max;
    c->pol_port = pol->port;
    snprintf(c->pol_label, sizeof c->pol_label, "%s", pol->label);
    /* Before authorize the connection has no assigned difficulty yet, so
     * seeding it here keeps a subscribe-only conn reporting its port's value
     * rather than the default it was born with. */
    c->difficulty = c->pol_initial_diff;
}

void stratum_conn_apply_listener_for_test(stratum_conn_t *c,
                                          const stratum_listener_t *pol) {
    conn_apply_listener(c, pol);
}

stratum_conn_t *stratum_conn_new_for_test(stratum_server_t *s) {
    stratum_conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->server = s;
    c->fd = -1;
    c->pol_initial_diff = s ? s->cfg.initial_diff : 1.0;
    c->pol_vardiff_min  = s ? s->cfg.vardiff_min  : 1.0;
    c->pol_vardiff_max  = s ? s->cfg.vardiff_max  : 0.0;
    c->pol_port         = s ? s->cfg.bind_port    : 0;
    c->difficulty = c->pol_initial_diff;
    c->vd_window_min_achieved = HUGE_VAL;
    pthread_mutex_init(&c->write_lock, NULL);
    pthread_mutex_init(&c->jobdiff_lock, NULL);
    return c;
}

void stratum_conn_free_for_test(stratum_conn_t *c) {
    if (!c) return;
    /* A flood usually ends by going quiet, and the refusals since the last
     * periodic report would otherwise die with the connection -- so the
     * operator would see "1 refused" for a burst of nine hundred. Force the
     * final report out before the counters go. */
    if (c->server && c->rl_limited > 0) {
        c->rl_reported_ms = 0;   /* bypass the interval; this is the last one */
        submit_rate_report(c->server, c, mono_ms());
    }
    conn_clear_coinbase(c);
    pthread_mutex_destroy(&c->write_lock);
    pthread_mutex_destroy(&c->jobdiff_lock);
    free(c);
}

stratum_job_t *stratum_job_find_for_test(stratum_server_t *s, const char *job_id) {
    return find_job(s, job_id);
}
uint32_t stratum_job_height_for_test(const stratum_job_t *j) {
    return j ? j->height : 0;
}
int64_t stratum_job_value_sats_for_test(const stratum_job_t *j) {
    return j ? j->value_sats : 0;
}

/* Render (or reuse) this connection's coinbase for the current job and hand
 * back the pieces a submit is hashed from, plus the connection's assigned
 * difficulty. Tests use it to compute the hash a given nonce would produce,
 * which is the only way to pick shares achieving a chosen difficulty instead
 * of whatever the first nonce happens to land on — and simulating a miner
 * that filters at its own difficulty floor needs exactly that. */
int stratum_conn_coinbase_for_test(stratum_server_t *s, stratum_conn_t *c,
                                   const char *job_id,
                                   const uint8_t **cb1, size_t *cb1_len,
                                   const uint8_t **cb2, size_t *cb2_len,
                                   const uint8_t **en1) {
    if (!s || !c || !job_id) return -1;
    int rc = -1;
    pthread_rwlock_rdlock(&s->job_lock);
    stratum_job_t *j = s->current_job;
    if (j && strcmp(j->job_id, job_id) == 0 &&
        conn_render_coinbase(s, c, j) == 0) {
        *cb1 = c->cb1; *cb1_len = c->cb1_len;
        *cb2 = c->cb2; *cb2_len = c->cb2_len;
        *en1 = c->extranonce1;
        rc = 0;
    }
    pthread_rwlock_unlock(&s->job_lock);
    return rc;
}

double stratum_conn_difficulty_for_test(const stratum_conn_t *c) {
    return c ? c->difficulty : 0.0;
}

const char *stratum_conn_worker_name_for_test(const stratum_conn_t *c) {
    return c ? c->worker_name : NULL;
}
const char *stratum_conn_payout_address_for_test(const stratum_conn_t *c) {
    return c ? c->payout_address : NULL;
}
int stratum_conn_authorized_for_test(const stratum_conn_t *c) {
    return c ? c->authorized : 0;
}
int stratum_conn_subscribed_for_test(const stratum_conn_t *c) {
    return c ? c->subscribed : 0;
}

/* ---- real connection thread ------------------------------------------ */

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    return 0;
}

/* Configure an accepted socket so idle miners get reaped instead of
 * clogging fds/threads indefinitely. Two mechanisms, belt-and-suspenders:
 *
 *   1. Application-level: SO_RCVTIMEO gives recv() a bounded wake so we
 *      can compare last_activity_ms against cfg.idle_timeout_sec. Catches
 *      miners that hold the TCP open but never send anything (e.g. bad
 *      username, misconfigured worker).
 *   2. OS-level: SO_KEEPALIVE + tightened TCP_KEEPIDLE/INTVL/CNT so Linux
 *      drops the socket after ~5 min of unacked probes. Catches half-open
 *      TCPs where the miner box vanished from the network without FIN.
 *
 * idle_timeout_sec <= 0 disables the read-timeout path (legacy blocking
 * recv). Returns 0 on success, -1 on fatal setsockopt failure. */
static int conn_socket_setup(int fd, int idle_timeout_sec) {
    int one = 1;
    /* TCP_NODELAY: stratum is tiny latency-sensitive JSON, don't Nagle. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Kernel keepalive. Default kernel setting is ~2h before probes even
     * start, useless for our purposes — override with tight values. */
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 120;   /* start probing after 2 min of inactivity */
    int intvl = 30;   /* probe every 30s */
    int cnt = 3;      /* drop after 3 unacked probes ≈ 3.5 min total */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#endif

    if (idle_timeout_sec > 0) {
        /* Poll interval: min(idle_timeout, 30s). Longer wastes the tail
         * of the timeout; shorter costs one recv wake per fd per interval
         * (500 conns × wake/30s = ~16/s, negligible). */
        int poll_s = idle_timeout_sec < 30 ? idle_timeout_sec : 30;
        struct timeval tv = { .tv_sec = poll_s, .tv_usec = 0 };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            return -1;
        }
    }
    return 0;
}

int stratum_socket_setup_for_test(int fd, int idle_timeout_sec) {
    return conn_socket_setup(fd, idle_timeout_sec);
}

static void conn_register(stratum_server_t *s, stratum_conn_t *c) {
    pthread_mutex_lock(&s->conns_lock);
    c->next = s->conns_head;
    s->conns_head = c;
    pthread_mutex_unlock(&s->conns_lock);
}

static void conn_unregister(stratum_server_t *s, stratum_conn_t *c) {
    pthread_mutex_lock(&s->conns_lock);
    struct stratum_conn **p = &s->conns_head;
    while (*p) {
        if (*p == c) { *p = c->next; break; }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&s->conns_lock);
}

static void *conn_thread(void *arg) {
    stratum_conn_t *c = arg;
    stratum_server_t *s = c->server;
    char buf[MAX_LINE_BYTES + 1];
    size_t blen = 0;

    /* Seed activity tracking at connect time — a client that never sends
     * a single byte is still governed by cfg.idle_timeout_sec. */
    c->last_activity_ms = mono_ms();
    const uint64_t idle_timeout_ms =
        s->cfg.idle_timeout_sec > 0
            ? (uint64_t)s->cfg.idle_timeout_sec * 1000u
            : 0;

    while (!atomic_load(&s->stop)) {
        ssize_t n = recv(c->fd, buf + blen, sizeof(buf) - 1 - blen, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* SO_RCVTIMEO wake. Drop iff we've been silent past the
                 * configured budget. Otherwise loop and try again. */
                if (idle_timeout_ms > 0 &&
                    mono_ms() - c->last_activity_ms > idle_timeout_ms) {
                    LOG_INFO("stratum: idle timeout after %us — closing fd=%d worker='%s'",
                             s->cfg.idle_timeout_sec, c->fd,
                             c->worker_name[0] ? c->worker_name : "(unauthorized)");
                    goto done;
                }
                continue;
            }
            break;  /* real socket error */
        }
        if (n == 0) break;  /* peer closed */
        c->last_activity_ms = mono_ms();
        blen += (size_t)n;
        buf[blen] = '\0';
        for (;;) {
            char *nl = memchr(buf, '\n', blen);
            if (!nl) {
                if (blen >= MAX_LINE_BYTES) { goto done; } /* oversize */
                break;
            }
            *nl = '\0';
            char *line = buf;
            char *out = NULL; size_t olen = 0;
            int rc = stratum_handle_message(s, c, line, &out, &olen);
            if (out && olen) {
                pthread_mutex_lock(&c->write_lock);
                write_all(c->fd, out, olen);
                pthread_mutex_unlock(&c->write_lock);
            }
            free(out);
            if (rc < 0) goto done;
            size_t consumed = (size_t)(nl - buf) + 1;
            memmove(buf, buf + consumed, blen - consumed);
            blen -= consumed;
        }
    }
done:
    close(c->fd);
    conn_unregister(s, c);
    atomic_fetch_sub(&s->conn_count, 1);
    stratum_conn_free_for_test(c);
    return NULL;
}

static void *listener_thread(void *arg) {
    struct stratum_listener_slot *ls = arg;
    stratum_server_t *s = ls->srv;
    while (!atomic_load(&s->stop)) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int fd = accept(ls->fd, (struct sockaddr *)&cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (atomic_load(&s->stop)) break;
            LOG_WARN("stratum: accept: %s", strerror(errno));
            continue;
        }
        if (atomic_load(&s->conn_count) >= s->cfg.max_conns) {
            close(fd);
            continue;
        }
        if (conn_socket_setup(fd, s->cfg.idle_timeout_sec) < 0) {
            LOG_WARN("stratum: socket setup failed for accepted fd: %s",
                     strerror(errno));
            close(fd);
            continue;
        }
        stratum_conn_t *c = stratum_conn_new_for_test(s);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        /* The port decides the difficulty. Everything after this point reads
         * the policy off the connection and never looks at the listener. */
        conn_apply_listener(c, &ls->pol);
        atomic_fetch_add(&s->conn_count, 1);
        conn_register(s, c);
        if (pthread_create(&c->thr, NULL, conn_thread, c) != 0) {
            conn_unregister(s, c);
            atomic_fetch_sub(&s->conn_count, 1);
            close(fd);
            stratum_conn_free_for_test(c);
            continue;
        }
        pthread_detach(c->thr);
        c->thr_started = 1;
    }
    return NULL;
}

int stratum_server_start(const stratum_cfg_t *cfg, stratum_server_t **out) {
    if (!cfg || !out) return -1;
    stratum_server_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->cfg = *cfg;
    if (s->cfg.max_conns <= 0) s->cfg.max_conns = 500;
    if (s->cfg.initial_diff <= 0) s->cfg.initial_diff = 1.0;
    /* Negative → explicit disable. 0 → apply default (10 min). Positive kept. */
    if (s->cfg.idle_timeout_sec == 0) s->cfg.idle_timeout_sec = 600;
    else if (s->cfg.idle_timeout_sec < 0) s->cfg.idle_timeout_sec = 0;
    pthread_rwlock_init(&s->job_lock, NULL);
    pthread_mutex_init(&s->recent_lock, NULL);
    pthread_mutex_init(&s->conns_lock, NULL);
    pthread_mutex_init(&s->share_dedupe_lock, NULL);
    atomic_init(&s->stop, 0);
    atomic_init(&s->conn_count, 0);
    atomic_init(&s->extranonce1_seq, (unsigned)now_ms());

    /* Listener 0 is always bind_port on the server-wide defaults, so a config
     * naming no extra listeners binds exactly what it always did. The rest
     * come from cfg.listeners, each overriding the difficulty policy for the
     * connections it accepts. */
    s->listeners[0].srv = s;
    s->listeners[0].pol.port = cfg->bind_port;
    s->listener_count = 1;
    for (int i = 0; i < cfg->listener_count &&
                    s->listener_count < STRATUM_MAX_LISTENERS; ++i) {
        if (cfg->listeners[i].port <= 0) continue;
        s->listeners[s->listener_count].srv = s;
        s->listeners[s->listener_count].pol = cfg->listeners[i];
        s->listener_count++;
    }

    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        ls->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ls->fd < 0) goto bind_failed;
        int one = 1;
        setsockopt(ls->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)ls->pol.port);
        if (cfg->bind_addr[0] == '\0' || strcmp(cfg->bind_addr, "0.0.0.0") == 0) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
                goto bind_failed;
            }
        }
        if (bind(ls->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_ERROR("stratum bind %s:%d: %s", cfg->bind_addr, ls->pol.port,
                      strerror(errno));
            goto bind_failed;
        }
        if (listen(ls->fd, 64) < 0) goto bind_failed;
        if (pthread_create(&ls->thr, NULL, listener_thread, ls) != 0) {
            goto bind_failed;
        }
        ls->thr_started = 1;
    }
    *out = s;
    return 0;

    /* A pool that came up on some of its ports is worse than one that did not
     * come up: the operator sees a running process and a marketplace sees a
     * refused connection. Tear down whatever bound and fail the start. */
bind_failed:
    atomic_store(&s->stop, 1);
    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        if (ls->fd >= 0) { shutdown(ls->fd, SHUT_RDWR); close(ls->fd); ls->fd = -1; }
        if (ls->thr_started) { pthread_join(ls->thr, NULL); ls->thr_started = 0; }
    }
    free(s);
    return -1;
}

void stratum_server_set_job(stratum_server_t *s, stratum_job_t *new_job) {
    if (!s || !new_job) return;
    pthread_rwlock_wrlock(&s->job_lock);
    stratum_job_t *old = s->current_job;
    s->current_job = new_job;
    pthread_rwlock_unlock(&s->job_lock);
    if (old) retire_job(s, old);

    /* broadcast notify with clean_jobs=true. Each conn renders its own
     * coinbase against the new job (paying its miner address). */
    pthread_mutex_lock(&s->conns_lock);
    for (stratum_conn_t *c = s->conns_head; c; c = c->next) {
        if (!c->subscribed || c->fd < 0 || !c->authorized) continue;
        char *out = NULL; size_t olen = 0;
        send_current_notify(s, c, &out, &olen, 1);
        if (out) {
            pthread_mutex_lock(&c->write_lock);
            write_all(c->fd, out, olen);
            pthread_mutex_unlock(&c->write_lock);
            free(out);
        }
    }
    pthread_mutex_unlock(&s->conns_lock);
}

void stratum_server_stop(stratum_server_t *s) {
    if (!s) return;
    atomic_store(&s->stop, 1);
    for (int i = 0; i < s->listener_count; ++i) {
        struct stratum_listener_slot *ls = &s->listeners[i];
        if (ls->fd >= 0) {
            shutdown(ls->fd, SHUT_RDWR);
            close(ls->fd);
            ls->fd = -1;
        }
        if (ls->thr_started) {
            pthread_join(ls->thr, NULL);
            ls->thr_started = 0;
        }
    }
}

void stratum_server_free(stratum_server_t *s) {
    if (!s) return;
    stratum_server_stop(s);
    pthread_rwlock_wrlock(&s->job_lock);
    stratum_job_free(s->current_job);
    s->current_job = NULL;
    pthread_rwlock_unlock(&s->job_lock);
    for (size_t i = 0; i < RECENT_JOBS; ++i) stratum_job_free(s->recent[i]);
    pthread_rwlock_destroy(&s->job_lock);
    pthread_mutex_destroy(&s->recent_lock);
    pthread_mutex_destroy(&s->conns_lock);
    pthread_mutex_destroy(&s->share_dedupe_lock);
    free(s);
}
