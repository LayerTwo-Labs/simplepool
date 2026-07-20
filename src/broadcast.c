/* Best-effort Redis pub/sub broadcaster.
 *
 * One background thread owns the redisContext. Producers enqueue
 * already-formatted (channel, payload) pairs into a bounded ring queue;
 * the thread drains the queue and PUBLISHes each entry. On error it
 * disconnects, backs off, and reconnects. Producers never touch hiredis. */

#define _POSIX_C_SOURCE 200809L
#include "broadcast.h"
#include "log.h"

#include <hiredis/hiredis.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define CHANNEL_MAX 32
#define PAYLOAD_MAX 1024

typedef struct {
    char channel[CHANNEL_MAX];
    char payload[PAYLOAD_MAX];
} bc_event_t;

struct broadcast {
    /* Config snapshot. */
    char     url[256];
    int      publish_timeout_ms;
    int      reconnect_backoff_ms;
    size_t   queue_capacity;
    int      enabled;            /* 0 when url is empty */

    /* Ring queue. */
    bc_event_t      *ring;
    size_t           head, tail;   /* head = read, tail = write */
    pthread_mutex_t  qmu;
    pthread_cond_t   qcv;
    int              shutdown;

    /* Background thread. */
    pthread_t worker;
    int       worker_started;

    /* Stats (atomic-ish; only the worker writes published/reconnects). */
    _Atomic uint64_t enqueued;
    _Atomic uint64_t published;
    _Atomic uint64_t dropped_queue_full;
    _Atomic uint64_t dropped_redis_down;
    _Atomic uint64_t reconnects;
};

/* ---------- url parsing ---------- */

/* Accept "redis://[user[:pass]@]host[:port][/db]" or "host:port". On
 * success fills host/port/db_index; user/pass ignored for now (the
 * current pool deployment uses unauthenticated redis on a private vlan).
 * Returns 0 ok. */
static int parse_url(const char *url, char *host, size_t host_cap,
                     int *port, int *db_index) {
    *port = 6379;
    *db_index = 0;
    if (!url || !*url) return -1;

    const char *p = url;
    if (strncmp(p, "redis://", 8) == 0) p += 8;

    /* Skip user[:pass]@ if present. */
    const char *at = strchr(p, '@');
    if (at) p = at + 1;

    /* Split host[:port][/db]. */
    const char *slash = strchr(p, '/');
    const char *colon = NULL;
    /* find LAST ':' before the slash (or end), in case host is ipv6
     * notation we don't bother supporting — pool deploys use ipv4. */
    for (const char *q = p; *q && (!slash || q < slash); q++) {
        if (*q == ':') colon = q;
    }

    size_t hlen;
    if (colon) {
        hlen = (size_t)(colon - p);
        long pv = strtol(colon + 1, NULL, 10);
        if (pv > 0 && pv < 65536) *port = (int)pv;
    } else {
        hlen = slash ? (size_t)(slash - p) : strlen(p);
    }
    if (hlen == 0 || hlen + 1 > host_cap) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (slash) {
        long dv = strtol(slash + 1, NULL, 10);
        if (dv >= 0 && dv < 16) *db_index = (int)dv;
    }
    return 0;
}

/* ---------- queue ---------- */

static int q_push(broadcast_t *b, const char *channel, const char *payload) {
    if (!b->enabled || b->shutdown) return -1;
    pthread_mutex_lock(&b->qmu);
    size_t next = (b->tail + 1) % b->queue_capacity;
    if (next == b->head) {
        pthread_mutex_unlock(&b->qmu);
        atomic_fetch_add_explicit(&b->dropped_queue_full, 1, memory_order_relaxed);
        return -1;
    }
    snprintf(b->ring[b->tail].channel, CHANNEL_MAX, "%s", channel);
    snprintf(b->ring[b->tail].payload, PAYLOAD_MAX, "%s", payload);
    b->tail = next;
    pthread_cond_signal(&b->qcv);
    pthread_mutex_unlock(&b->qmu);
    atomic_fetch_add_explicit(&b->enqueued, 1, memory_order_relaxed);
    return 0;
}

/* Pop with a timeout. Returns 1 ok, 0 timeout, -1 shutdown+empty. */
static int q_pop(broadcast_t *b, bc_event_t *out, int timeout_ms) {
    pthread_mutex_lock(&b->qmu);
    while (b->head == b->tail && !b->shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = pthread_cond_timedwait(&b->qcv, &b->qmu, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&b->qmu);
            return 0;
        }
    }
    if (b->head == b->tail && b->shutdown) {
        pthread_mutex_unlock(&b->qmu);
        return -1;
    }
    *out = b->ring[b->head];
    b->head = (b->head + 1) % b->queue_capacity;
    pthread_mutex_unlock(&b->qmu);
    return 1;
}

/* ---------- worker ---------- */

