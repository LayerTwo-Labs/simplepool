/* simplepool-broadcaster — post the pool's stats to a Telegram channel.
 *
 * Reads the dashboard's public JSON API and posts to one channel: a daily
 * digest, found / orphaned blocks, the pool going quiet and coming back,
 * ledger health changes, and optionally a pinned message kept current.
 * With BROADCASTER_COMMANDS=1 it also answers /pool_status in that chat.
 *
 * Config is environment-only — see lib/config.js for the full list.
 *
 * Run:
 *   TELEGRAM_BOT_TOKEN=... TELEGRAM_CHAT_ID=@yourchannel \
 *   DASHBOARD_URL=http://127.0.0.1:8081 \
 *   node index.js
 *
 * Try it without a token: BROADCASTER_DRY_RUN=1 prints every post instead.
 */

import { readFileSync } from 'node:fs';

import { loadConfig } from './lib/config.js';
import { loadState, saveState } from './lib/state.js';
import { DashboardClient } from './lib/dashboard.js';
import { TelegramClient, DryRunClient } from './lib/telegram.js';
import { Broadcaster } from './lib/broadcaster.js';
import { Commands } from './lib/commands.js';

const cfg = loadConfig();
const { version } = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'));

const log = {
    info:  (m) => console.log(`[info]  ${m}`),
    warn:  (m) => console.warn(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

log.info(`simplepool-broadcaster ${version} starting ` +
         `(dashboard=${cfg.dashboardUrl} chat=${cfg.chatId ?? '-'}` +
         `${cfg.threadId != null ? ` topic=${cfg.threadId}` : ''} state=${cfg.statePath}` +
         `${cfg.dryRun ? ' DRY RUN' : ''})`);
log.info(`  poll ${cfg.pollMs}ms, digest ${cfg.digestHour == null ? 'off' : `${cfg.digestHour}:00 UTC`}, ` +
         `live ${cfg.live ? `every ${cfg.liveMs}ms${cfg.livePin ? '' : ' (unpinned)'}` : 'off'}, ` +
         `summary ${cfg.summaryHours == null ? 'off' : `every ${cfg.summaryHours}h`}, ` +
         `stale after ${cfg.staleSharesSec}s`);

// One state object and one writer for both loops, so neither can save over
// the other's changes with a stale copy.
const state = loadState(cfg.statePath);
const persist = () => saveState(cfg.statePath, state);
const dashboard = new DashboardClient({ url: cfg.dashboardUrl });
const telegram = cfg.dryRun
    ? new DryRunClient()
    : new TelegramClient({ token: cfg.token, chatId: cfg.chatId, threadId: cfg.threadId });

const broadcaster = new Broadcaster({ dashboard, telegram, state, cfg, log, persist });

if (cfg.commands && cfg.dryRun) {
    log.info('  commands: off under BROADCASTER_DRY_RUN (they need a real bot to read messages)');
} else if (cfg.commands) {
    const commands = new Commands({ dashboard, telegram, state, cfg, log, persist });
    // Its own loop: a long poll must never delay a post, and a failure here
    // must never stop the posts.
    commands.run().catch((e) => log.error(`commands stopped: ${e.message}`));
}

let lastTickError = null;
async function loop() {
    try {
        await broadcaster.tick();
        if (lastTickError) log.info('tick succeeded again');
        lastTickError = null;
    } catch (e) {
        // Logged once per distinct failure, not once per poll
        if (e.message !== lastTickError) log.warn(`tick failed: ${e.message}`);
        lastTickError = e.message;
    }
    setTimeout(loop, cfg.pollMs);
}

loop();
