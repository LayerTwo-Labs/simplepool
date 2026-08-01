#ifndef SIMPLEPOOL_CONFIG_H
#define SIMPLEPOOL_CONFIG_H

#include <stddef.h>

typedef struct {
    /* listener */
    char listen_addr[64];
    int  listen_port;
    int  max_conns;
    double initial_diff;

    /* vardiff — auto-adjust each connection's difficulty to keep the
     * share rate near `target_spm` shares/minute. Set vardiff_enabled = 0
     * to pin every connection to initial_diff (the legacy behaviour). */
    int    vardiff_enabled;       /* default 1 */
    double vardiff_target_spm;    /* default 12 shares/min (one every 5s) */
    double vardiff_min;           /* default 1.0 */
    double vardiff_max;           /* default 1e12; clamped by network diff */
    int    vardiff_window_sec;    /* retarget interval, default 30 */

    /* Idle-connection reaper. A connection that hasn't sent any bytes in
     * idle_timeout_sec is closed. Guards against half-open TCPs from
     * crashed miners and clients that connect but never authenticate.
     * Set to a negative value to disable entirely; 0 uses the default. */
    int    idle_timeout_sec;      /* default 600 (10 min) */

    /* bitcoind */
    char bitcoind_url[512];
    char bitcoind_user[128];
    char bitcoind_pass[256];
    int  bitcoind_poll_interval_ms;

    /* coinbase */
    char operator_address[128];   /* 1% (fee_bps) fee recipient */
    int  fee_bps;                 /* basis points, default 100 (=1%), cap 1000 */
    char coinbase_tag[64];

    /* sqlite */
    char db_path[512];
    int  commit_window_ms;
    int  commit_max_shares;

    /* redis broadcast — optional. Empty url disables the module. */
    char redis_url[256];
    int  redis_publish_timeout_ms;
    int  redis_reconnect_backoff_ms;

    /* PPS mode. pool_mode = "solo" (default) preserves the per-block
     * direct-payout flow: each miner's coinbase pays that miner. pool_mode
     * = "pps-classic" pays every block into a single pool-owned BTC wallet
     * and credits each accepted share to the worker's pps_credits row; the
     * operator later batches that BTC into Thunder via the admin
     * dashboard's deposit action, and the payout worker drains the Thunder
     * reserve to miners. */
    char pool_mode[16];                       /* "solo" | "pps-classic" */
    /* pps-classic: coinbase pays this BTC address (P2WPKH/P2PKH/P2SH) for
     * the net-of-fee reward. Required when pool_mode = pps-classic;
     * ignored otherwise. */
    char pool_btc_address[128];
    /* PPS rate — sats credited per unit of share difficulty. Used by the
     * payout worker downstream; the C proxy only computes accrued credits. */
    double pps_sats_per_diff;

    /* logging */
    int  log_level;            /* 0..3 */
} proxy_config_t;

void proxy_config_defaults(proxy_config_t *cfg);
int  proxy_config_load(const char *path, proxy_config_t *cfg,
                       char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_CONFIG_H */
