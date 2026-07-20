/* Unit tests for the broadcast module.
 *
 * These tests run without a live redis. They exercise:
 *  - disabled mode (empty url -> open/close no-op, publishes are no-ops)
 *  - enqueue without redis -> dropped_redis_down increments, no crash
 *  - shutdown drains and joins cleanly
 *
 * Live-redis integration is covered in tests/test_integration.sh.
 */

#define _POSIX_C_SOURCE 200809L
#include "../src/broadcast.h"
#include "../src/log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_disabled(void) {
    broadcast_cfg_t cfg = {0};
    /* url empty => disabled */
    broadcast_t *b = NULL;
    CHECK(broadcast_open(&cfg, &b) == 0);
    CHECK(b != NULL);

    /* Every publish on a disabled instance is a no-op (must not crash). */
    broadcast_share(b, "worker.rig", "addr", 12345, 1.5, 0, NULL);
    broadcast_share(b, "worker.rig", "addr", 12346, 1.5, 1, "deadbeef");
    broadcast_reject(b, "worker.rig", 12347, "low diff");
    broadcast_block(b, "worker", "addr", 12348, 100, "abcd", 5000000000LL, 50000LL);
    broadcast_node_tip(b, 99, "ab12", 12349);
    broadcast_credit(b, "worker", 12350, 500, 1500);

    broadcast_stats_t s;
    broadcast_get_stats(b, &s);
    CHECK(s.enqueued == 0);
    CHECK(s.published == 0);

    broadcast_close(b);
}

static void test_no_redis_running(void) {
    /* Point at a port nothing's listening on; expect drops, no crash. */
    broadcast_cfg_t cfg = {0};
    snprintf(cfg.url, sizeof cfg.url, "127.0.0.1:1");
    cfg.publish_timeout_ms = 50;
    cfg.reconnect_backoff_ms = 50;
    cfg.queue_capacity = 64;

    broadcast_t *b = NULL;
    CHECK(broadcast_open(&cfg, &b) == 0);
    CHECK(b != NULL);

    for (int i = 0; i < 8; i++) {
        broadcast_share(b, "w", "a", 1000 + (uint64_t)i, 1.0, 0, NULL);
    }

    /* Give the worker a moment to attempt the first connect & fail. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
    nanosleep(&ts, NULL);

    broadcast_stats_t s;
    broadcast_get_stats(b, &s);
    CHECK(s.enqueued == 8);
    CHECK(s.published == 0);
    CHECK(s.dropped_redis_down > 0);

    broadcast_close(b);
}

static void test_queue_overflow(void) {
    /* Tiny queue + no redis -> first few enqueue ok, rest drop. */
    broadcast_cfg_t cfg = {0};
    snprintf(cfg.url, sizeof cfg.url, "127.0.0.1:1");
    cfg.publish_timeout_ms = 50;
    cfg.reconnect_backoff_ms = 60000;  /* don't reconnect; we want full queue */
    cfg.queue_capacity = 4;

    broadcast_t *b = NULL;
    CHECK(broadcast_open(&cfg, &b) == 0);

    /* The worker will dequeue one event for its first (failing) connect,
     * so push enough to overflow even after that pull. */
    for (int i = 0; i < 32; i++) {
        broadcast_share(b, "w", "a", 1000 + (uint64_t)i, 1.0, 0, NULL);
    }
    /* No sleep — overflows happen synchronously in q_push. */
    broadcast_stats_t s;
    broadcast_get_stats(b, &s);
    CHECK(s.dropped_queue_full > 0);

    broadcast_close(b);
}

int main(void) {
    /* Two of these tests deliberately point the broadcaster at a dead
     * port to verify the worker thread degrades gracefully when Redis is
     * unreachable. That produces WARN lines that look like errors when
     * `make test` is scanned — silence everything below ERROR. */
    log_init(3);

    test_disabled();
    test_no_redis_running();
    test_queue_overflow();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_broadcast: ok\n");
    return 0;
}
