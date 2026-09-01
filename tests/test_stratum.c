#include "../src/stratum.h"
#include "../src/share.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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

/* The callbacks run on whichever thread handled the share, and
 * test_job_rotation_races_submits drives several at once. Guarding the
 * observer keeps a sanitizer reporting races in the code under test rather
 * than in the harness watching it. Single-threaded tests pay one uncontended
 * lock per callback. */
static pthread_mutex_t obs_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_share(void *ctx, const char *w, const char *addr,
                     uint64_t ts, double d,
                     int is_block, const char *blk) {
    (void)ts; (void)blk; (void)addr;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    o->shares++;
    o->sum_share_diff += d;
    o->last_is_block = is_block;
    if (is_block) o->blocks++;
    snprintf(o->last_worker, sizeof(o->last_worker), "%s", w ? w : "");
    pthread_mutex_unlock(&obs_mu);
}
static void on_reject(void *ctx, const char *w, uint64_t ts, const char *r) {
    (void)ts; (void)w;
    obs_t *o = ctx;
    pthread_mutex_lock(&obs_mu);
    o->rejects++;
    snprintf(o->last_reason, sizeof(o->last_reason), "%s", r ? r : "");
    pthread_mutex_unlock(&obs_mu);
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
                           STRATUM_EXTRANONCE1_SIZE, STRATUM_EXTRANONCE2_SIZE,
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
        CHECK(cJSON_IsString(ex1) &&
              strlen(ex1->valuestring) == STRATUM_EXTRANONCE1_SIZE * 2);
        cJSON *ex2sz = cJSON_GetArrayItem(result, 2);
        CHECK(cJSON_IsNumber(ex2sz) &&
              ex2sz->valueint == STRATUM_EXTRANONCE2_SIZE);
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
    stratum_server_set_job(s, make_test_job("0001", net), 1);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.last_is_block == 0);
    CHECK(obs.blocks == 0);
    free(out); out=NULL; olen=0;

    /* Duplicate: same parameters again. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);  /* not incremented */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* An extranonce2 of any width other than the one advertised on subscribe
 * must be rejected, not credited. cb1 carries a scriptSig length varint
 * computed from en1_size + en2_size, so a short or long extranonce2 yields a
 * coinbase whose declared scriptSig length disagrees with its contents --
 * an invalid transaction. The header over that coinbase still hashes, so an
 * unchecked pool would credit the share and only discover the problem when
 * the network rejected a block. */
static void test_submit_rejects_wrong_extranonce2_size(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,   /* any hash clears the target */
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* Four bytes -- what a miner that ignored mining.subscribe and assumed
     * the classic width would send. */
    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 0);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "extranonce2 size") != NULL);
    CHECK(out != NULL && strstr(out, "wrong extranonce2 size") != NULL);
    free(out); out=NULL; olen=0;

    /* Too wide is equally wrong. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe00\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 0);
    CHECK(obs.rejects == 2);
    free(out); out=NULL; olen=0;

    /* Exactly the advertised width still works. */
    rc = stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Accepting a connection whose peer has already gone away.
 *
 * listener_thread hands the new connection to a thread and then does a little
 * bookkeeping on it. That thread owns the connection and frees it when the
 * connection ends — so if the peer is already gone, it can finish before the
 * bookkeeping runs, and the bookkeeping then lands on freed memory. One of the
 * two touches is a write, which corrupts the allocator's own structures rather
 * than merely reading rubbish, so the damage usually surfaces later and
 * somewhere unrelated.
 *
 * Connecting and closing immediately, many times over, is the shape that hits
 * it. Under -fsanitize=address this is reported at the touch; without a
 * sanitizer it must simply not crash. */
#define CHURN_ROUNDS 300

static void test_accept_churn_peer_gone(void) {
    obs_t obs = {0};
    stratum_server_t *s = NULL;
    int port = 0;

    /* Any free high port. Bind failure is an environment problem, not a test
     * failure, so try a few and skip if the sandbox forbids listening. */
    for (int p = 39331; p < 39341 && !s; ++p) {
        stratum_cfg_t cfg = { .bind_port = p, .max_conns = CHURN_ROUNDS + 16,
                               .initial_diff = 1.0,
                               .ctx = &obs, .on_share = on_share,
                               .on_reject = on_reject, .on_block = on_block };
        snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
        if (stratum_server_start(&cfg, &s) == 0 && s) { port = p; break; }
        s = NULL;
    }
    if (!s) {
        fprintf(stderr, "SKIP accept-churn: could not bind a local port\n");
        return;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int connected = 0;
    for (int i = 0; i < CHURN_ROUNDS; ++i) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
            connected++;
            /* Abort rather than close politely: RST tears the connection down
             * at once, so the server's thread reaches its teardown as early as
             * possible — which is the whole point. */
            struct linger lg = { .l_onoff = 1, .l_linger = 0 };
            setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        }
        close(fd);
    }

    /* Precondition: connections actually had to be established, or the accept
     * path this test exists to exercise was never entered. */
    CHECK(connected > 0);

    /* Let the accept and teardown threads finish while the server is still up,
     * so anything they corrupt is attributed here rather than at exit. */
    sleep_ms(300);
    stratum_server_free(s);
}

/* Job rotation against concurrent submits.
 *
 * The job-swap thread walks every connection on each set_job and re-renders
 * that connection's coinbase; the connection's own thread renders and reads
 * the same buffers on submit. Nothing in the suite exercised those two
 * against each other, so the resulting use-after-free on cb1/cb2 was
 * invisible to it.
 *
 * Under -fsanitize=address (make asan) or ThreadSanitizer this reproduces the
 * defect on the unfixed code. Without a sanitizer it still drives the paths
 * and must not crash. */
#define RACE_CONNS   4
#define RACE_SUBMITS 400
#define RACE_JOBS    300

