#include "../src/stratum.h"
#include "../src/share.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* Observers + state. */
typedef struct {
    int   shares;
    int   rejects;
    int   blocks;
    int   last_is_block;
    double sum_share_diff;   /* difficulty the pool credited, summed */
    char  last_worker[64];
    char  last_reason[128];
    /* Block-candidate accounting. submit_rejects makes the stubbed
     * submitblock refuse, which is the common case on a low-difficulty
     * chain and the one that used to be recorded as a block anyway. */
    int   submit_rejects;
    int   submits;
    int   found_calls;
    int   last_accepted;
    char  last_submit_error[128];
} obs_t;

static void on_share(void *ctx, const char *w, const char *addr,
                     uint64_t ts, double d,
                     int is_block, const char *blk) {
    (void)ts; (void)blk; (void)addr;
    obs_t *o = ctx;
    o->shares++;
    o->sum_share_diff += d;
    o->last_is_block = is_block;
    if (is_block) o->blocks++;
    snprintf(o->last_worker, sizeof(o->last_worker), "%s", w ? w : "");
}
static void on_reject(void *ctx, const char *w, uint64_t ts, const char *r) {
    (void)ts; (void)w;
    obs_t *o = ctx;
    o->rejects++;
    snprintf(o->last_reason, sizeof(o->last_reason), "%s", r ? r : "");
}
static int on_block(void *ctx, const char *hex, char *errbuf, size_t errlen) {
    (void)hex;
    obs_t *o = ctx;
    if (!o) return 0;
    o->submits++;
    if (o->submit_rejects) {
        snprintf(errbuf, errlen, "inconclusive");
        return -30;
    }
    return 0;
}
static void on_block_found(void *ctx, const char *w, const char *addr,
                           uint64_t ts, uint32_t height, const char *hash,
                           int64_t reward, int64_t fee,
                           int accepted, const char *submit_error) {
    (void)w; (void)addr; (void)ts; (void)height; (void)hash;
    (void)reward; (void)fee;
    obs_t *o = ctx;
    o->found_calls++;
    o->last_accepted = accepted;
    snprintf(o->last_submit_error, sizeof(o->last_submit_error), "%s",
             submit_error ? submit_error : "");
}

/* Helper: parse the first line of an output buffer. Mutates buf (NUL terminator). */
static cJSON *parse_first_line(char *buf) {
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return cJSON_Parse(buf);
}

/* usleep is gone from POSIX.1-2008 (which the Makefile requests), so glibc
 * hides its declaration; nanosleep is the conforming replacement. */
static void sleep_ms(long ms) {
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Helper: count newline-delimited messages. */
static int count_lines(const char *buf, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len; ++i) if (buf[i] == '\n') n++;
    return n;
}

/* Standard regtest P2WPKH used in fixtures so the per-connection coinbase
 * renderer can produce a valid scriptPubKey. */
#define TEST_ADDR "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"

/* Build a tiny job for tests. The coinbase is rendered per-connection at
 * notify/submit time using the miner's address, so the job only carries
 * template-level data. */
static stratum_job_t *make_test_job(const char *job_id,
                                    const uint8_t *network_target_be) {
    uint8_t prev[32] = {0};
    return stratum_job_new(job_id, 1, prev,
                           /*value_sats*/ 5000000000LL,
                           /*wc_hex*/ NULL,
                           /*en1*/ 4, /*en2*/ 4,
                           NULL, 0, 0x1d00ffffu, 0x60000000u,
                           network_target_be, 800000, NULL, 0,
                           /*coinbasetxn_hex*/ NULL,
                           /*coinbase_has_witness*/ 0);
}

