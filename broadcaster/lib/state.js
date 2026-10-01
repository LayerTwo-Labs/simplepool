/* What has already been posted, kept in one small JSON file so a restart
 * neither repeats a post nor skips one.
 *
 *   initialized       false until the first successful poll. That poll
 *                     records every block already in the list without
 *                     announcing it: a fresh install should not post the
 *                     pool's whole history.
 *   blocks            hash -> last status announced
 *   lastDigestDate    'YYYY-MM-DD' (UTC) of the last daily digest
 *   liveMessageId     the pinned message edited in place
 *   liveUpdatedAt     ms of its last refresh
 *   sharesFlowing     last announced liveness (true/false/null)
 *   healthOk          last announced ledger-health state (true/false/null)
 *
 * Written to a temp file and renamed, so a crash mid-write leaves the old
 * file rather than a truncated one.
 */

import { readFileSync, writeFileSync, renameSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';

/* Hashes are only ever compared against the latest page of blocks, so
 * older ones can be dropped. */
const MAX_BLOCKS = 500;

export function emptyState() {
    return {
        initialized: false,
        blocks: {},
        lastDigestDate: null,
        liveMessageId: null,
        liveUpdatedAt: 0,
        sharesFlowing: null,
        healthOk: null,
    };
}

export function loadState(path) {
    let raw;
    try {
        raw = readFileSync(path, 'utf8');
    } catch (e) {
        if (e.code === 'ENOENT') return emptyState();
        throw e;
    }
    return { ...emptyState(), ...JSON.parse(raw) };
}

export function saveState(path, state) {
    const keys = Object.keys(state.blocks);
    if (keys.length > MAX_BLOCKS) {
        for (const k of keys.slice(0, keys.length - MAX_BLOCKS)) delete state.blocks[k];
    }
    mkdirSync(dirname(path), { recursive: true });
    const tmp = `${path}.tmp`;
    writeFileSync(tmp, JSON.stringify(state, null, 2));
    renameSync(tmp, path);
}