typedef struct {
    stratum_server_t *s;
    stratum_conn_t   *c;
    int               id;
} race_arg_t;

static atomic_int race_stop;
/* Index of the job most recently installed by race_job_thread. Submitting
 * against anything else is answered by find_job() with "stale or unknown
 * job", which returns long before the coinbase is rendered — so a submitter
 * picking job ids on its own never reaches the code this test exists to
 * race. */
static atomic_int race_cur_job;

static void *race_submit_thread(void *arg) {
    race_arg_t *a = (race_arg_t *)arg;
    char msg[256];
    for (int i = 0; i < RACE_SUBMITS && !atomic_load(&race_stop); ++i) {
        char *out = NULL; size_t olen = 0;
        /* Track the live job, and every fourth submit aim one behind it, so
         * both the current_job fast path and the recent-ring path of
         * find_job() are exercised against a concurrent retire. */
        int cur = atomic_load(&race_cur_job);
        int target = (i % 4 == 0 && cur > 0) ? cur - 1 : cur;
        /* extranonce2 must be exactly the width advertised at subscribe
         * (8 bytes); any other width is refused before the coinbase is
         * built, which would put this test back to proving nothing. */
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J%d\",\"%08x%08x\",\"60000000\",\"%08x\"]}",
                 target, (unsigned)a->id, (unsigned)i, (unsigned)i);
        stratum_handle_message(a->s, a->c, msg, &out, &olen);
        free(out);
    }
    return NULL;
}

/* Drain the miner side of each socketpair. With no reader the kernel buffer
 * fills and the broadcast's write blocks — which is the head-of-line stall
 * SO_SNDTIMEO now bounds. Here we want the race, not the stall, so these act
 * like miners that read. */
static void *race_drain_thread(void *arg) {
    int fd = *(int *)arg;
    char sink[4096];
    while (!atomic_load(&race_stop)) {
        ssize_t n = recv(fd, sink, sizeof sink, MSG_DONTWAIT);
        if (n > 0) continue;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *race_job_thread(void *arg) {
    stratum_server_t *s = (stratum_server_t *)arg;
    uint8_t net[32];
    memset(net, 0xff, sizeof net);
    for (int i = 0; i < RACE_JOBS && !atomic_load(&race_stop); ++i) {
        char jid[16];
        snprintf(jid, sizeof jid, "J%d", i);
        stratum_job_t *j = make_test_job(jid, net);
        if (!j) break;
        stratum_server_set_job(s, j, 1);
        atomic_store(&race_cur_job, i);
    }
    return NULL;
}

static void test_job_rotation_races_submits(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = RACE_CONNS,
                           .initial_diff = 1.0,
                           .vardiff_enabled = 1, .vardiff_window_sec = 1,
                           .vardiff_target_spm = 60, .vardiff_min = 0.001,
                           .vardiff_max = 1e6,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    atomic_store(&race_stop, 0);
    atomic_store(&race_cur_job, 0);

    uint8_t net[32];
    memset(net, 0xff, sizeof net);
    stratum_server_set_job(s, make_test_job("J0", net), 1);

    /* Each connection needs a real fd and a place in the broadcast list, or
     * set_job skips it and the race under test never happens. socketpair
     * gives a writable fd with a peer we control. */
    stratum_conn_t *conns[RACE_CONNS];
    int             fds[RACE_CONNS][2];
    race_arg_t      args[RACE_CONNS];
    pthread_t       subs[RACE_CONNS], jobthr;

    for (int i = 0; i < RACE_CONNS; ++i) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i]) == 0);
        conns[i] = stratum_conn_new_for_test(s);
        char *out = NULL; size_t olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
            &out, &olen); free(out); out = NULL; olen = 0;
        stratum_handle_message(s, conns[i],
            "{\"id\":2,\"method\":\"mining.authorize\","
             "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
            &out, &olen); free(out);
        /* Precondition: an unauthorized or unsubscribed conn is skipped by
         * the broadcast loop, and this test would prove nothing. */
        CHECK(stratum_conn_authorized_for_test(conns[i]) == 1);
        CHECK(stratum_conn_subscribed_for_test(conns[i]) == 1);
        stratum_conn_register_for_test(s, conns[i], fds[i][0]);
        args[i].s = s; args[i].c = conns[i]; args[i].id = i;
    }

    pthread_t drains[RACE_CONNS];
    int       peer[RACE_CONNS];
    for (int i = 0; i < RACE_CONNS; ++i) {
        peer[i] = fds[i][1];
        pthread_create(&drains[i], NULL, race_drain_thread, &peer[i]);
    }

    pthread_create(&jobthr, NULL, race_job_thread, s);
    for (int i = 0; i < RACE_CONNS; ++i)
        pthread_create(&subs[i], NULL, race_submit_thread, &args[i]);

    for (int i = 0; i < RACE_CONNS; ++i) pthread_join(subs[i], NULL);
    atomic_store(&race_stop, 1);
    pthread_join(jobthr, NULL);
    for (int i = 0; i < RACE_CONNS; ++i) pthread_join(drains[i], NULL);

    /* Precondition, and the load-bearing assertion of this test: shares must
     * actually have been CREDITED. Every earlier draft of this test passed
     * while proving nothing — first because the submitters aimed at job ids
     * find_job() answered with "stale or unknown job", then because their
     * extranonce2 was the wrong width. Both are refused long before the
     * coinbase is rendered, so the render/free path being raced here was
     * never reached. A submit that is merely *rejected* is not evidence. */
    CHECK(obs.shares > 0);

    for (int i = 0; i < RACE_CONNS; ++i) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    stratum_server_free(s);
    for (int i = 0; i < RACE_CONNS; ++i) stratum_conn_free_for_test(conns[i]);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
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

    uint8_t en2[STRATUM_EXTRANONCE2_SIZE];
    for (size_t i = 0; i < STRATUM_EXTRANONCE2_SIZE; ++i) {
        unsigned byte = 0;
        sscanf(en2_hex + 2 * i, "%2x", &byte);
        en2[i] = (uint8_t)byte;
    }

    size_t cb_len = cb1_len + STRATUM_EXTRANONCE1_SIZE +
                    STRATUM_EXTRANONCE2_SIZE + cb2_len;
    uint8_t *cb = malloc(cb_len);
    if (!cb) return 0.0;
    size_t off = 0;
    memcpy(cb + off, cb1, cb1_len);
    off += cb1_len;
    memcpy(cb + off, en1, STRATUM_EXTRANONCE1_SIZE);
    off += STRATUM_EXTRANONCE1_SIZE;
    memcpy(cb + off, en2, STRATUM_EXTRANONCE2_SIZE);
    off += STRATUM_EXTRANONCE2_SIZE;
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        achieved[i] = mine_nonce(s, c, "J1", "deadbeefcafebabe", 1e-6, HUGE_VAL,
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
        submit_nonce(s, c, "deadbeefcafebabe", nonce[i], &out, &olen);
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
    submit_nonce(s, c, "deadbeefcafebabe", nonce[N - 1], &out, &olen);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        double d = mine_nonce(s, c, "J1", "deadbeefcafebabe", 1e-6, 2e-6,
                              from, &nonce[i]);
        if (d <= 0.0) { mined = 0; break; }
        from = nonce[i] + 1;
    }
    CHECK(mined == 1);
    if (!mined) { stratum_conn_free_for_test(c); stratum_server_free(s); return; }

    /* The window was armed at authorize, and the mining above ran inside it —
     * 163 ms on a quiet machine, over 8 s under a sanitizer. Crossing the
     * 1 s boundary here spends a retarget this test never asked for, and
     * everything below then measures the wrong window. Re-arm so the test
     * turns on behaviour rather than on how fast the box mines. */
    stratum_conn_rearm_vardiff_for_test(c);

    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < N - 1; ++i) {
        submit_nonce(s, c, "deadbeefcafebabe", nonce[i], &out, &olen);
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
    submit_nonce(s, c, "deadbeefcafebabe", nonce[N - 1], &out, &olen);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(out != NULL);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "\"params\":[1]") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* After a retarget raises the difficulty, shares for a job the miner already
 * holds must stay acceptable at the difficulty that job went out under -- the
 * miner only applies set_difficulty on a later job. */
