/* Slipstream service config, loaded from environment variables.
 *
 * Required:
 *   ENFORCER_GBT_URL        the enforcer's block template server, the same
 *                           endpoint the proxy mines from (bitcoind_url in
 *                           proxy.conf), e.g. http://127.0.0.1:8122. The
 *                           enforcer must run with --enable-slipstream.
 *
 * Optional:
 *   SLIPSTREAM_DB_PATH      this service's own SQLite file (default
 *                           ../data/slipstream.db). Every submission and
 *                           every status change is kept here. Not shares.db:
 *                           this service never writes to the proxy's
 *                           database, whose writer is the proxy's share
 *                           path, and nothing here is worth contending
 *                           with it for.
 *   PROXY_DB_PATH           shares.db, opened read-only (default
 *                           ../data/shares.db). Pool identity for info.json
 *                           comes from pool_meta, and blocks_found says
 *                           whether the block that mined a tx was ours.
 *                           Without it both are reported as unknown.
 *   BITCOIND_RPC_URL        bitcoind JSON-RPC, read-only use (getblockheader,
 *                           getrawtransaction). Without it a mined tx is
 *                           never confirmed or noticed orphaned: the
 *                           enforcer's template server cannot answer either.
 *   BITCOIND_RPC_USER / BITCOIND_RPC_PASS, or BITCOIND_RPC_COOKIE_FILE
 *   SLIPSTREAM_PORT         HTTP port (default 8124)
 *   SLIPSTREAM_BIND         bind address (default 127.0.0.1; put nginx in
 *                           front to publish it)
 *   SLIPSTREAM_TRUST_PROXY  '1' = take the client address from
 *                           X-Forwarded-For, for rate limiting. Only behind a
 *                           proxy that sets it, or anyone can pick theirs.
 *   SLIPSTREAM_MIN_FEE_RATE floor in sat/vB (default 1). The required rate is
 *                           the higher of this and the current mineable rate,
 *                           as Slipstream states it.
 *   SLIPSTREAM_CONFIRMATIONS depth at which a mined tx counts as confirmed
 *                           (default 6)
 *   SLIPSTREAM_EXPIRY_BLOCKS blocks a tx may wait unmined before it is
 *                           withdrawn (default 144, about a day)
 *   SLIPSTREAM_POLL_MS      how often to read the template and follow every
 *                           open tx (default 5000)
 *   SLIPSTREAM_RATE_LIMIT_PER_MIN
 *                           submissions per client address per minute
 *                           (default 30)
 *
 * info.json presentation fields — facts about the pool (mode, fee, coinbase
 * tag, addresses) are never configured here, they are read from pool_meta,
 * which the proxy writes:
 *   POOL_NAME, POOL_OPERATOR, POOL_LOGO, POOL_CONTACT
 *   POOL_CHAIN              chain name as the pool advertises it, e.g.
 *                           'betanet'. pool_meta.network can only say main,
 *                           test, signet or regtest. Defaults to that.
 *   POOL_PAYOUT_TEXT        overrides the per-mode default payout sentence
 *   PUBLIC_STRATUM_URL, PUBLIC_DASHBOARD_URL, PUBLIC_SLIPSTREAM_URL
 */

function num(name, dflt, { min = -Infinity } = {}) {
    const raw = process.env[name];
    if (raw === undefined || raw === '') return dflt;
    const v = Number(raw);
    if (!Number.isFinite(v) || v < min) {
        throw new Error(`${name}=${raw}: expected a number >= ${min}`);
    }
    return v;
}

function str(name) {
    const v = process.env[name];
    return v === undefined || v === '' ? null : v;
}

export function loadConfig() {
    const enforcerUrl = str('ENFORCER_GBT_URL');
    if (!enforcerUrl) {
        throw new Error('ENFORCER_GBT_URL is required (the enforcer block template server)');
    }
    return {
        enforcerUrl,
        dbPath:       str('SLIPSTREAM_DB_PATH') || '../data/slipstream.db',
        proxyDbPath:  str('PROXY_DB_PATH')      || '../data/shares.db',
        bitcoind: str('BITCOIND_RPC_URL') ? {
            url:        str('BITCOIND_RPC_URL'),
            user:       str('BITCOIND_RPC_USER'),
            pass:       str('BITCOIND_RPC_PASS'),
            cookieFile: str('BITCOIND_RPC_COOKIE_FILE'),
        } : null,
        port:         num('SLIPSTREAM_PORT', 8124, { min: 1 }),
        bind:         str('SLIPSTREAM_BIND') || '127.0.0.1',
        trustProxy:   process.env.SLIPSTREAM_TRUST_PROXY === '1',
        minFeeRate:   num('SLIPSTREAM_MIN_FEE_RATE', 1, { min: 0 }),
        confirmations: num('SLIPSTREAM_CONFIRMATIONS', 6, { min: 1 }),
        expiryBlocks: num('SLIPSTREAM_EXPIRY_BLOCKS', 144, { min: 1 }),
        pollMs:       num('SLIPSTREAM_POLL_MS', 5000, { min: 100 }),
        rateLimitPerMin: num('SLIPSTREAM_RATE_LIMIT_PER_MIN', 30, { min: 1 }),
        presentation: {
            name:         str('POOL_NAME'),
            operator:     str('POOL_OPERATOR'),
            logo:         str('POOL_LOGO'),
            contact:      str('POOL_CONTACT'),
            chain:        str('POOL_CHAIN'),
            payoutText:   str('POOL_PAYOUT_TEXT'),
            stratumUrl:   str('PUBLIC_STRATUM_URL'),
            dashboardUrl: str('PUBLIC_DASHBOARD_URL'),
            slipstreamUrl: str('PUBLIC_SLIPSTREAM_URL'),
        },
    };
}