static void test_subscribe(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    /* Don't start the server — we exercise just the handler. */
    stratum_server_t *s = NULL;
    /* Hack: synthesize a server by calling start with port 0 -> kernel
     * picks one. */
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0); CHECK(s != NULL);
    if (!s) return;

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    rc = stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(out != NULL && olen > 0);
    cJSON *resp = parse_first_line(out);
    CHECK(resp != NULL);
    if (resp) {
        cJSON *result = cJSON_GetObjectItem(resp, "result");
        CHECK(cJSON_IsArray(result));
        CHECK(cJSON_GetArraySize(result) == 3);
        cJSON *subs = cJSON_GetArrayItem(result, 0);
        CHECK(cJSON_IsArray(subs) && cJSON_GetArraySize(subs) == 2);
        cJSON *ex1 = cJSON_GetArrayItem(result, 1);
        CHECK(cJSON_IsString(ex1) && strlen(ex1->valuestring) == 8);
        cJSON *ex2sz = cJSON_GetArrayItem(result, 2);
        CHECK(cJSON_IsNumber(ex2sz) && ex2sz->valueint == 4);
        cJSON_Delete(resp);
    }
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_authorize_triggers_setdiff_notify(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0);

    /* Provide a job so notify can be sent. */
    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("0001", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    /* Subscribe first to get extranonce1. */
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen);
    free(out); out = NULL; olen = 0;

    rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".w1\",\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    /* Expect 3 lines: response(true), set_difficulty, notify. */
    int n = count_lines(out, olen);
    CHECK(n == 3);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "mining.notify") != NULL);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".w1") == 0);
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_unknown_job(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    /* No job set. */
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"NOPE\",\"00000000\",\"60000000\",\"00000000\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "stale") != NULL);
    /* response should carry an error array */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_share_and_dedupe(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           /* tiny diff -> worker target = max -> any hash passes */
                           .initial_diff = 1e-12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* Network target = all zeros -> never a block. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.last_is_block == 0);
    CHECK(obs.blocks == 0);
    free(out); out=NULL; olen=0;

    /* Duplicate: same parameters again. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);  /* not incremented */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Invalid Bitcoin address as the username must be rejected outright. */
static void test_authorize_rejects_non_address(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_reject = on_reject };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"alice.w1\",\"x\"]}",
        &out, &olen);
    CHECK(!stratum_conn_authorized_for_test(c));
    CHECK(obs.rejects == 1);
    /* JSON-RPC response carries an error array. */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Valid address with a funky label: address is preserved verbatim,
 * worker_name contains the full username (sanitized chars allowed
 * already), payout_address is exactly the address portion. */
static void test_authorize_address_with_label(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".rig-007\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c));
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".rig-007") == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A hash that beats the network target but not the share target must be
 * accepted and submitted as a block, never rejected as low difficulty.
 * Regression: worker diff 1e12 makes the worker target ~0 so every hash
 * fails the share check, while an all-ff network target makes every hash
 * a block. The job is set after authorize so the authorize-time clamp
 * (no job yet) leaves the huge difficulty in place. */
static void test_block_wins_over_low_difficulty(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net));

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.rejects == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.blocks == 1);
    CHECK(obs.last_is_block == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* ---- vardiff vs. a miner enforcing its own difficulty floor ------------ */

/* Reproduce the hash a submit would produce, so a test can pick nonces that
 * achieve a chosen difficulty. make_test_job carries no merkle branches, so
 * the merkle root is just the coinbase txid. */
static double achieved_diff_for_nonce(stratum_server_t *s, stratum_conn_t *c,
                                      const char *job_id, const char *en2_hex,
                                      uint32_t nonce) {
    const uint8_t *cb1 = NULL, *cb2 = NULL, *en1 = NULL;
    size_t cb1_len = 0, cb2_len = 0;
    if (stratum_conn_coinbase_for_test(s, c, job_id, &cb1, &cb1_len,
                                       &cb2, &cb2_len, &en1) != 0) return 0.0;

    uint8_t en2[4];
    for (int i = 0; i < 4; ++i) {
        unsigned byte = 0;
        sscanf(en2_hex + 2 * i, "%2x", &byte);
        en2[i] = (uint8_t)byte;
    }

    size_t cb_len = cb1_len + 4 + 4 + cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) return 0.0;
    size_t off = 0;
    memcpy(cb + off, cb1, cb1_len); off += cb1_len;
    memcpy(cb + off, en1, 4);       off += 4;
    memcpy(cb + off, en2, 4);       off += 4;
    memcpy(cb + off, cb2, cb2_len);

    uint8_t cb_txid_le[32], root_le[32], header[80], hash_be[32];
    dsha256(cb, cb_len, cb_txid_le);
    free(cb);
    merkle_root_from_branches(cb_txid_le, NULL, 0, root_le);
    uint8_t prev[32] = {0};
    build_header(1, prev, root_le, 0x60000000u, 0x1d00ffffu, nonce, header);
    hash_header(header, hash_be);
    return target_to_diff(hash_be);
}