static void test_submit_judged_at_the_jobs_own_difficulty(void) {
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Share #2 fails the new 1e12 target but met the 1e-12 that J1 was
     * notified under, which is the difficulty the miner was actually working
     * to. It has to be accepted. */
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The regression a single remembered previous difficulty could not survive.
 * Vardiff retargets every window, so a miner on a slow chain sees several of
 * them while still holding one job. Keeping only the value from before the
 * most recent retarget loses the one the job actually went out under, and the
 * pool then rejects honest work -- the failure marketplaces report as a pool
 * that "passes the extranonce check then collapses when diff changes".
 *
 * Two retargets, then a share for the original job. It was mined against the
 * difficulty J1 was notified under and must be credited at that difficulty,
 * however many retargets have happened since. */
static void test_job_difficulty_survives_repeated_retargets(void) {
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* First retarget: vardiff_min floors it at 1e12. */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    double first = set_diff_value(out);
    CHECK(first > 0.999e12 && first < 1.001e12);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Second retarget: the 4x step cap takes it to 4e12. This is the one that
     * used to be fatal -- it overwrote the remembered 1e-12 with 1e12. */
    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000002\"]}",
        &out, &olen);
    double second = set_diff_value(out);
    CHECK(second > 3.99e12 && second < 4.01e12);
    CHECK(obs.shares == 2);
    free(out); out=NULL; olen=0;

    /* Still J1, still mined at the 1e-12 it was notified under. It clears
     * neither 4e12 nor the 1e12 from one retarget ago, so nothing but the
     * job's own difficulty can accept it. */
    stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000003\"]}",
        &out, &olen);
    CHECK(obs.shares == 3);
    CHECK(obs.rejects == 0);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The other half of the contract: judging at the job's difficulty is not a
 * blanket amnesty for anything the miner has ever been assigned. A job handed
 * out *after* a retarget carries the new difficulty, and a share that would
 * have passed under the old one must be rejected. Without this, a miner could
 * hold the easiest difficulty it was ever given for the life of the
 * connection. */
