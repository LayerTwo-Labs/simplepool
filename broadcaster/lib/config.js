/* Broadcaster config, loaded from environment variables.
 *
 * Required (unless BROADCASTER_DRY_RUN=1):
 *   TELEGRAM_BOT_TOKEN      from @BotFather. The bot is only a posting
 *                           credential: add it to the channel as an admin
 *                           with "Post messages" (plus "Edit messages" and
 *                           "Pin messages" for BROADCASTER_LIVE=1). Posts show
 *                           under the channel's name, not the bot's.
 *   TELEGRAM_CHAT_ID        '@channelname' for a public channel, or the
 *                           numeric '-100...' id for a private one.
 *
 * Optional:
 *   DASHBOARD_URL           where to read stats from (default
 *                           http://127.0.0.1:8081). Only the dashboard's
 *                           public JSON API is used, never shares.db, so
 *                           every number posted is the number the site shows.
 *   PUBLIC_DASHBOARD_URL    link included in posts, e.g.
 *                           https://pool.example.com. Omitted when unset.
 *   POOL_NAME               heading for posts (default 'simplepool')
 *   BROADCASTER_STATE_PATH  small JSON file remembering what was already
 *                           posted, so a restart does not repeat itself
 *                           (default ../data/broadcaster.json)
 *   BROADCASTER_POLL_MS     how often to read the dashboard (default 60000)
 *   DIGEST_UTC_HOUR         hour (0-23, UTC) of the daily digest (default 12).
 *                           Set to 'off' to disable it.
 *   BROADCASTER_LIVE        '1' = keep one pinned message edited in place
 *                           with current stats (default off)
 *   BROADCASTER_LIVE_MS     how often the pinned message is refreshed
 *                           (default 300000)
 *   STALE_SHARES_SEC        no accepted share for this long is announced as
 *                           the pool being down (default 900)
 *   HEALTH_DEBOUNCE         consecutive polls a new health / liveness state
 *                           must hold before it is announced (default 3), so
 *                           a flapping check does not spam the channel
 *   BROADCASTER_DRY_RUN     '1' = print posts to stdout instead of sending
 */

function num(name, dflt, { min = -Infinity, max = Infinity } = {}) {
    const raw = process.env[name];
    if (raw === undefined || raw === '') return dflt;
    const v = Number(raw);
    if (!Number.isFinite(v) || v < min || v > max) {
        throw new Error(`${name}=${raw}: expected a number in [${min}, ${max}]`);
    }
    return v;
}

function str(name) {
    const v = process.env[name];
    return v === undefined || v === '' ? null : v;
}

export function loadConfig() {
    const dryRun = process.env.BROADCASTER_DRY_RUN === '1';
    if (!dryRun) {
        for (const name of ['TELEGRAM_BOT_TOKEN', 'TELEGRAM_CHAT_ID']) {
            if (!str(name)) throw new Error(`${name} is required (or set BROADCASTER_DRY_RUN=1)`);
        }
    }
    const digestRaw = str('DIGEST_UTC_HOUR');
    return {
        token:        str('TELEGRAM_BOT_TOKEN'),
        chatId:       str('TELEGRAM_CHAT_ID'),
        dashboardUrl: (str('DASHBOARD_URL') || 'http://127.0.0.1:8081').replace(/\/+$/, ''),
        publicUrl:    str('PUBLIC_DASHBOARD_URL')?.replace(/\/+$/, '') ?? null,
        poolName:     str('POOL_NAME') || 'simplepool',
        statePath:    str('BROADCASTER_STATE_PATH') || '../data/broadcaster.json',
        pollMs:       num('BROADCASTER_POLL_MS', 60000, { min: 1000 }),
        digestHour:   digestRaw === 'off' ? null : num('DIGEST_UTC_HOUR', 12, { min: 0, max: 23 }),
        live:         process.env.BROADCASTER_LIVE === '1',
        liveMs:       num('BROADCASTER_LIVE_MS', 300000, { min: 60000 }),
        staleSharesSec: num('STALE_SHARES_SEC', 900, { min: 60 }),
        debounce:     num('HEALTH_DEBOUNCE', 3, { min: 1 }),
        dryRun,
    };
}
