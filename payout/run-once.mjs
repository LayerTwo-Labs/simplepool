/* One-shot payout tick — same config surface as index.js, but runs a
 * single runOnce() and exits instead of looping. For operators who want
 * cron-style control, and for the payout integration test.
 *
 * Exit codes: 0 = tick completed with no failed payouts,
 *             1 = at least one payout failed,
 *             2 = bad config.
 * Prints the tick summary as one JSON line on stdout. */

import { loadConfig } from './lib/config.js';
import { openDb } from './lib/db.js';
import { ThunderClient } from './lib/thunder.js';
import { EnforcerWalletClient } from './lib/enforcer-wallet.js';
import { runOnce, reportStuck } from './lib/payout.js';

const cfg = loadConfig();

const log = {
    debug: (m) => process.env.PAYOUT_DEBUG === '1' && console.error(`[debug] ${m}`),
    info:  (m) => console.error(`[info]  ${m}`),
    warn:  (m) => console.error(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

const db = openDb(cfg.dbPath);
/* Same rail selection as index.js, and it has to be: PAYOUT_RAIL decides
 * which client can actually move the money, not merely which environment
 * variables are required. Constructing a ThunderClient unconditionally here
 * pointed an L1 pool at Thunder with cfg.rpcUrl === null -- the tick found
 * nobody to pay and exited 0, so cron-style operators got a clean run and no
 * payments. The two clients present the same interface, so nothing below
 * branches. */
const thunder = cfg.rail === 'btc'
    ? new EnforcerWalletClient({
        addr:            cfg.enforcerAddr,
        feeRateSatPerVb: cfg.feeRateSatPerVb,
        passphrase:      cfg.walletPassphrase,
      })
    : new ThunderClient({
        url:  cfg.rpcUrl,
        user: cfg.rpcUser,
        pass: cfg.rpcPass,
      });

reportStuck({ db }, log);
const res = await runOnce({ db, thunder, cfg }, log);
db.close();

console.log(JSON.stringify(res));
process.exit(res.failed > 0 ? 1 : 0);