static void test_job_notified_after_a_retarget_uses_the_new_difficulty(void) {
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

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    sleep_ms(1100);
    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(set_diff_value(out) > 0.999e12);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* New job, and a fresh authorize to notify this connection of it -- the
     * broadcast path walks the server's live-connection list, which a test
     * connection is not on. The notify records J2 against the 1e12 now in
     * force, which is exactly what a real miner would be told. */
    stratum_server_set_job(s, make_test_job("J2", net), 1);
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    /* An easy hash: fine under J1's 1e-12, nowhere near J2's 1e12. */
    stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J2\",\"deadbeefcafebabe\",\"60000000\",\"00000009\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);   /* still just the one */
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "low difficulty") != NULL);
    CHECK(out != NULL && strstr(out, "low difficulty") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A miner is served the difficulty of the port it dialled.
 *
 * One difficulty cannot serve both a home ASIC and a rented fleet: at
 * difficulty 1024 a 1 PH/s order is ~227 shares per second down a single
 * connection, and the marketplaces refuse to deliver below their own floor
 * because of it. So the port carries the policy, and the connection inherits
 * it at accept time. */
static void test_listener_policy_sets_the_connections_difficulty(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 4,
                          .initial_diff = 1.0,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 12.0,
                          .vardiff_min = 1.0,
                          .vardiff_max = 1e15,
                          .vardiff_window_sec = 30,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* All-zero network target: difficulty is effectively infinite, so the
     * network clamp cannot mask what the listener asked for. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    /* The default port keeps the server-wide difficulty. */
    stratum_conn_t *home = stratum_conn_new_for_test(s);
    handshake(s, home);
    CHECK(stratum_conn_difficulty_for_test(home) == 1.0);

    /* The rental port starts at its own floor, with no ramp to get there. */
    stratum_listener_t rental = { .port = 3335, .initial_diff = 65536.0,
                                  .vardiff_min = 65536.0, .vardiff_max = 0.0,
                                  .label = "braiins" };
    stratum_conn_t *rent = stratum_conn_new_for_test(s);
    stratum_conn_apply_listener_for_test(rent, &rental);
    handshake(s, rent);
    CHECK(stratum_conn_difficulty_for_test(rent) == 65536.0);

    /* And the two coexist: serving the fleet did not move the home miner. */
    CHECK(stratum_conn_difficulty_for_test(home) == 1.0);

    stratum_conn_free_for_test(home);
    stratum_conn_free_for_test(rent);
    stratum_server_free(s);
}

/* A field the listener leaves at zero keeps the server-wide default, so a
 * port can raise its floor without restating the whole policy. */
static void test_listener_policy_falls_back_per_field(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                          .initial_diff = 7.0,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 12.0,
                          .vardiff_min = 3.0,
                          .vardiff_max = 1e15,
                          .vardiff_window_sec = 30,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    /* Only the port is set: everything else must come from the server. */
    stratum_listener_t bare = { .port = 3336 };
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    stratum_conn_apply_listener_for_test(c, &bare);
    handshake(s, c);
    CHECK(stratum_conn_difficulty_for_test(c) == 7.0);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A port that set only vardiff_min asked the rate loop to stay above a
 * number. It did not promise anyone that difficulty, so the network ceiling
 * still wins.
 *
 * Share difficulty is not raised above the network difficulty by default: a
 * miner filters locally against the stratum target, so a share target harder
 * than the network target discards valid blocks before the pool sees them. A
 * port that genuinely needs the floor kept says min_diff, and pays for it in
 * exactly those discarded blocks -- see
 * test_promised_min_diff_survives_the_network_clamp. Everything else keeps
 * the old behaviour, which is what makes that trade affordable. The
 * dashboard's listener_difficulty health check reports either case. */
static void test_listener_difficulty_still_clamped_to_the_network(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                          .initial_diff = 1.0,
                          .vardiff_enabled = 1,
                          .vardiff_target_spm = 12.0,
                          .vardiff_min = 1.0,
                          .vardiff_max = 1e15,
                          .vardiff_window_sec = 30,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* DIFF1 network target — the whole chain is at difficulty 1. */
    uint8_t net[32] = {0};
    net[4] = 0xff; net[5] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_listener_t rental = { .port = 3337, .initial_diff = 500000.0,
                                  .vardiff_min = 500000.0,
                                  .label = "nicehash" };
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    stratum_conn_apply_listener_for_test(c, &rental);
    handshake(s, c);
    /* Not 500000. The chain cannot back it, and pretending otherwise costs
     * blocks. */
    CHECK(stratum_conn_difficulty_for_test(c) == 1.0);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The submit ceiling.
 *
 * A connection's share rate is its hashrate over the difficulty it was given,
 * and a fleet pointed at a home-miner port makes those wildly mismatched: 1
 * PH/s against difficulty 1 is ~232,000 submits per second down one socket.
 * Past the ceiling a submit is refused before any validation work, so the
 * flood costs a reply rather than a coinbase render and four SHA256 passes. */
static void test_submit_ceiling_refuses_past_the_limit(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,   /* any hash clears it */
                          .max_submits_per_sec = 5,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    char *out = NULL; size_t olen = 0;
    char msg[256];
    /* Five land, the rest are refused — all inside one second, so they share
     * a window. Distinct nonces, so nothing is refused as a duplicate. */
    for (int i = 0; i < 40; ++i) {
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\","
                 "\"60000000\",\"%08x\"]}", (unsigned)(i + 1));
        stratum_handle_message(s, c, msg, &out, &olen);
        if (i >= 5) {
            CHECK(out != NULL && strstr(out, "submitting too fast") != NULL);
        }
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 5);

    /* One report for the whole flood, not one per refusal: reporting each
     * would write tens of thousands of rows a second into the table that
     * exists to account for shares. */
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "submitting too fast") != NULL);

    /* ...but the ones after that first report are not lost. A flood usually
     * ends by going quiet, well inside the reporting interval, so the tail is
     * flushed when the connection goes away. Without this an operator sees
     * "1 refused" for a burst of thirty-five. */
    stratum_conn_free_for_test(c);
    CHECK(obs.rejects == 2);
    CHECK(strstr(obs.last_reason, "35 total") != NULL);

    stratum_server_free(s);
}

/* The window rolls: a miner refused in one second is served again in the
 * next. The ceiling throttles, it does not ban. */
static void test_submit_ceiling_window_rolls(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .max_submits_per_sec = 2,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    char *out = NULL; size_t olen = 0;
    char msg[256];
    unsigned nonce = 1;
    for (int i = 0; i < 4; ++i) {
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\","
                 "\"60000000\",\"%08x\"]}", nonce++);
        stratum_handle_message(s, c, msg, &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 2);

    sleep_ms(1100);
    for (int i = 0; i < 2; ++i) {
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\","
                 "\"60000000\",\"%08x\"]}", nonce++);
        stratum_handle_message(s, c, msg, &out, &olen);
        CHECK(out != NULL && strstr(out, "submitting too fast") == NULL);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 4);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Zero means no ceiling, which is what every existing deployment and every
 * other test in this file runs with. */