static redisContext *open_redis(const char *url, int publish_timeout_ms) {
    char host[128];
    int  port = 6379, db_index = 0;
    if (parse_url(url, host, sizeof host, &port, &db_index) < 0) {
        LOG_ERROR("broadcast: bad redis url '%s'", url);
        return NULL;
    }
    struct timeval tv = {
        .tv_sec  = publish_timeout_ms / 1000,
        .tv_usec = (publish_timeout_ms % 1000) * 1000,
    };
    redisContext *c = redisConnectWithTimeout(host, port, tv);
    if (!c || c->err) {
        if (c) {
            LOG_WARN("broadcast: redis connect %s:%d failed: %s",
                     host, port, c->errstr);
            redisFree(c);
        } else {
            LOG_WARN("broadcast: redis connect %s:%d: OOM", host, port);
        }
        return NULL;
    }
    redisSetTimeout(c, tv);
    if (db_index > 0) {
        redisReply *r = redisCommand(c, "SELECT %d", db_index);
        if (!r || c->err) {
            LOG_WARN("broadcast: SELECT %d failed: %s", db_index, c->errstr);
            if (r) freeReplyObject(r);
            redisFree(c);
            return NULL;
        }
        freeReplyObject(r);
    }
    LOG_INFO("broadcast: connected to redis %s:%d/%d", host, port, db_index);
    return c;
}

static void *worker_main(void *arg) {
    broadcast_t *b = (broadcast_t *)arg;
    redisContext *rc = NULL;
    uint64_t next_attempt_ms = 0;

    while (1) {
        bc_event_t ev;
        int got = q_pop(b, &ev, 500);
        if (got < 0) break;            /* shutdown + drained */
        if (got == 0) continue;        /* timeout — recheck shutdown */

        /* (Re)connect with backoff. */
        if (!rc) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            if (now_ms < next_attempt_ms) {
                atomic_fetch_add_explicit(&b->dropped_redis_down, 1,
                                          memory_order_relaxed);
                continue;
            }
            rc = open_redis(b->url, b->publish_timeout_ms);
            if (!rc) {
                next_attempt_ms = now_ms + (uint64_t)b->reconnect_backoff_ms;
                atomic_fetch_add_explicit(&b->dropped_redis_down, 1,
                                          memory_order_relaxed);
                continue;
            }
            atomic_fetch_add_explicit(&b->reconnects, 1, memory_order_relaxed);
        }

        redisReply *reply = redisCommand(rc, "PUBLISH %s %s",
                                         ev.channel, ev.payload);
        if (!reply || rc->err) {
            LOG_WARN("broadcast: PUBLISH failed (%s); reconnecting",
                     rc->errstr);
            if (reply) freeReplyObject(reply);
            redisFree(rc);
            rc = NULL;
            atomic_fetch_add_explicit(&b->dropped_redis_down, 1,
                                      memory_order_relaxed);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            next_attempt_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000
                              + (uint64_t)b->reconnect_backoff_ms;
            continue;
        }
        freeReplyObject(reply);
        atomic_fetch_add_explicit(&b->published, 1, memory_order_relaxed);
    }
    if (rc) redisFree(rc);
    return NULL;
}

/* ---------- open / close ---------- */

int broadcast_open(const broadcast_cfg_t *cfg, broadcast_t **out) {
    if (!cfg || !out) return -1;
    broadcast_t *b = (broadcast_t *)calloc(1, sizeof *b);
    if (!b) return -1;

    b->publish_timeout_ms   = cfg->publish_timeout_ms   > 0 ? cfg->publish_timeout_ms   : 200;
    b->reconnect_backoff_ms = cfg->reconnect_backoff_ms > 0 ? cfg->reconnect_backoff_ms : 2000;
    b->queue_capacity       = cfg->queue_capacity       > 0 ? cfg->queue_capacity       : 4096;
    snprintf(b->url, sizeof b->url, "%s", cfg->url);

    pthread_mutex_init(&b->qmu, NULL);
    pthread_cond_init(&b->qcv, NULL);

    if (b->url[0] == '\0') {
        b->enabled = 0;
        LOG_INFO("broadcast: disabled (no redis_url configured)");
        *out = b;
        return 0;
    }
    b->enabled = 1;

    b->ring = (bc_event_t *)calloc(b->queue_capacity, sizeof(bc_event_t));
    if (!b->ring) {
        pthread_mutex_destroy(&b->qmu);
        pthread_cond_destroy(&b->qcv);
        free(b);
        return -1;
    }

    if (pthread_create(&b->worker, NULL, worker_main, b) != 0) {
        free(b->ring);
        pthread_mutex_destroy(&b->qmu);
        pthread_cond_destroy(&b->qcv);
        free(b);
        return -1;
    }
    b->worker_started = 1;
    *out = b;
    return 0;
}

void broadcast_close(broadcast_t *b) {
    if (!b) return;
    if (b->worker_started) {
        pthread_mutex_lock(&b->qmu);
        b->shutdown = 1;
        pthread_cond_broadcast(&b->qcv);
        pthread_mutex_unlock(&b->qmu);
        pthread_join(b->worker, NULL);
    }
    pthread_mutex_destroy(&b->qmu);
    pthread_cond_destroy(&b->qcv);
    free(b->ring);
    free(b);
}

