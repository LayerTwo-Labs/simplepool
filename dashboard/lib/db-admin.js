/* Writable SQLite handle used by admin write-actions (only the
 * deposit action today).
 *
 * Kept in its own module so no read-side code (stats.js, admin.js) can
 * accidentally reach for the writable handle. server.js imports both
 * openDb (read-only) and openAdminDb (writable) explicitly and passes
 * each to the routes that need it. */

import { openDb } from './db.js';

export function openAdminDb(dbPath) {
    return openDb(dbPath, { readonly: false });
}