static void test_submit_ceiling_zero_disables(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .max_submits_per_sec = 0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    char *out = NULL; size_t olen = 0;
    char msg[256];
    for (int i = 0; i < 50; ++i) {
        snprintf(msg, sizeof msg,
                 "{\"id\":9,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\","
                 "\"60000000\",\"%08x\"]}", (unsigned)(i + 1));
        stratum_handle_message(s, c, msg, &out, &olen);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 50);
    CHECK(obs.rejects == 0);

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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

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
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(obs.shares == 1);
    free(out); out=NULL; olen=0;

    /* Same template, new id. Identical header -> identical hash. */
    stratum_server_set_job(s, make_test_job("J2", net), 1);
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J2\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
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
    stratum_server_set_job(s, make_test_job("HELD", net), 1);

    /* What handle_submit does: take the job, then do slow work with it. */
    stratum_job_t *held = stratum_job_find_for_test(s, "HELD");
    CHECK(held != NULL);
    CHECK(stratum_job_height_for_test(held) == 800000);
    CHECK(stratum_job_value_sats_for_test(held) == 5000000000LL);

    /* Meanwhile the tip watcher churns through enough templates to push HELD
     * out of the retention ring entirely and free it. */
    /* ⚠️ Must exceed RECENT_JOBS, which lives in stratum.c and is not visible
     * here -- so this count cannot be derived and has to be kept ahead of it by
     * hand. It was 24 against a ring of 8; the ring is now 16. If it ever drops
     * below the ring size, HELD stays findable, `gone == NULL` fails, and had
     * the assertion been written the other way the test would have passed while
     * exercising nothing. */
    for (int i = 0; i < 64; ++i) {
        char jid[16];
        snprintf(jid, sizeof jid, "J%d", i);
        stratum_server_set_job(s, make_test_job(jid, net), 1);
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
    stratum_server_set_job(s, make_test_job("A", net), 1);
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
    stratum_server_set_job(s, make_test_job("J1", net), 1);
    gate = 1;
    stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(strstr(out, "not crediting shares") != NULL);
    CHECK(obs.shares == 0);            /* nothing banked */
    free(out); out=NULL; olen=0;

    /* And once the chain retargets, work is taken again. */
    gate = 0;
    stratum_handle_message(s, c,
        "{\"id\":5,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"60000000\",\"00000002\"]}",
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


/* ---------------------------------------------------------------------- */
/* Rented-hashrate compatibility                                           */
/* ---------------------------------------------------------------------- */

/* Pull the clean_jobs flag (params[8]) out of a mining.notify line.
 * Returns 1/0, or -1 if there is no notify in the buffer. */
static int notify_clean_flag(const char *buf, size_t len) {
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, buf, len);
    copy[len] = '\0';
    int result = -1;
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strstr(line, "mining.notify")) continue;
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (cJSON_IsArray(params) && cJSON_GetArraySize(params) == 9) {
            cJSON *clean = cJSON_GetArrayItem(params, 8);
            if (cJSON_IsBool(clean)) result = cJSON_IsTrue(clean) ? 1 : 0;
        }
        cJSON_Delete(msg);
        break;
    }
    free(copy);
    return result;
}

/* clean_jobs is an instruction to throw work away, so it may only be sent
 * when the work is genuinely dead — a new tip. The periodic template refresh
 * (a fresher ntime, newly arrived transactions) leaves the miner's job
 * perfectly mineable, and flagging it clean discards hashrate in flight on
 * every connected miner, roughly twenty times per block on a 10-minute chain.
 *
 * This reads the flag off a real socket rather than off the handler, because
 * the bug lived in the broadcast path and not in the handler: a probe that
 * connects, reads its first notify and leaves never sees a second broadcast
 * at all, which is exactly why this was invisible to probing while miners'
 * logs showed it. */