/* Find the next nonce at or above `from` whose share achieves at least `want`
 * and less than `want_max`. Returns the achieved difficulty and writes the
 * nonce, or 0.0 if none was found.
 *
 * The upper bound matters because extranonce1 is seeded from the clock, so
 * every run mines a different set of hashes. Without it, how far a share
 * overshoots its target is left to chance, and a test that depends on shares
 * NOT overshooting by some factor becomes a rare flake. */
static double mine_nonce(stratum_server_t *s, stratum_conn_t *c,
                         const char *job_id, const char *en2_hex,
                         double want, double want_max,
                         uint32_t from, uint32_t *out_nonce) {
    for (uint32_t n = from; n < from + 4000000u; ++n) {
        double d = achieved_diff_for_nonce(s, c, job_id, en2_hex, n);
        if (d >= want && d < want_max) { *out_nonce = n; return d; }
    }
    return 0.0;
}

static void submit_nonce(stratum_server_t *s, stratum_conn_t *c,
                         const char *en2_hex, uint32_t nonce,
                         char **out, size_t *olen) {
    char msg[256];
    snprintf(msg, sizeof msg,
             "{\"id\":9,\"method\":\"mining.submit\","
             "\"params\":[\"w\",\"J1\",\"%s\",\"60000000\",\"%08x\"]}",
             en2_hex, nonce);
    stratum_handle_message(s, c, msg, out, olen);
}

/* Pull the difficulty out of a mining.set_difficulty notification. */
static double set_diff_value(const char *out) {
    const char *p = out ? strstr(out, "mining.set_difficulty") : NULL;
    if (!p) return -1.0;
    p = strstr(p, "\"params\":[");
    if (!p) return -1.0;
    return atof(p + strlen("\"params\":["));
}

static void handshake(stratum_server_t *s, stratum_conn_t *c) {
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out = NULL; olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out);
}

/* A miner enforcing a local difficulty floor 1000x above what the pool
 * assigned it. Every share it sends clears the floor, but the pool credits
 * each one at the difficulty it assigned and the rate loop sees a share rate
 * inside its deadband, so nothing ever moves it off vardiff_min. Vardiff must
 * notice that every share in the window cleared far more than it asked for,
 * and raise the difficulty to just under the floor it measured.
 *
 * Scaled down from the difficulties this was found at in production (assigned
 * 1, floor 256): difficulty D takes D * 2^32 hashes to mine, so a real 256
 * would be 2^40 hashes per share. The logic is all ratios, so 1e-9 against a
 * 1e-6 floor exercises exactly the same path for ~4300 hashes a share. */
