/* simplepool-slipstream — take txs from anyone, mine them in the pool's blocks.
 *
 * Submissions are checked against the pool's own bitcoind and the fee rule,
 * then broadcast to it. The enforcer's template mempool mirrors that node's,
 * so an accepted tx reaches the templates the proxy mines. This service
 * records every submission and follows each accepted tx from template to
 * block.
 *
 * Config is environment-only — see lib/config.js for the full list.
 *
 * Run:
 *   BITCOIND_RPC_URL=http://127.0.0.1:8332 BITCOIND_RPC_COOKIE_FILE=... \
 *   ENFORCER_GBT_URL=http://127.0.0.1:8122 \
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

log.info(`simplepool-slipstream ${version} starting ` +
         `(node=${cfg.bitcoind.url} enforcer=${cfg.enforcerUrl} db=${cfg.dbPath})`);
log.info(`  fee floor ${cfg.minFeeRate} sat/vB, confirmed at ${cfg.confirmations}, ` +
         `poll ${cfg.pollMs}ms`);

const slipstream = new Slipstream({
    store:    openStore(cfg.dbPath),
    bitcoind: new BitcoindClient(cfg.bitcoind),
    enforcer: new EnforcerClient({ url: cfg.enforcerUrl }),
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