static void test_clean_jobs_only_on_a_new_tip(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 4, .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    stratum_conn_attach_for_test(s, c, sv[0]);

    char rx[8192];

    /* Periodic refresh: same tip, new template. The miner keeps its work. */
    stratum_server_set_job(s, make_test_job("J2", net), 0);
    ssize_t n = read(sv[1], rx, sizeof rx);
    CHECK(n > 0);
    CHECK(n > 0 && notify_clean_flag(rx, (size_t)n) == 0);

    /* New tip: every job in every miner's hands builds on a parent that is no
     * longer the tip, so this one really does mean start over. */
    stratum_server_set_job(s, make_test_job("J3", net), 1);
    n = read(sv[1], rx, sizeof rx);
    CHECK(n > 0);
    CHECK(n > 0 && notify_clean_flag(rx, (size_t)n) == 1);

    /* The refresh did not cost the miner its old job either: the pool still
     * takes submits against it out of the recent ring. */
    stratum_job_t *held = stratum_job_find_for_test(s, "J2");
    CHECK(held != NULL);
    if (held) stratum_job_free(held);

    stratum_conn_detach_for_test(s, c);
    close(sv[0]); close(sv[1]);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* The counterpart. A port that says min_diff is making a promise a
 * marketplace will measure on the wire, so that floor is kept even where the
 * chain is easier -- and the miner then discards blocks it solved, which is
 * the price of the port being usable at all. */
static void test_promised_min_diff_survives_the_network_clamp(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 2,
                          .initial_diff = 1.0,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* DIFF1 network target: the whole chain sits at difficulty 1, three
     * orders of magnitude under what the rental port advertises. */
    uint8_t net[32] = {0};
    net[4] = 0xff; net[5] = 0xff;
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_listener_t rental = { .port = 3335, .initial_diff = 65536.0,
                                  .vardiff_min = 65536.0,
                                  .min_diff = 65536.0,
                                  .label = "braiins" };
    stratum_conn_t *rent = stratum_conn_new_for_test(s);
    stratum_conn_apply_listener_for_test(rent, &rental);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, rent, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out = NULL; olen = 0;
    stratum_handle_message(s, rent,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}", &out, &olen);
    CHECK(stratum_conn_difficulty_for_test(rent) == 65536.0);
    /* And the fleet is told so on its first set_difficulty, not after four
     * minutes of vardiff climbing -- which is the window in which an order
     * gets cancelled. */
    CHECK(set_diff_value(out) == 65536.0);
    free(out);

    /* The home port on the same pool and the same chain is untouched: it
     * promised nothing, so it still gets the chain's own difficulty and keeps
     * every block it finds. */
    stratum_conn_t *home = stratum_conn_new_for_test(s);
    handshake(s, home);
    CHECK(stratum_conn_difficulty_for_test(home) == 1.0);

    stratum_conn_free_for_test(rent);
    stratum_conn_free_for_test(home);
    stratum_server_free(s);
}

/* Run one connection through a window that leaves the rate loop wanting a
 * lower difficulty, and report where it ended up. Everything is scaled down
 * by ~2^30 from the real numbers so a share costs thousands of hashes rather
 * than trillions; the logic is all ratios, so the path exercised is the same
 * one 1024 would take. */
static double vardiff_after_a_slow_window(double promised_min_diff,
                                          int *emitted_setdiff) {
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

    uint8_t net[32] = {0};   /* nothing is ever a block */
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    if (promised_min_diff > 0.0) {
        stratum_listener_t rental = { .port = 3335,
                                      .initial_diff = 1e-6,
                                      .vardiff_min = 1e-12,
                                      .min_diff = promised_min_diff,
                                      .label = "rental" };
        stratum_conn_apply_listener_for_test(c, &rental);
    }
    handshake(s, c);

    uint32_t n1 = 0, n2 = 0;
    double a1 = mine_nonce(s, c, "J1", "deadbeefcafebabe", 1e-6, HUGE_VAL, 1, &n1);
    double a2 = mine_nonce(s, c, "J1", "deadbeefcafebabe", 1e-6, HUGE_VAL,
                           n1 + 1, &n2);
    CHECK(a1 > 0.0 && a2 > 0.0);
    if (a1 <= 0.0 || a2 <= 0.0) {
        stratum_conn_free_for_test(c); stratum_server_free(s); return -1.0;
    }

    char *out = NULL; size_t olen = 0;
    submit_nonce(s, c, "deadbeefcafebabe", n1, &out, &olen);
    free(out); out = NULL; olen = 0;

    /* Two shares in ~1.1s against a 6000/min target: the rate loop sees a
     * miner running two orders of magnitude slow and wants to cut. */
    sleep_ms(1100);
    submit_nonce(s, c, "deadbeefcafebabe", n2, &out, &olen);
    CHECK(obs.shares == 2);
    CHECK(obs.rejects == 0);
    *emitted_setdiff = set_diff_value(out) > 0.0;
    free(out);

    double got = stratum_conn_difficulty_for_test(c);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    return got;
}

/* Vardiff is the other way a connection ends up under the floor: authorize
 * gets it right and then the rate loop walks it straight back down. */
static void test_vardiff_cannot_retarget_below_a_promised_floor(void) {
    int emitted = -1;
    /* A port promising nothing: the rate loop cuts by its 4x step, as it
     * always has. */
    double unfloored = vardiff_after_a_slow_window(0.0, &emitted);
    CHECK(unfloored > 0.0);
    CHECK(unfloored < 1e-6);
    CHECK(emitted == 1);

    /* A port promising the starting difficulty: the same window produces the
     * same proposal, the floor overrides it, and — since nothing moved — the
     * miner is not sent a pointless set_difficulty either. */
    emitted = -1;
    double floored = vardiff_after_a_slow_window(1e-6, &emitted);
    CHECK(floored == 1e-6);
    CHECK(emitted == 0);
}

/* A submit whose ntime differs from the job's is the normal case, not an
 * error: ntime rolling is how a miner extends its search space without
 * touching extranonce2. */
static void test_ntime_rolling_is_accepted(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    /* job ntime is 0x60000000. Roll an hour forward, both boundaries, and a
     * little backward (a proxy or a skewed rig clock lands there). */
    static const char *ok_ntimes[] = {
        "60000e10",  /* +3600 */
        "60001c20",  /* +7200, the forward boundary */
        "5ffffda8",  /* -600, the backward boundary */
        "60000000",  /* the job's own value */
    };
    char *out = NULL; size_t olen = 0;
    for (size_t i = 0; i < sizeof ok_ntimes / sizeof ok_ntimes[0]; ++i) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "{\"id\":3,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"%s\",\"0000000%zu\"]}",
                 ok_ntimes[i], i);
        int rc = stratum_handle_message(s, c, msg, &out, &olen);
        CHECK(rc == 0);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 4);
    CHECK(obs.rejects == 0);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Past the consensus window the header still hashes and still looks like a
 * fine share, but the chain will not take a block built on it. Accepting it
 * means crediting work that can never pay, and — if it beats the network
 * target — assembling a block the node throws out, which is how a solved
 * block vanishes leaving only a submitblock warning. */
static void test_ntime_outside_the_window_is_rejected(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                          .initial_diff = 1e-12,
                          .ctx = &obs, .on_share = on_share,
                          .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    handshake(s, c);

    static const char *bad_ntimes[] = {
        "60001c21",  /* +7201, one second past the forward bound */
        "5ffffda7",  /* -601, one second past the backward bound */
        "ffffffff",  /* a miner rolling into 2106 */
    };
    char *out = NULL; size_t olen = 0;
    for (size_t i = 0; i < sizeof bad_ntimes / sizeof bad_ntimes[0]; ++i) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "{\"id\":3,\"method\":\"mining.submit\","
                 "\"params\":[\"w\",\"J1\",\"deadbeefcafebabe\",\"%s\",\"0000000%zu\"]}",
                 bad_ntimes[i], i);
        int rc = stratum_handle_message(s, c, msg, &out, &olen);
        /* Rejected, but the connection stays: this is a bad share, not a bad
         * client. */
        CHECK(rc == 0);
        CHECK(strstr(out, "\"error\"") != NULL);
        free(out); out = NULL; olen = 0;
    }
    CHECK(obs.shares == 0);
    CHECK(obs.rejects == 3);
    CHECK(strstr(obs.last_reason, "ntime") != NULL);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* A JSON-RPC response carries no "method", and the handler used to read that
 * as a protocol violation and hang up. Firmware and stratum proxies do send
 * them, so that was a pool dropping a working miner mid-session with no error
 * and no reason — indistinguishable, from the miner's side, from a pool that
 * drops hashrate at random. */
