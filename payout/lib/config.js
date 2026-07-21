/* Payout worker config loaded from environment variables.
 *
 * Required:
 *   PAYOUT_DB_PATH         path to data/shares.db (writable; we UPDATE
 *                          pps_credits.paid_sats on successful tx)
 *   THUNDER_RPC_URL        Thunder node JSON-RPC endpoint (e.g.
 *                          http://127.0.0.1:6000)
 *
 * Optional:
 *   PAYOUT_INTERVAL_MS     how often to scan for due payouts (default 30s)
 *   PAYOUT_MIN_SATS        skip workers below this owed balance (default 10000)
 *   PAYOUT_MAX_PER_TICK    cap workers paid per scan (default 50) to bound
 *                          tail latency and Thunder RPC load
 *   PAYOUT_DRY_RUN         '1' = log what would be sent, don't touch
 *                          Thunder or update paid_sats
 *   THUNDER_RPC_USER       optional basic-auth user
 *   THUNDER_RPC_PASS       optional basic-auth pass
 *   THUNDER_FROM_ADDRESS   pool reserve address to send from (must match
 *                          pool_thunder_reserve_address in proxy.conf so
 *                          it matches what the C proxy deposits to)
 */

function require_env(name) {
    const v = process.env[name];
    if (!v) {
        console.error(`fatal: ${name} is required`);
        process.exit(2);
    }
    return v;
}

export function loadConfig() {
    return {
        dbPath:        require_env('PAYOUT_DB_PATH'),
        rpcUrl:        require_env('THUNDER_RPC_URL'),
        rpcUser:       process.env.THUNDER_RPC_USER || null,
        rpcPass:       process.env.THUNDER_RPC_PASS || null,
        fromAddress:   require_env('THUNDER_FROM_ADDRESS'),
        intervalMs:    parseInt(process.env.PAYOUT_INTERVAL_MS  || '30000', 10),
        minSats:       BigInt(process.env.PAYOUT_MIN_SATS       || '10000'),
        maxPerTick:    parseInt(process.env.PAYOUT_MAX_PER_TICK || '50',    10),
        dryRun:        process.env.PAYOUT_DRY_RUN === '1',
        /* Admin HTTP surface — used by the dashboard's "Trigger payout now"
         * button. Loopback-bound by default; set port=0 to disable. */
        adminHttpBind: process.env.PAYOUT_ADMIN_BIND || '127.0.0.1',
        adminHttpPort: parseInt(process.env.PAYOUT_ADMIN_PORT || '9080', 10),
    };
}
