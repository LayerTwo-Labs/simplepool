#define _POSIX_C_SOURCE 200809L
#include "bitcoind.h"
#include "coinbase.h"
#include "config.h"
#include "log.h"
#include "share.h"
#include "store.h"
#include "stratum.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

/* ---------- helpers ---------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* hex (big-endian display order) -> bytes (display order). */
static int hex_to_bytes_display(const char *hex, uint8_t *out, size_t expected) {
    size_t n = strlen(hex);
    if (n != expected * 2) return -1;
    for (size_t i = 0; i < expected; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* Reverse 32 bytes in-place. */
static void rev32(uint8_t b[32]) {
    for (int i = 0; i < 16; i++) {
        uint8_t t = b[i];
        b[i] = b[31 - i];
        b[31 - i] = t;
    }
}

/* Compute merkle branches for index 0 over [coinbase_placeholder, ...txids_le].
 * branches_out must hold up to tx_count entries. Returns number of branches. */
static size_t compute_merkle_branches_for_idx0(const uint8_t (*txids_le)[32],
                                                size_t tx_count,
                                                uint8_t (*branches_out)[32]) {
    /* Branches: at each level, the sibling of node 0. The leaf-level sibling
     * is txids_le[0] (the first non-coinbase tx) — i.e. branches don't depend
     * on the coinbase content. We can compute by carrying a placeholder leaf
     * (zeros) and recording the sibling at each level. */
    size_t n = tx_count + 1;
    if (n == 1) return 0;

    /* Working buffer with placeholder at idx 0, then txids. */
    uint8_t (*level)[32] = (uint8_t (*)[32])calloc(n, 32);
    if (!level) return 0;
    /* level[0] = zeros (placeholder). */
    for (size_t i = 0; i < tx_count; i++) {
        memcpy(level[i + 1], txids_le[i], 32);
    }

    size_t branch_count = 0;
    while (n > 1) {
        memcpy(branches_out[branch_count++], level[1], 32);

        /* Build next level. */
        size_t new_n = (n + 1) / 2;
        for (size_t i = 0; i < new_n; i++) {
            uint8_t pair[64];
            memcpy(pair, level[2 * i], 32);
            if (2 * i + 1 < n) {
                memcpy(pair + 32, level[2 * i + 1], 32);
            } else {
                memcpy(pair + 32, level[2 * i], 32);
            }
            uint8_t h[32];
            dsha256(pair, 64, h);
            memcpy(level[i], h, 32);
        }
        n = new_n;
    }
    free(level);
    return branch_count;
}

/* ---------- shared server state ---------- */

typedef struct {
    bitcoind_client_t *btc;
    store_t           *store;
    stratum_server_t  *srv;
    proxy_config_t    *cfg;

    pthread_mutex_t lock;
    int             last_height;
    char            last_prev_hash[65];
    uint64_t        last_built_ms;
} server_ctx_t;

/* Build a job from a freshly fetched template. The coinbase is rendered
 * per-connection inside stratum.c (each miner pays their own address),
 * so we only pass template-level data here. */
static stratum_job_t *build_job_from_template(const proxy_config_t *cfg,
                                              const bitcoind_template_t *t,
                                              char *errbuf, size_t errlen) {
    (void)cfg;
    /* Convert tx txids: hex (display BE) -> internal LE. */
    uint8_t (*txids_le)[32] = NULL;
    char **tx_hex_list = NULL;
    if (t->tx_count > 0) {
        txids_le = (uint8_t (*)[32])calloc(t->tx_count, 32);
        tx_hex_list = (char **)calloc(t->tx_count, sizeof(char *));
        if (!txids_le || !tx_hex_list) {
            snprintf(errbuf, errlen, "oom");
            free(txids_le); free(tx_hex_list);
            return NULL;
        }
        for (size_t i = 0; i < t->tx_count; i++) {
            uint8_t be[32];
            if (hex_to_bytes_display(t->txs[i].txid_hex, be, 32) < 0) {
                snprintf(errbuf, errlen, "bad txid hex at %zu", i);
                free(txids_le);
                for (size_t j = 0; j < i; j++) free(tx_hex_list[j]);
                free(tx_hex_list);
                return NULL;
            }
            memcpy(txids_le[i], be, 32);
            rev32(txids_le[i]);
            tx_hex_list[i] = strdup(t->txs[i].data_hex ? t->txs[i].data_hex : "");
        }
    }

    /* Branches. */
    uint8_t (*branches)[32] = NULL;
    size_t branch_count = 0;
    if (t->tx_count > 0) {
        branches = (uint8_t (*)[32])calloc(t->tx_count + 1, 32);
        if (!branches) {
            snprintf(errbuf, errlen, "oom branches");
            free(txids_le);
            for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
            free(tx_hex_list);
            return NULL;
        }
        branch_count = compute_merkle_branches_for_idx0(
            (const uint8_t (*)[32])txids_le, t->tx_count, branches);
    }

    /* prev_hash: GBT gives BE display hex; header wants natural LE bytes. */
    uint8_t prev_le[32] = {0};
    if (hex_to_bytes_display(t->prev_hash_hex, prev_le, 32) < 0) {
        snprintf(errbuf, errlen, "bad prev hash hex");
        free(branches); free(txids_le);
        for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
        free(tx_hex_list);
        return NULL;
    }
    rev32(prev_le);

    uint8_t target_be[32] = {0};
    /* If GBT supplies target hex, use it; else derive from nbits. */
    if (t->target_hex[0] != '\0' && strlen(t->target_hex) == 64) {
        if (hex_to_bytes_display(t->target_hex, target_be, 32) < 0) {
            nbits_to_target(t->bits, target_be);
        }
    } else {
        nbits_to_target(t->bits, target_be);
    }

    char job_id[32];
    snprintf(job_id, sizeof job_id, "%llx", (unsigned long long)now_ms());

    stratum_job_t *job = stratum_job_new(
        job_id, t->version, prev_le,
        t->coinbase_value_sats,
        t->default_witness_commitment,
        /*en1*/ 4, /*en2*/ 4,
        (const uint8_t (*)[32])branches, branch_count,
        t->bits, t->curtime, target_be,
        (uint32_t)t->height,
        (const char *const *)tx_hex_list, t->tx_count);

    /* stratum_job_new copies; free our originals. */
    free(branches);
    free(txids_le);
    if (tx_hex_list) {
        for (size_t j = 0; j < t->tx_count; j++) free(tx_hex_list[j]);
        free(tx_hex_list);
    }
    if (!job) {
        snprintf(errbuf, errlen, "stratum_job_new failed");
        return NULL;
    }
    return job;
}

/* ---------- observer hooks ---------- */

static void on_share_cb(void *ctx, const char *worker_name,
                        const char *payout_address, uint64_t ts_ms,
                        double difficulty, int is_block,
                        const char *block_hash_or_null) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_share_addr(s->store, worker_name, payout_address,
                                ts_ms, difficulty, is_block,
                                block_hash_or_null);
    }
}