static void test_a_jsonrpc_response_does_not_close_the_connection(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);

    char *out = NULL; size_t olen = 0;
    for (int i = 0; i < 50; ++i) {
        int rc = stratum_handle_message(s, c,
            "{\"id\":1,\"result\":true,\"error\":null}", &out, &olen);
        CHECK(rc == 0);
        CHECK(out == NULL);   /* nothing to answer, so answer nothing */
    }

    /* A blank line is the only keepalive a miner has — the pool asks it
     * nothing between shares — so it is not an error either. */
    CHECK(stratum_handle_message(s, c, "", &out, &olen) == 0);
    CHECK(stratum_handle_message(s, c, "   \r", &out, &olen) == 0);
    CHECK(out == NULL);

    /* And an unimplemented method is answered, not punished. */
    int rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.suggest_difficulty\",\"params\":[512]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(out != NULL && strstr(out, "\"error\"") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Tolerance is not a licence to spew. A client that never manages a single
 * intelligible request is not a miner, and the streak counter is what lets
 * the pool cut it on evidence rather than on the first stray byte. */
static void test_persistent_garbage_still_closes_the_connection(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);

    char *out = NULL; size_t olen = 0;
    int closed_at = -1;
    for (int i = 1; i <= 20; ++i) {
        int rc = stratum_handle_message(s, c, "}{ not json", &out, &olen);
        free(out); out = NULL; olen = 0;
        if (rc < 0) { closed_at = i; break; }
    }
    CHECK(closed_at == 8);

    /* One good request in between clears the record: a miner that hiccups
     * once an hour must never accumulate its way to a disconnect. */
    stratum_conn_t *c2 = stratum_conn_new_for_test(s);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 7; ++i) {
            CHECK(stratum_handle_message(s, c2, "}{ not json", &out, &olen) == 0);
            free(out); out = NULL; olen = 0;
        }
        CHECK(stratum_handle_message(s, c2,
            "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
            &out, &olen) == 0);
        free(out); out = NULL; olen = 0;
    }

    stratum_conn_free_for_test(c);
    stratum_conn_free_for_test(c2);
    stratum_server_free(s);
}

/* An authorized miner with nothing to say is the normal resting state, not an
 * idle connection: the pool solicits nothing between shares, so a small rig
 * that has not cleared its assigned difficulty yet sends no bytes at all.
 * Reaping that on the unauthorized budget disconnects working hashrate. */
static void test_authorized_miner_gets_the_long_idle_budget(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("J1", net), 1);

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    /* Still a stranger: it has told us nothing and costs an fd for nothing. */
    CHECK(stratum_conn_idle_budget_for_test(s, c) == 600);

    handshake(s, c);
    CHECK(stratum_conn_authorized_for_test(c) == 1);
    CHECK(stratum_conn_idle_budget_for_test(s, c) == 7200);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);

    /* Negative means "never reap a working miner" and must not be confused
     * with unset, which means "use the default". Folding one into the other
     * would silently reap on the 600s budget the operator was disabling. */
    stratum_cfg_t off = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .idle_timeout_authorized_sec = -1 };
    snprintf(off.bind_addr, sizeof(off.bind_addr), "127.0.0.1");
    stratum_server_t *s2 = NULL;
    stratum_server_start(&off, &s2);
    uint8_t net2[32]; memset(net2, 0xff, 32);
    stratum_server_set_job(s2, make_test_job("J1", net2), 1);
    stratum_conn_t *c2 = stratum_conn_new_for_test(s2);
    CHECK(stratum_conn_idle_budget_for_test(s2, c2) == 600);
    handshake(s2, c2);
    CHECK(stratum_conn_idle_budget_for_test(s2, c2) == 0);
    stratum_conn_free_for_test(c2);
    stratum_server_free(s2);
}

/* A failed bind must not close the process's standard input.
 *
 * The listener slots live in a calloc'd stratum_server, so every slot's fd
 * starts at 0 -- a perfectly valid descriptor number, and on a normal process
 * it is stdin. The bind_failed teardown walks i < listener_count and closes
 * every slot whose fd is >= 0, but listener_count is set BEFORE the bind loop
 * runs. So when a bind fails partway, every slot the loop never reached is
 * still holding fd 0, and the teardown shuts down and closes descriptor 0.
 *
 * Three listeners are the minimum that shows it: slot 0 binds, slot 1 fails,
 * and slot 2 -- untouched, fd still 0 -- is what gets closed.
 *
 * The symptom in production is not a crash. It is a pool that failed to start
 * for an understandable reason (a port already in use) and, on the way out,
 * quietly closed stdin for whatever runs next in the same process image. */
static void test_failed_bind_does_not_close_stdin(void) {
    /* Occupy a port so a listener bound to it is guaranteed to fail. */
    int squatter = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(squatter >= 0);
    if (squatter < 0) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                       /* let the kernel choose */
    if (bind(squatter, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(squatter, 1) < 0) {
        close(squatter);
        CHECK(0 && "could not set up an occupied port");
        return;
    }
    socklen_t alen = sizeof a;
    CHECK(getsockname(squatter, (struct sockaddr *)&a, &alen) == 0);
    int taken = ntohs(a.sin_port);

    /* stdin must be open going in, or the test proves nothing. */
    CHECK(fcntl(0, F_GETFD) != -1);

    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 4, .initial_diff = 1.0 };
    snprintf(cfg.bind_addr, sizeof cfg.bind_addr, "127.0.0.1");
    cfg.listeners[0].port = taken;        /* fails: already in use */
    cfg.listeners[1].port = taken + 1;    /* never reached; fd still 0 */
    cfg.listener_count = 2;

    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) < 0);   /* the start must fail */
    CHECK(s == NULL);

    /* The point of the test: the failure must not have taken stdin with it. */
    CHECK(fcntl(0, F_GETFD) != -1);

    close(squatter);
    if (g_fail == 0) printf("ok: a failed bind leaves stdin alone\n");
}


