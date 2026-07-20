#ifndef SIMPLEPOOL_BROADCAST_H
#define SIMPLEPOOL_BROADCAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Best-effort Redis pub/sub broadcaster for pool events.
 *
 * SQLite remains the source of truth. This module is fire-and-forget: a
 * background thread drains a bounded ring queue, JSON-encodes each event,
 * and PUBLISHes it on a per-event-type channel. If Redis is down or the
 * queue overflows the event is dropped — the stratum and writer threads
 * are never blocked by Redis I/O.
 *
 * Channels:
 *   pool:shares   — accepted shares
 *   pool:rejects  — rejected submissions
 *   pool:blocks   — solved blocks (after the share that solved them)
 *   pool:tip      — upstream bitcoind tip changes
 *   pool:credits  — PPS accrual (only emitted in PPS mode, see TODO)
 *
 * Payload schema matches the corresponding SQLite row shape (snake_case
 * field names, sats as integers, hashes as lowercase hex strings).
 */

typedef struct broadcast broadcast_t;

typedef struct {
    /* Redis connection URL: "redis://host:port[/db]" or "host:port".
     * Empty string disables the module — broadcast_open succeeds and
     * every publish call is a no-op. */
    char url[256];

    /* Per-publish timeout. Default 200ms. */
    int publish_timeout_ms;

    /* Backoff before retrying a broken connection. Default 2000ms. */
    int reconnect_backoff_ms;

    /* Ring queue depth (events). Default 4096. */
    size_t queue_capacity;
} broadcast_cfg_t;

/* Open the broadcaster. If cfg->url is empty, returns a disabled instance
 * (still safe to call publish methods on). Otherwise starts a background
 * thread that connects to Redis with backoff. Returns 0 ok, negative on
 * config error. */
int broadcast_open(const broadcast_cfg_t *cfg, broadcast_t **out);

/* Stop the publisher thread (drains queue best-effort), disconnect, free. */
void broadcast_close(broadcast_t *b);

/* Fire-and-forget. Each is safe from any thread; never blocks longer than
 * the queue mutex (microseconds). */
void broadcast_share(broadcast_t *b,
                     const char *worker_name,
                     const char *payout_address,
                     uint64_t ts_ms, double difficulty,
                     int is_block, const char *share_hash_or_null);

void broadcast_reject(broadcast_t *b,
                      const char *worker_name,
                      uint64_t ts_ms, const char *reason);

void broadcast_block(broadcast_t *b,
                     const char *worker_name,
                     const char *finder_address,
                     uint64_t ts_ms, uint32_t height,
                     const char *block_hash,
                     int64_t reward_sats, int64_t fee_sats);

void broadcast_node_tip(broadcast_t *b,
                        int height, const char *tip_hash,
                        uint64_t observed_at_s);

/* PPS credit event — accrued sats added to a worker after an accepted
 * share. Only fires when pool_mode = pps. */
void broadcast_credit(broadcast_t *b,
                      const char *worker_name,
                      uint64_t ts_ms,
                      int64_t delta_sats,
                      int64_t accrued_total_sats);

/* Read-only stats for /metrics. */
typedef struct {
    uint64_t enqueued;
    uint64_t published;
    uint64_t dropped_queue_full;
    uint64_t dropped_redis_down;
    uint64_t reconnects;
} broadcast_stats_t;
void broadcast_get_stats(broadcast_t *b, broadcast_stats_t *out);

#endif /* SIMPLEPOOL_BROADCAST_H */