static void test_vardiff_tracks_miner_local_floor(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-9,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 250.0,
                           .vardiff_min = 1e-9,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: nothing is ever a block, so acceptance comes
     * only from the share-difficulty path. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-9);

    /* Mine six shares that each clear the miner's own floor of 1e-6. */
    const int N = 6;
    uint32_t nonce[6];
    double   achieved[6];
    double   floor_seen = 1e300;
    uint32_t from = 1;
    int mined = 1;
    for (int i = 0; i < N; ++i) {
        achieved[i] = mine_nonce(s, c, "J1", "deadbeef", 1e-6, HUGE_VAL,
                                 from, &nonce[i]);
        if (achieved[i] <= 0.0) { mined = 0; break; }
        if (achieved[i] < floor_seen) floor_seen = achieved[i];
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }

    char *out = NULL; size_t olen = 0;
    /* First five land inside the window: counted, no retarget yet. */
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, "deadbeef", nonce[i], &out, &olen);
        CHECK(set_diff_value(out) < 0.0);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == N - 1);
    CHECK(obs.rejects == 0);
    /* Every one was credited at the assigned 1e-9, not the >=1e-6 it actually
     * achieved: the pool books a thousandth of the work it received, which is
     * the under-crediting this fix is about. */
    CHECK(obs.sum_share_diff > 0.999e-9 * (N - 1) &&
          obs.sum_share_diff < 1.001e-9 * (N - 1));
    CHECK(floor_seen > 500.0 * (obs.sum_share_diff / (N - 1)));

    sleep_ms(1100);
    submit_nonce(s, c, "deadbeef", nonce[N - 1], &out, &olen);
    CHECK(obs.shares == N);
    CHECK(obs.rejects == 0);

    /* Difficulty must now sit just under the floor we measured. */
    double got = set_diff_value(out);
    double want = floor_seen * 0.95;
    CHECK(got > 0.0);
    CHECK(got > want * 0.999 && got < want * 1.001);
    /* Under the smallest difficulty actually observed, so the retarget never
     * lands somewhere no share in the window would have reached. */
    CHECK(got < floor_seen);
    CHECK(got > 1e-7);                /* and off vardiff_min for good */
    double held = stratum_conn_difficulty_for_test(c);
    CHECK(held > got * 0.999 && held < got * 1.001);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The floor detector must not block a legitimate downward retarget. A miner
 * whose shares match its assigned difficulty still achieves slightly more
 * than it on every accepted share, so testing the window minimum against the
 * rate loop's already-cut proposal would fire on every 4x cut and pin the
 * difficulty of every miner that simply slowed down. */
static void test_vardiff_still_lowers_for_a_matched_miner(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-6,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 6000.0,
                           .vardiff_min = 1e-12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 1e-6);

    /* A matched miner: every share clears the difficulty it was assigned and
     * overshoots by less than 2x, so the window minimum is nowhere near the
     * 4x the floor check triggers on. Bounding the overshoot is what keeps
     * that true on every run rather than almost every run. */
    const int N = 6;
    uint32_t nonce[6];
    uint32_t from = 1;
    int mined = 1;
    for (int i = 0; i < N; ++i) {
        double d = mine_nonce(s, c, "J1", "deadbeef", 1e-6, 2e-6,
                              from, &nonce[i]);
        if (d <= 0.0) { mined = 0; break; }
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }

    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, "deadbeef", nonce[i], &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == N - 1);
    CHECK(obs.rejects == 0);

    /* Six shares against a 6000/min target is far under rate: the retarget
     * cuts by the 4x step cap, to a quarter of 1e-6. Before the floor check
     * was made to compare against the difficulty actually in force, the
     * window minimum (always at least the assigned difficulty, and here about
     * 1.17e-6) beat this proposal times four, and pinned the difficulty
     * instead of letting it fall. */
    sleep_ms(1100);
    submit_nonce(s, c, "deadbeef", nonce[N - 1], &out, &olen);
    CHECK(obs.rejects == 0);
    double got = set_diff_value(out);
    CHECK(got > 2.49e-7 && got < 2.51e-7);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Vardiff must not raise the share difficulty past the network difficulty.
 * vardiff_min = 1e12 would floor the retarget at 1e12, but the job's
 * network target is DIFF1 (difficulty 1), so the emitted set_difficulty
 * must be clamped to 1. */
