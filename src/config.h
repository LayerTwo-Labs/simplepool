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
    int  templates_retention_days;  /* template history kept; 0 = forever */

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
    /* PPS rate override — sats credited per unit of share difficulty.
     *
     * Leave unset (0) and the proxy derives the rate from each block
     * template as (coinbasevalue / network_difficulty) * (1 - fee_bps/1e4).
     * That is the recommended configuration: fee_bps becomes the single
     * knob controlling the fee, and the rate tracks difficulty instead of
     * going stale.
     *
     * Set it and the value is used verbatim and taken to be ALREADY NET of
     * fee — fee_bps is not applied on top, because historically operators
     * baked the fee into this number by hand. The proxy logs the fee that
     * choice actually implies and warns when it disagrees with fee_bps.
     * A static value silently drifts as difficulty moves, and can invert
     * into paying miners more than the pool earns, so prefer derived. */
    double pps_sats_per_diff;

    /* Minimum network difficulty at which PPS accrual is allowed to run.
     *
     * The PPS rate is block_value / network_difficulty, which is the expected
     * value of a share — correct only while every share the pool produces has
     * an independent chance of becoming a block. That holds when difficulty is
     * calibrated to hashrate. It stops holding when the pool produces
     * solutions faster than the chain accepts blocks, and then the rate is
     * overstated by exactly that ratio.
     *
     * The threshold is the difficulty at which this pool ALONE would find one
     * block per block interval:
     *
     *     min_difficulty = pool_hashrate * block_interval_sec / 2^32
     *
     * A 40 TH/s pool on a 600s chain needs difficulty >= ~5,600,000. Below it,
     * accrual is refused. That is a floor, not a target: the pool shares the
     * chain with other miners, so the genuinely safe difficulty is higher.
     *
     * 0 disables the check, which is only safe on a chain whose difficulty is
     * already calibrated — mainnet, testnet, signet. On a young forknet during
     * its difficulty ramp, leaving this at 0 is how a pool accrues millions of
     * BTC of liability in minutes. The proxy logs the value it observes to be
     * necessary, so a wrong setting is visible rather than silent. */
    double pps_min_network_difficulty;

    /* Target seconds between blocks on this chain — 600 for Bitcoin and every
     * chain derived from it. Used for the difficulty floor above and for the
     * issuance ceiling, which caps accrual at what the chain can actually mint
     * (one block_value per interval, across all miners on earth). */
    int block_interval_sec;

    /* Refuse mining.authorize while accrual is gated off by the floor.
     *
     * Default on, and deliberately so: a miner whose shares are accepted but
     * not credited is working for free without being told. Turning it off
     * accepts shares that earn nothing, which is only reasonable if the miners
     * are yours and you know why. */
    int pps_refuse_shares_below_min;

    /* logging */
    int  log_level;            /* 0..3 */
} proxy_config_t;

void proxy_config_defaults(proxy_config_t *cfg);
int  proxy_config_load(const char *path, proxy_config_t *cfg,
                       char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_CONFIG_H */
