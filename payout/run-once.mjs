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
import { runOnce, reportStuck } from './lib/payout.js';

const cfg = loadConfig();

const log = {
    debug: (m) => process.env.PAYOUT_DEBUG === '1' && console.error(`[debug] ${m}`),
    info:  (m) => console.error(`[info]  ${m}`),
    warn:  (m) => console.error(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

const db      = openDb(cfg.dbPath);
const thunder = new ThunderClient({
    url:  cfg.rpcUrl,
    user: cfg.rpcUser,
    pass: cfg.rpcPass,
});

reportStuck({ db }, log);
const res = await runOnce({ db, thunder, cfg }, log);
db.close();

console.log(JSON.stringify(res));
process.exit(res.failed > 0 ? 1 : 0);