/* ---- dual-stack listener ------------------------------------------------ */

/* Connect to a started server over a chosen family and return the fd, or -1.
 * Real sockets on purpose: what is under test is which families bind() and
 * accept() actually serve, and stratum_conn_new_for_test never goes through
 * accept() at all. */
static int dial(int family, int port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_storage ss;
    socklen_t len;
    memset(&ss, 0, sizeof ss);
    if (family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)(void *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)port);
        a->sin6_addr = in6addr_loopback;
        len = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)(void *)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)port);
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*a);
    }
    if (connect(fd, (struct sockaddr *)&ss, len) < 0) { close(fd); return -1; }
    return fd;
}

/* Walks a small port range rather than insisting on one number. A fixed port
 * makes this kind of test flaky: a back-to-back run can find the previous
 * process's socket still lingering, and the bind then fails for a reason that
 * has nothing to do with the code under test. Writes the port actually bound
 * back through *port. */
static stratum_server_t *start_on(const char *addr, int *port) {
    for (int p = *port; p < *port + 20; p++) {
        stratum_cfg_t cfg = { .bind_port = p, .max_conns = 8,
                              .initial_diff = 1.0, .vardiff_enabled = 0 };
        snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "%s", addr);
        stratum_server_t *s = NULL;
        if (stratum_server_start(&cfg, &s) != 0) continue;
        uint8_t net[32]; memset(net, 0xff, sizeof net);
        stratum_server_set_job(s, make_test_job("J1", net), 1);
        *port = p;
        return s;
    }
    return NULL;
}

/* The point of the change: with listen_addr = "::" an IPv6 client connects. */
static void test_dual_stack_accepts_ipv6(void) {
    int port = 39334;
    stratum_server_t *s = start_on("::", &port);
    if (!s) { printf("ok: dual-stack v6 skipped (no IPv6 on this host)\n"); return; }
    int fd = dial(AF_INET6, port);
    CHECK(fd >= 0);
    if (fd >= 0) {
        sleep_ms(150);
        close(fd);
        sleep_ms(150);
    }
    stratum_server_free(s);
    printf("ok: a dual-stack listener accepts an IPv6 client\n");
}

/* ...and IPv4 clients must keep working on the same socket, or turning this on
 * would silently strand every existing miner. */
static void test_dual_stack_still_accepts_ipv4(void) {
    int port = 39364;
    stratum_server_t *s = start_on("::", &port);
    if (!s) { printf("ok: dual-stack v4 skipped (no IPv6 on this host)\n"); return; }
    int fd = dial(AF_INET, port);
    CHECK(fd >= 0);
    if (fd >= 0) {
        sleep_ms(150);
        close(fd);
        sleep_ms(150);
    }
    stratum_server_free(s);
    printf("ok: a dual-stack listener still accepts an IPv4 client\n");
}

/* 🔴 The gate, and the reason the other two mean anything. The default must not
 * have changed: on "0.0.0.0" an IPv6 client is still refused. Without this an
 * accepted-everywhere result would be equally consistent with the config gate
 * having been ignored and every deployment silently becoming dual-stack. */
static void test_ipv4_default_still_refuses_ipv6(void) {
    int port = 39394;
    stratum_server_t *s = start_on("0.0.0.0", &port);
    CHECK(s != NULL);
    if (!s) return;
    int fd = dial(AF_INET6, port);
    CHECK(fd < 0);
    if (fd >= 0) close(fd);
    stratum_server_free(s);
    printf("ok: the IPv4 default still refuses an IPv6 client\n");
}

int main(void) {
    test_failed_bind_does_not_close_stdin();
    test_subscribe();
    test_authorize_triggers_setdiff_notify();
    test_submit_unknown_job();
    test_submit_share_and_dedupe();
    test_submit_rejects_wrong_extranonce2_size();
    test_accept_churn_peer_gone();
    test_job_rotation_races_submits();
    test_authorize_rejects_non_address();
    test_authorize_address_with_label();
    test_block_wins_over_low_difficulty();
    test_vardiff_tracks_miner_local_floor();
    test_vardiff_still_lowers_for_a_matched_miner();
    test_vardiff_clamped_to_network_diff();
    test_submit_ceiling_refuses_past_the_limit();
    test_submit_ceiling_window_rolls();
    test_submit_ceiling_zero_disables();
    test_listener_policy_sets_the_connections_difficulty();
    test_listener_policy_falls_back_per_field();
    test_listener_difficulty_still_clamped_to_the_network();
    test_submit_judged_at_the_jobs_own_difficulty();
    test_job_difficulty_survives_repeated_retargets();
    test_job_notified_after_a_retarget_uses_the_new_difficulty();
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
    test_clean_jobs_only_on_a_new_tip();
    test_promised_min_diff_survives_the_network_clamp();
    test_vardiff_cannot_retarget_below_a_promised_floor();
    test_ntime_rolling_is_accepted();
    test_ntime_outside_the_window_is_rejected();
    test_a_jsonrpc_response_does_not_close_the_connection();
    test_persistent_garbage_still_closes_the_connection();
    test_authorized_miner_gets_the_long_idle_budget();
    test_dual_stack_accepts_ipv6();
    test_dual_stack_still_accepts_ipv4();
    test_ipv4_default_still_refuses_ipv6();
    printf("test_stratum: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