static void test_vardiff_clamped_to_network_diff(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 0.001,
                           .vardiff_min = 1e12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* DIFF1 target = difficulty 1.0. */
    uint8_t net[32] = {0};
    net[4] = 0xff; net[5] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Let the vardiff window elapse, then submit: the observed rate blows
     * past target_spm, the retarget floors at vardiff_min, and the network
     * clamp must pull it back down to 1. */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(out != NULL);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "\"params\":[1]") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* After a retarget raises the difficulty, shares mined against the old
 * difficulty must stay acceptable for the grace period (the miner only
 * applies set_difficulty on a later job). */
static void test_vardiff_grace_accepts_old_diff_shares(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e-12,
                           .vardiff_enabled = 1,
                           .vardiff_target_spm = 0.001,
                           .vardiff_min = 1e12,
                           .vardiff_max = 1e15,
                           .vardiff_window_sec = 1,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: nothing is ever a block, so acceptance can
     * only come from the share-difficulty path. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Share #1 after the window elapses triggers a retarget to 1e12
     * (no network clamp: the all-zero target has infinite difficulty). */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Share #2 fails the new 1e12 target but met the old 1e-12 one — the
     * grace window must accept it. */
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Idle-socket reaper: verify the accepted-socket setup path applies
 * SO_RCVTIMEO derived from idle_timeout_sec. We can't cheaply test the
 * "silent client gets dropped" path in a unit test — that would need a
 * real 3s+ sleep — but confirming SO_RCVTIMEO is present is enough to
 * guarantee the recv loop wakes and gets to check last_activity_ms. */
static void test_socket_setup_applies_rcvtimeo(void) {
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    /* idle_timeout=45 → poll interval clamped to 30s (SO_RCVTIMEO ceiling). */
    CHECK(stratum_socket_setup_for_test(sp[0], 45) == 0);
    struct timeval tv = {0};
    socklen_t len = sizeof tv;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
    CHECK(tv.tv_sec == 30);
    /* idle_timeout=5 → poll interval matches (below the 30s clamp). */
    CHECK(stratum_socket_setup_for_test(sp[1], 5) == 0);
    struct timeval tv2 = {0};
    socklen_t len2 = sizeof tv2;
    CHECK(getsockopt(sp[1], SOL_SOCKET, SO_RCVTIMEO, &tv2, &len2) == 0);
    CHECK(tv2.tv_sec == 5);
    close(sp[0]);
    close(sp[1]);
}

/* idle_timeout_sec=0 leaves SO_RCVTIMEO unset — legacy blocking recv. */
static void test_socket_setup_disabled(void) {
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
    CHECK(stratum_socket_setup_for_test(sp[0], 0) == 0);
    struct timeval tv = {0};
    socklen_t len = sizeof tv;
    CHECK(getsockopt(sp[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &len) == 0);
    CHECK(tv.tv_sec == 0 && tv.tv_usec == 0);
    close(sp[0]);
    close(sp[1]);
}

/* Every connection must get a distinct extranonce1. Two connections sharing
 * one render identical coinbases, so they mine the same header and submit the
 * same hash — half the hashrate is wasted and PPS credits the share twice.
 *
 * The old allocator was `seq ^ now_ms()`, which collides whenever the delta in
 * the clock equals the delta in the counter: an even seq at an even
 * millisecond and the next seq a millisecond later produce the same value.
 * Subscribing in a tight loop, as a miner opening several connections at once
 * does, reproduces it. */
static void test_extranonce1_unique_across_connections(void) {
    enum { N = 64 };
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = N, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    if (!s) return;

    char seen[N][9];
    stratum_conn_t *conns[N];
    int collected = 0;

    for (int i = 0; i < N; ++i) {
        conns[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
            &out, &olen);
        if (out) {
            cJSON *resp = parse_first_line(out);
            if (resp) {
                cJSON *ex1 = cJSON_GetArrayItem(
                    cJSON_GetObjectItem(resp, "result"), 1);
                if (cJSON_IsString(ex1)) {
                    snprintf(seen[collected], sizeof(seen[0]), "%s",
                             ex1->valuestring);
                    collected++;
                }
                cJSON_Delete(resp);
            }
            free(out);
        }
        /* Straddle millisecond boundaries so the old clock-XOR allocator is
         * actually given the chance to collide. */
        if (i % 8 == 0) sleep_ms(1);
    }
    CHECK(collected == N);

    int dupes = 0;
    for (int i = 0; i < collected; ++i)
        for (int j = i + 1; j < collected; ++j)
            if (strcmp(seen[i], seen[j]) == 0) dupes++;
    CHECK(dupes == 0);

    for (int i = 0; i < N; ++i) stratum_conn_free_for_test(conns[i]);
    stratum_server_free(s);
}

/* The per-connection ring keys on (job_id|en2|ntime|nonce|version), so the
 * same solution resubmitted under a *different* job id slips past it. When
 * both jobs carry the same template the header — and therefore the hash — is
 * identical, and it must still be credited only once. The server-wide ring
 * keys on the hash itself, which is what makes this hold. */
static void test_dedupe_same_hash_across_job_ids(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Same template, new id. Identical header -> identical hash. */
    stratum_server_set_job(s, make_test_job("J2", net));
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J2\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);  /* still one */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A candidate the node refuses is reported as not accepted, with the node's
 * reason. Before this the submitblock result was discarded and the candidate
 * was recorded as a found block regardless — on a low-difficulty chain that
 * is nearly every candidate, and every one of them credited the pool with a
 * reward that never existed. */
static void test_rejected_candidate_is_not_a_block(void) {
    obs_t obs = {0};
    obs.submit_rejects = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block,
                           .on_block_found = on_block_found };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net));

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.submits == 1);          /* it was offered to the node */
    CHECK(obs.found_calls == 1);      /* and still reported, not dropped */
    CHECK(obs.last_accepted == 0);    /* but not as an accepted block */
    CHECK(strstr(obs.last_submit_error, "inconclusive") != NULL);
    /* The share itself is untouched: the miner did the work, and in
     * pps-classic absorbing this variance is exactly what the pool is for. */
    CHECK(obs.rejects == 0);
    CHECK(obs.shares == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The accepting path still reports accepted, with no error text — a
 * candidate the node took is 'pending', never 'rejected'. */
static void test_accepted_candidate_reports_accepted(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           .initial_diff = 1e12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block,
                           .on_block_found = on_block_found };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.found_calls == 1);
    CHECK(obs.last_accepted == 1);
    CHECK(obs.last_submit_error[0] == '\0');
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A job found by a submit must stay valid for that submit, even though the
 * tip watcher retires and frees jobs on another thread at every new template.
 *
 * This is not hypothetical. find_job used to hand back a borrowed pointer
 * after releasing its lock, and the production pps pool recorded blocks_found
 * rows carrying a freed job's fields — heights 0, 2 and 550 on a chain mining
 * at 963,000+, and two rewards of 1.29 million BTC. A freed job's
 * network_target_be can also make an ordinary hash look like a solved block,
 * which is how those rows came to exist at all.
 *
 * Ring turnover is the trigger: RECENT_JOBS is 8, and on a chain serving
 * several templates a second the ring recycles in seconds, so a submit only
 * has to be slightly slow to be reading freed memory. Here it is forced
 * deterministically — under ASan this aborts without the reference count. */
