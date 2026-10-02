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
 *   TELEGRAM_THREAD_ID      post into one topic of a forum group instead of
 *                           its General topic. A topic link
 *                           https://t.me/c/1518607784/23563 is chat
 *                           -1001518607784, topic 23563. Unset = General.
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
 *   BROADCASTER_LIVE_PIN    '0' = post the live message without pinning it,
 *                           for a chat where the bot may not pin (default 1)
 *   BROADCASTER_COMMANDS    '1' = answer /pool_status in TELEGRAM_CHAT_ID with
 *                           the current stats (default off). Reads messages
 *                           with getUpdates, so nothing else may poll this
 *                           bot and it must have no webhook.
 *   BROADCASTER_COMMAND_COOLDOWN_SEC  at most one reply per chat this often
 *                           (default 30), so the command cannot flood a chat
 *   BROADCASTER_SUMMARY_HOURS  also post the current stats as a NEW message
 *                           every N hours (1-168), on UTC boundaries: 6 means
 *                           00, 06, 12 and 18 UTC (exact for any N that
 *                           divides 24). The pinned live message
 *                           is edited in place, and Telegram neither notifies
 *                           nor moves it for an edit; this is the post that
 *                           shows up in the feed. Default off.
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

/* Topic ids are message ids: a positive integer. */
function topicId() {
    const v = num('TELEGRAM_THREAD_ID', null, { min: 1 });
    if (v != null && !Number.isInteger(v)) {
        throw new Error(`TELEGRAM_THREAD_ID=${v}: expected a whole number (the topic id)`);
    }
    return v;
}

export function loadConfig() {
    const dryRun = process.env.BROADCASTER_DRY_RUN === '1';
    if (!dryRun) {
        for (const name of ['TELEGRAM_BOT_TOKEN', 'TELEGRAM_CHAT_ID']) {
            if (!str(name)) throw new Error(`${name} is required (or set BROADCASTER_DRY_RUN=1)`);
        }
    }
    const digestRaw = str('DIGEST_UTC_HOUR');
    const summaryRaw = str('BROADCASTER_SUMMARY_HOURS');
    return {
        token:        str('TELEGRAM_BOT_TOKEN'),
        chatId:       str('TELEGRAM_CHAT_ID'),
        threadId:     topicId(),
        dashboardUrl: (str('DASHBOARD_URL') || 'http://127.0.0.1:8081').replace(/\/+$/, ''),
        publicUrl:    str('PUBLIC_DASHBOARD_URL')?.replace(/\/+$/, '') ?? null,
        poolName:     str('POOL_NAME') || 'simplepool',
        statePath:    str('BROADCASTER_STATE_PATH') || '../data/broadcaster.json',
        pollMs:       num('BROADCASTER_POLL_MS', 60000, { min: 1000 }),
        digestHour:   digestRaw === 'off' ? null : num('DIGEST_UTC_HOUR', 12, { min: 0, max: 23 }),
        live:         process.env.BROADCASTER_LIVE === '1',
        livePin:      process.env.BROADCASTER_LIVE_PIN !== '0',
        commands:     process.env.BROADCASTER_COMMANDS === '1',
        commandCooldownSec: num('BROADCASTER_COMMAND_COOLDOWN_SEC', 30, { min: 0 }),
        liveMs:       num('BROADCASTER_LIVE_MS', 300000, { min: 60000 }),
        summaryHours: summaryRaw == null || summaryRaw === 'off' || summaryRaw === '0'
            ? null : num('BROADCASTER_SUMMARY_HOURS', null, { min: 1, max: 168 }),
        staleSharesSec: num('STALE_SHARES_SEC', 900, { min: 60 }),
        debounce:     num('HEALTH_DEBOUNCE', 3, { min: 1 }),
        dryRun,
    };
}
