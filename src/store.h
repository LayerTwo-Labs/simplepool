#ifndef SIMPLEPOOL_STORE_H
#define SIMPLEPOOL_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct store store_t;

typedef struct {
    char path[512];
    int  commit_window_ms;     /* default 100 */
    int  commit_max_shares;    /* default 100 */
} store_cfg_t;

/* Open the DB (creates file + applies schema if missing). Starts a writer
 * thread. Returns 0 ok, negative on error. */
int store_open(const store_cfg_t *cfg, store_t **out);

/* Stop the writer thread (drains queue, commits), close DB, free. */
void store_close(store_t *s);

/* Record an accepted share. Thread-safe. Returns immediately - the actual
 * INSERT is batched on the writer thread. Returns 0 if queued, negative if
 * the queue is full (caller may log/drop).
 *
 * share_hash_or_null is the SHA256 of the share's block header in big-endian
 * hex. When is_block=1 it is also the block hash. For older callers / tests
 * that still pass NULL the row is stored with NULL in the hash column. */
int store_record_share(store_t *s, const char *worker_name,
                       uint64_t ts_ms, double difficulty,
                       int is_block, const char *share_hash_or_null);

/* Record a rejected share. */
int store_record_reject(store_t *s, const char *worker_name,
                        uint64_t ts_ms, const char *reason);

/* Record a block found. Thread-safe.
 * finder_address may be NULL (legacy callers); reward_sats/fee_sats may be
 * 0 to skip recording. */
int store_record_block(store_t *s, uint64_t ts_ms, int height,
                       const char *hash, const char *finder_name,
                       const char *finder_address,
                       int64_t reward_sats, int64_t fee_sats);

/* Record an accepted share with the miner's payout_address so the worker
 * row can be tagged. payout_address may be NULL (legacy/tests). The
 * share_hash semantics match store_record_share() above. */
int store_record_share_addr(store_t *s, const char *worker_name,
                            const char *payout_address,
                            uint64_t ts_ms, double difficulty,
                            int is_block, const char *share_hash_or_null);

/* PPS credit: add delta_sats to the worker's accrued_sats in pps_credits.
 * Async (writer thread). delta_sats must be > 0. payout_address (the
 * miner's Thunder address) is tagged onto the workers row as usual. */
int store_record_credit(store_t *s, const char *worker_name,
                        const char *payout_address,
                        uint64_t ts_ms, int64_t delta_sats);

/* Record / refresh the upstream bitcoind tip the proxy is mining on.
 * Single-row upsert keyed on id=1. tip_observed_at is preserved when
 * (height, hash) match the existing row, so 'time since last tip change'
 * stays meaningful across repeated polls of the same tip. Synchronous —
 * not routed through the writer thread (it's called at most once per
 * bitcoind_poll_interval_ms). */
int store_record_node_tip(store_t *s, int height, const char *hash,
                          uint64_t observed_ts_s, uint64_t updated_ts_s);

/* Flush and wait until all currently-queued events are committed. Useful
 * for tests and clean shutdown before exit. Returns 0 ok, negative on
 * timeout (default 5s). */
int store_flush(store_t *s);

/* Stats for /metrics / health endpoints. Lockless reads of atomics. */
typedef struct {
    uint64_t shares_queued;
    uint64_t shares_committed;
    uint64_t shares_dropped;
    uint64_t rejects_queued;
    uint64_t rejects_committed;
    uint64_t blocks_committed;
    uint64_t batches;
    uint64_t pg_errors;        /* poorly named; sqlite errors */
} store_stats_t;
void store_get_stats(store_t *s, store_stats_t *out);

/* Optional: override default ring buffer capacity (events). Must be called
 * before store_open by setting a global; for tests only. */
void store_test_set_ring_capacity(size_t cap);

#endif /* SIMPLEPOOL_STORE_H */