static void test_job_survives_retirement_while_held(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("HELD", net));

    /* What handle_submit does: take the job, then do slow work with it. */
    stratum_job_t *held = stratum_job_find_for_test(s, "HELD");
    CHECK(held != NULL);
    CHECK(stratum_job_height_for_test(held) == 800000);
    CHECK(stratum_job_value_sats_for_test(held) == 5000000000LL);

    /* Meanwhile the tip watcher churns through enough templates to push HELD
     * out of the retention ring entirely and free it. */
    for (int i = 0; i < 24; ++i) {
        char jid[16];
        snprintf(jid, sizeof jid, "J%d", i);
        stratum_server_set_job(s, make_test_job(jid, net));
    }

    /* It is gone from the lookup — correct, a later submit for it is stale. */
    stratum_job_t *gone = stratum_job_find_for_test(s, "HELD");
    CHECK(gone == NULL);

    /* But the holder's copy is still intact, not recycled memory. These are
     * exactly the two fields that reached blocks_found as garbage. */
    CHECK(stratum_job_height_for_test(held) == 800000);
    CHECK(stratum_job_value_sats_for_test(held) == 5000000000LL);

    stratum_job_free(held);
    stratum_server_free(s);
}

/* The reference must not leak either: once the holder lets go, the job is
 * destroyed rather than pinned for the process's lifetime. Run under ASan or
 * valgrind, a leak here is the failure. */