void broadcast_get_stats(broadcast_t *b, broadcast_stats_t *out) {
    if (!b || !out) return;
    out->enqueued           = atomic_load_explicit(&b->enqueued, memory_order_relaxed);
    out->published          = atomic_load_explicit(&b->published, memory_order_relaxed);
    out->dropped_queue_full = atomic_load_explicit(&b->dropped_queue_full, memory_order_relaxed);
    out->dropped_redis_down = atomic_load_explicit(&b->dropped_redis_down, memory_order_relaxed);
    out->reconnects         = atomic_load_explicit(&b->reconnects, memory_order_relaxed);
}

/* ---------- JSON helpers ---------- */

/* Escape `s` into out, capping at cap-1 bytes. Returns bytes written. We
 * deliberately handle only the JSON-mandatory escapes; payouts/worker
 * names are sanitized upstream (alphanum + . _ -). */
static size_t json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (!s) { if (cap >= 5) memcpy(out, "null", 5); return cap >= 5 ? 4 : 0; }
    if (cap == 0) return 0;
    out[o++] = '"';
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (o + 7 >= cap) break;       /* leave room for \uXXXX + close */
        if (c == '"' || c == '\\') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    if (o + 1 < cap) out[o++] = '"';
    out[o] = '\0';
    return o;
}

/* ---------- per-event publish ---------- */

void broadcast_share(broadcast_t *b,
                     const char *worker_name,
                     const char *payout_address,
                     uint64_t ts_ms, double difficulty,
                     int is_block, const char *share_hash_or_null) {
    if (!b || !b->enabled) return;
    char w[160], a[160], h[160];
    json_escape(worker_name, w, sizeof w);
    json_escape(payout_address, a, sizeof a);
    json_escape(share_hash_or_null, h, sizeof h);
    char payload[PAYLOAD_MAX];
    snprintf(payload, sizeof payload,
             "{\"worker\":%s,\"payout_address\":%s,\"ts_ms\":%llu,"
             "\"difficulty\":%.6f,\"is_block\":%d,\"share_hash\":%s}",
             w, a, (unsigned long long)ts_ms, difficulty, is_block, h);
    q_push(b, "pool:shares", payload);
}

void broadcast_reject(broadcast_t *b,
                      const char *worker_name,
                      uint64_t ts_ms, const char *reason) {
    if (!b || !b->enabled) return;
    char w[160], r[300];
    json_escape(worker_name, w, sizeof w);
    json_escape(reason, r, sizeof r);
    char payload[PAYLOAD_MAX];
    snprintf(payload, sizeof payload,
             "{\"worker\":%s,\"ts_ms\":%llu,\"reason\":%s}",
             w, (unsigned long long)ts_ms, r);
    q_push(b, "pool:rejects", payload);
}

void broadcast_block(broadcast_t *b,
                     const char *worker_name,
                     const char *finder_address,
                     uint64_t ts_ms, uint32_t height,
                     const char *block_hash,
                     int64_t reward_sats, int64_t fee_sats) {
    if (!b || !b->enabled) return;
    char w[160], a[160], h[160];
    json_escape(worker_name, w, sizeof w);
    json_escape(finder_address, a, sizeof a);
    json_escape(block_hash, h, sizeof h);
    char payload[PAYLOAD_MAX];
    snprintf(payload, sizeof payload,
             "{\"worker\":%s,\"finder_address\":%s,\"ts_ms\":%llu,"
             "\"height\":%u,\"hash\":%s,\"reward_sats\":%lld,\"fee_sats\":%lld}",
             w, a, (unsigned long long)ts_ms, height, h,
             (long long)reward_sats, (long long)fee_sats);
    q_push(b, "pool:blocks", payload);
}

void broadcast_node_tip(broadcast_t *b,
                        int height, const char *tip_hash,
                        uint64_t observed_at_s) {
    if (!b || !b->enabled) return;
    char h[160];
    json_escape(tip_hash, h, sizeof h);
    char payload[PAYLOAD_MAX];
    snprintf(payload, sizeof payload,
             "{\"height\":%d,\"hash\":%s,\"observed_at_s\":%llu}",
             height, h, (unsigned long long)observed_at_s);
    q_push(b, "pool:tip", payload);
}

void broadcast_credit(broadcast_t *b,
                      const char *worker_name,
                      uint64_t ts_ms,
                      int64_t delta_sats,
                      int64_t accrued_total_sats) {
    if (!b || !b->enabled) return;
    char w[160];
    json_escape(worker_name, w, sizeof w);
    char payload[PAYLOAD_MAX];
    snprintf(payload, sizeof payload,
             "{\"worker\":%s,\"ts_ms\":%llu,\"delta_sats\":%lld,"
             "\"accrued_total_sats\":%lld}",
             w, (unsigned long long)ts_ms,
             (long long)delta_sats, (long long)accrued_total_sats);
    q_push(b, "pool:credits", payload);
}