static void on_reject_cb(void *ctx, const char *worker_name, uint64_t ts_ms,
                         const char *reason) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_reject(s->store, worker_name, ts_ms, reason);
    }
}

static void on_block_cb(void *ctx, const char *block_hex) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (!s || !s->btc) return;
    char err[512] = {0};
    int rc = bitcoind_submit_block(s->btc, block_hex, err, sizeof err);
    if (rc == 0) {
        LOG_INFO("submitted block to bitcoind successfully");
    } else {
        LOG_ERROR("submitblock failed: %s", err);
    }
}

static void on_block_found_cb(void *ctx, const char *worker_name,
                              const char *finder_address,
                              uint64_t ts_ms, uint32_t height,
                              const char *block_hash,
                              int64_t reward_sats, int64_t fee_sats) {
    server_ctx_t *s = (server_ctx_t *)ctx;
    if (s && s->store) {
        store_record_block(s->store, ts_ms, (int)height, block_hash,
                           worker_name, finder_address,
                           reward_sats, fee_sats);
    }
    LOG_INFO("BLOCK FOUND: height=%u finder=%s reward=%lld fee=%lld hash=%s",
             height, worker_name ? worker_name : "?",
             (long long)reward_sats, (long long)fee_sats,
             block_hash ? block_hash : "?");
}

/* ---------- tip watcher ---------- */

