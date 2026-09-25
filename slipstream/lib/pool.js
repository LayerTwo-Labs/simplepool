/* Read-only view of the proxy's shares.db.
 *
 * Opened lazily and re-tried, like the dashboard's: the proxy may not have
 * created it yet. Every read degrades to "unknown" rather than failing, so
 * slipstream keeps accepting txs through a proxy restart.
 */

import Database from 'better-sqlite3';
import fs from 'node:fs';

export function openPoolDb(path) {
    let db = null;
    let lastTryMs = 0;

    function get() {
        if (db) return db;
        const nowMs = Date.now();
        if (!path || nowMs - lastTryMs < 1000) return null;
        lastTryMs = nowMs;
        if (!fs.existsSync(path)) return null;
        try {
            db = new Database(path, { readonly: true, fileMustExist: true });
            db.pragma('busy_timeout = 2000');
        } catch (err) {
            console.error(`[warn]  pool db open failed: ${err.message}`);
            db = null;
        }
        return db;
    }

    return {
        /* pool_meta's identity columns, or null if there is no pool_meta
         * yet. Read per call: a proxy restarted onto another mode or network
         * shows up on the next request. */
        meta() {
            const d = get();
            if (!d) return null;
            try {
                return d.prepare(`
                    SELECT pool_mode, fee_bps, network, coinbase_tag,
                           operator_address, pool_btc_address
                      FROM pool_meta WHERE id = 1
                `).get() ?? null;
            } catch {
                return null;   // a DB predating the identity columns
            }
        },

        /* Whether the pool found `blockHash`: true, false, or null when
         * shares.db cannot say. */
        foundBlock(blockHash) {
            const d = get();
            if (!d) return null;
            try {
                return d.prepare('SELECT 1 FROM blocks_found WHERE hash = ?').get(blockHash) !== undefined;
            } catch {
                return null;
            }
        },
    };
}