static void test_held_job_is_freed_on_release(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("A", net));
    stratum_job_t *held = stratum_job_find_for_test(s, "A");
    CHECK(held != NULL);
    stratum_job_free(held);        /* holder done; server still owns one */
    stratum_server_free(s);        /* server drops the last one */
}

/* While accrual is suspended the pool must turn miners away, not bank their
 * work. A miner whose shares are accepted but never credited is mining for
 * free without being told — worse than being refused, because they cannot
 * tell it is happening. */
static void test_gated_pps_refuses_authorize_and_submits(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 1, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 1,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_btc_address, sizeof cfg.pool_btc_address, "%s", TEST_ADDR);
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;

    /* Authorize is refused, and the reason says what to do about it. */
    int rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(strstr(out, "not crediting shares") != NULL);
    CHECK(stratum_conn_authorized_for_test(c) == 0);
    free(out); out=NULL; olen=0;

    /* A miner that got in before the gate closed is stopped too. */
    gate = 0;
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out); out=NULL; olen=0;

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net));
    gate = 1;
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "not crediting shares") != NULL);
    CHECK(obs.shares == 0);            /* nothing banked */
    free(out); out=NULL; olen=0;

    /* And once the chain retargets, work is taken again. */
    gate = 0;
    stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* An operator who has deliberately turned the refusal off keeps taking work. */
static void test_gate_can_be_disabled(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 1, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.pool_btc_address, sizeof cfg.pool_btc_address, "%s", TEST_ADDR);
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"2sYBNmMJMMZHi6xasMcCPgNiYJ1z\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Solo has no accrual to suspend, so the gate must never touch it. */
static void test_solo_is_never_gated(void) {
    obs_t obs = {0};
    _Atomic int gate = 1;
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .pps_enabled = 0, .pps_gate = &gate,
                          .pps_refuse_shares_below_min = 1,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}", &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

int main(void) {
    test_subscribe();
    test_authorize_triggers_setdiff_notify();
    test_submit_unknown_job();
    test_submit_share_and_dedupe();
    test_authorize_rejects_non_address();
    test_authorize_address_with_label();
    test_block_wins_over_low_difficulty();
    test_vardiff_tracks_miner_local_floor();
    test_vardiff_still_lowers_for_a_matched_miner();
    test_vardiff_clamped_to_network_diff();
    test_vardiff_grace_accepts_old_diff_shares();
    test_socket_setup_applies_rcvtimeo();
    test_socket_setup_disabled();
    test_extranonce1_unique_across_connections();
    test_dedupe_same_hash_across_job_ids();
    test_rejected_candidate_is_not_a_block();
    test_accepted_candidate_reports_accepted();
    test_job_survives_retirement_while_held();
    test_held_job_is_freed_on_release();
    test_gated_pps_refuses_authorize_and_submits();
    test_gate_can_be_disabled();
    test_solo_is_never_gated();
    printf("test_stratum: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
