/* simplepool-slipstream — take txs from anyone, mine them without relaying.
 *
 * Submissions go straight to the enforcer's block template server, the one
 * the proxy mines from, which keeps them in its template mempool and never
 * hands them to the node's. This service records every submission, holds
 * each one to the fee rule, and follows it from template to block.
 *
 * Config is environment-only — see lib/config.js for the full list.
 *
 * Run:
 *   ENFORCER_GBT_URL=http://127.0.0.1:8122 \
 *   BITCOIND_RPC_URL=http://127.0.0.1:8332 BITCOIND_RPC_COOKIE_FILE=... \
 *   PROXY_DB_PATH=../data/shares.db \
 *   node index.js
 */

import { readFileSync } from 'node:fs';

import { loadConfig } from './lib/config.js';
import { openStore } from './lib/store.js';
import { openPoolDb } from './lib/pool.js';
import { EnforcerClient, BitcoindClient } from './lib/rpc.js';
import { Slipstream } from './lib/slipstream.js';
import { startHttp } from './lib/http.js';

const cfg = loadConfig();
const { version } = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'));

const log = {
    debug: (m) => process.env.SLIPSTREAM_DEBUG === '1' && console.log(`[debug] ${m}`),
    info:  (m) => console.log(`[info]  ${m}`),
    warn:  (m) => console.warn(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

log.info(`simplepool-slipstream ${version} starting (enforcer=${cfg.enforcerUrl} db=${cfg.dbPath})`);
log.info(`  fee floor ${cfg.minFeeRate} sat/vB, confirmed at ${cfg.confirmations}, ` +
         `expiry ${cfg.expiryBlocks} blocks, poll ${cfg.pollMs}ms`);
if (!cfg.bitcoind) {
    log.warn('BITCOIND_RPC_URL unset: mined txs are never checked for orphaning, ' +
             'and their depth is only estimated from the template height');
}

const slipstream = new Slipstream({
    store:    openStore(cfg.dbPath),
    enforcer: new EnforcerClient({ url: cfg.enforcerUrl }),
    bitcoind: cfg.bitcoind ? new BitcoindClient(cfg.bitcoind) : null,
    pool:     openPoolDb(cfg.proxyDbPath),
    cfg,
    log,
});

let lastTickError = null;
async function loop() {
    try {
        await slipstream.tick();
        if (lastTickError) log.info('enforcer reachable again');
        lastTickError = null;
    } catch (e) {
        // Logged once per distinct failure, not once per poll
        if (e.message !== lastTickError) log.warn(`tick failed: ${e.message}`);
        lastTickError = e.message;
    }
    setTimeout(loop, cfg.pollMs);
}

startHttp({ slipstream, pool: slipstream.pool, cfg, version, log });
loop();
