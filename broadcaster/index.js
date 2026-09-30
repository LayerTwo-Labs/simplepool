/* simplepool-broadcaster — post the pool's stats to a Telegram channel.
 *
 * Reads the dashboard's public JSON API and posts to one channel: a daily
 * digest, found / orphaned blocks, the pool going quiet and coming back,
 * ledger health changes, and optionally a pinned message kept current.
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
import { loadState } from './lib/state.js';
import { DashboardClient } from './lib/dashboard.js';
import { TelegramClient, DryRunClient } from './lib/telegram.js';
import { Broadcaster } from './lib/broadcaster.js';

const cfg = loadConfig();
const { version } = JSON.parse(readFileSync(new URL('./package.json', import.meta.url), 'utf8'));

const log = {
    info:  (m) => console.log(`[info]  ${m}`),
    warn:  (m) => console.warn(`[warn]  ${m}`),
    error: (m) => console.error(`[error] ${m}`),
};

log.info(`simplepool-broadcaster ${version} starting ` +
         `(dashboard=${cfg.dashboardUrl} chat=${cfg.chatId ?? '-'} state=${cfg.statePath}` +
         `${cfg.dryRun ? ' DRY RUN' : ''})`);
log.info(`  poll ${cfg.pollMs}ms, digest ${cfg.digestHour == null ? 'off' : `${cfg.digestHour}:00 UTC`}, ` +
         `live ${cfg.live ? `every ${cfg.liveMs}ms` : 'off'}, stale after ${cfg.staleSharesSec}s`);

const broadcaster = new Broadcaster({
    dashboard: new DashboardClient({ url: cfg.dashboardUrl }),
    telegram:  cfg.dryRun ? new DryRunClient() : new TelegramClient({ token: cfg.token, chatId: cfg.chatId }),
    state:     loadState(cfg.statePath),
    cfg,
    log,
});

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