static void *tip_watcher(void *arg) {
    server_ctx_t *s = (server_ctx_t *)arg;
    while (!g_shutdown) {
        struct timespec ts;
        ts.tv_sec  = s->cfg->bitcoind_poll_interval_ms / 1000;
        ts.tv_nsec = (long)(s->cfg->bitcoind_poll_interval_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        if (g_shutdown) break;

        char err[512] = {0};
        bitcoind_template_t *t = NULL;
        if (bitcoind_get_block_template(s->btc, &t, err, sizeof err) < 0) {
            LOG_WARN("getblocktemplate poll failed: %s", err);
            continue;
        }

        int need_rebuild = 0;
        pthread_mutex_lock(&s->lock);
        if (t->height != s->last_height ||
            strcmp(t->prev_hash_hex, s->last_prev_hash) != 0) {
            need_rebuild = 1;
        } else if (now_ms() - s->last_built_ms > 30000) {
            /* Periodic refresh for new ntime + included txs. */
            need_rebuild = 1;
        }
        pthread_mutex_unlock(&s->lock);

        if (need_rebuild) {
            char berr[256] = {0};
            stratum_job_t *job = build_job_from_template(s->cfg, t, berr, sizeof berr);
            if (!job) {
                LOG_ERROR("rebuild job failed: %s", berr);
                bitcoind_template_free(t);
                continue;
            }
            stratum_server_set_job(s->srv, job);
            pthread_mutex_lock(&s->lock);
            s->last_height = t->height;
            snprintf(s->last_prev_hash, sizeof s->last_prev_hash, "%s",
                     t->prev_hash_hex);
            s->last_built_ms = now_ms();
            pthread_mutex_unlock(&s->lock);
            LOG_INFO("new job: height=%d prev=%.16s... txs=%zu",
                     t->height, t->prev_hash_hex, t->tx_count);
        }
        bitcoind_template_free(t);
    }
    return NULL;
}

/* ---------- usage ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [config_path]\n"
            "  config_path  path to proxy.conf (default ./proxy.conf)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *cfg_path = "./proxy.conf";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        cfg_path = argv[1];
    }

    /* Load config. */
    proxy_config_t cfg;
    char err[512] = {0};
    if (proxy_config_load(cfg_path, &cfg, err, sizeof err) < 0) {
        fprintf(stderr, "config error: %s\n", err);
        return 2;
    }
    /* Fail fast on a misconfigured operator_address — otherwise every
     * coinbase render at runtime would warn and drop the job. This catches
     * the proxy.conf.example placeholder ("bcrt1q...") and any typo. */
    {
        uint8_t op_spk[64];
        size_t  op_spk_len = sizeof op_spk;
        char    op_err[256] = {0};
        if (coinbase_address_to_script(cfg.operator_address, op_spk,
                                       sizeof op_spk, &op_spk_len,
                                       op_err, sizeof op_err) < 0) {
            fprintf(stderr,
                    "config error: invalid operator_address '%s': %s\n"
                    "  set operator_address in %s to a real bitcoin "
                    "address (e.g. bc1q... on mainnet)\n",
                    cfg.operator_address, op_err, cfg_path);
            return 2;
        }
    }
    log_init(cfg.log_level);
    LOG_INFO("simplepool starting (config=%s)", cfg_path);

    /* bitcoind client. */
    bitcoind_client_t btc = {0};
    bitcoind_cfg_t bcfg = {0};
    snprintf(bcfg.url,  sizeof bcfg.url,  "%s", cfg.bitcoind_url);
    snprintf(bcfg.user, sizeof bcfg.user, "%s", cfg.bitcoind_user);
    snprintf(bcfg.pass, sizeof bcfg.pass, "%s", cfg.bitcoind_pass);
    bcfg.timeout_ms = 10000;
    if (bitcoind_client_init(&btc, &bcfg) < 0) {
        fprintf(stderr, "bitcoind_client_init failed\n");
        return 3;
    }
    if (bitcoind_ping(&btc, err, sizeof err) < 0) {
        fprintf(stderr, "bitcoind ping failed: %s\n", err);
        bitcoind_client_free(&btc);
        return 3;
    }
    LOG_INFO("bitcoind ping ok");

    /* Store. */
    store_cfg_t scfg = {0};
    snprintf(scfg.path, sizeof scfg.path, "%s", cfg.db_path);
    scfg.commit_window_ms  = cfg.commit_window_ms;
    scfg.commit_max_shares = cfg.commit_max_shares;
    store_t *store = NULL;
    if (store_open(&scfg, &store) < 0) {
        fprintf(stderr, "store_open failed for %s\n", cfg.db_path);
        bitcoind_client_free(&btc);
        return 4;
    }

    /* Initial template + job. */
    bitcoind_template_t *tmpl = NULL;
    if (bitcoind_get_block_template(&btc, &tmpl, err, sizeof err) < 0) {
        fprintf(stderr, "initial GBT failed: %s\n", err);
        store_close(store);
        bitcoind_client_free(&btc);
        return 5;
    }

    stratum_job_t *initial_job = build_job_from_template(&cfg, tmpl, err, sizeof err);
    if (!initial_job) {
        fprintf(stderr, "build initial job failed: %s\n", err);
        bitcoind_template_free(tmpl);
        store_close(store);
        bitcoind_client_free(&btc);
        return 6;
    }

    /* Server context (must outlive callbacks). */
    server_ctx_t sctx;
    memset(&sctx, 0, sizeof sctx);
    pthread_mutex_init(&sctx.lock, NULL);
    sctx.btc   = &btc;
    sctx.store = store;
    sctx.cfg   = &cfg;
    sctx.last_height = tmpl->height;
    snprintf(sctx.last_prev_hash, sizeof sctx.last_prev_hash, "%s", tmpl->prev_hash_hex);
    sctx.last_built_ms = now_ms();

    /* Start stratum server. */
    stratum_cfg_t stcfg;
    memset(&stcfg, 0, sizeof stcfg);
    snprintf(stcfg.bind_addr, sizeof stcfg.bind_addr, "%s", cfg.listen_addr);
    stcfg.bind_port    = cfg.listen_port;
    stcfg.max_conns    = cfg.max_conns;
    stcfg.initial_diff = cfg.initial_diff;
    snprintf(stcfg.operator_address, sizeof stcfg.operator_address, "%s",
             cfg.operator_address);
    stcfg.fee_bps      = cfg.fee_bps;
    snprintf(stcfg.coinbase_tag, sizeof stcfg.coinbase_tag, "%s",
             cfg.coinbase_tag);
    stcfg.ctx            = &sctx;
    stcfg.on_share       = on_share_cb;
    stcfg.on_reject      = on_reject_cb;
    stcfg.on_block       = on_block_cb;
    stcfg.on_block_found = on_block_found_cb;

    stratum_server_t *srv = NULL;
    if (stratum_server_start(&stcfg, &srv) < 0) {
        fprintf(stderr, "stratum_server_start failed\n");
        stratum_job_free(initial_job);
        bitcoind_template_free(tmpl);
        store_close(store);
        bitcoind_client_free(&btc);
        return 7;
    }
    sctx.srv = srv;
    stratum_server_set_job(srv, initial_job);
    bitcoind_template_free(tmpl);

    LOG_INFO("stratum listening on %s:%d", cfg.listen_addr, cfg.listen_port);

    /* Signals. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Tip watcher thread. */
    pthread_t watcher;
    pthread_create(&watcher, NULL, tip_watcher, &sctx);

    /* Main loop: wait for shutdown. */
    while (!g_shutdown) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    LOG_INFO("shutdown requested");

    pthread_join(watcher, NULL);

    stratum_server_stop(srv);
    stratum_server_free(srv);

    store_flush(store);
    store_stats_t stats;
    store_get_stats(store, &stats);
    LOG_INFO("final stats: shares_committed=%llu rejects_committed=%llu blocks=%llu sqlite_errs=%llu",
             (unsigned long long)stats.shares_committed,
             (unsigned long long)stats.rejects_committed,
             (unsigned long long)stats.blocks_committed,
             (unsigned long long)stats.pg_errors);
    store_close(store);

    bitcoind_client_free(&btc);
    pthread_mutex_destroy(&sctx.lock);

    LOG_INFO("simplepool exited cleanly");
    return 0;
}
