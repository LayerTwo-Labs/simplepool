/* /pool_status: answer a command in the chat with the current stats.
 *
 * Runs beside the poster, on its own long-poll loop (BROADCASTER_COMMANDS=1).
 * Only TELEGRAM_CHAT_ID is answered: the bot can be added anywhere, and a
 * stranger's group must not get a reply. The answer goes to the topic the
 * command was typed in, as a reply to it, and at most once per
 * BROADCASTER_COMMAND_COOLDOWN_SEC per chat, so the command cannot be used
 * to flood the chat.
 *
 * A bot in privacy mode (the default) is shown "/pool_status@<bot>" but not
 * always a bare "/pool_status". Registering the command puts it in the "/"
 * menu, and a tap there sends the addressed form.
 *
 * The update offset is saved in broadcaster.json, so a restart neither
 * answers a command twice nor answers commands typed while it was down: the
 * very first start skips the backlog.
 */

import * as msg from './messages.js';

export const COMMANDS = [
    { command: 'pool_status', description: 'Current pool stats' },
];

const RETRY_MS = 5000;

export class Commands {
    constructor({ dashboard, telegram, state, cfg, log, persist, sleep = defaultSleep }) {
        this.dashboard = dashboard;
        this.telegram = telegram;
        this.state = state;
        this.cfg = cfg;
        this.log = log;
        this.persist = persist;
        this.sleep = sleep;
        this.username = null;
        this.lastReply = new Map();   // chat id -> ms of the last reply
        this.stopped = false;
    }

    /* getMe, register the menu, skip the backlog on a first start. */
    async init() {
        this.username = (await this.telegram.getMe()).username ?? null;
        try {
            await this.telegram.setMyCommands(COMMANDS);
        } catch (e) {
            this.log.warn(`could not register the command menu: ${e.message}`);
        }
        if (this.state.updateOffset == null) {
            const pending = await this.telegram.getUpdates({ offset: -1, timeoutSec: 0 });
            this.state.updateOffset = pending.length ? pending[pending.length - 1].update_id + 1 : 0;
            this.persist();
        }
        this.log.info(`commands: answering /pool_status in ${this.cfg.chatId} as @${this.username}`);
    }

    /* One long poll, and every message it brought. */
    async pollOnce(clock = Date.now) {
        const updates = await this.telegram.getUpdates({ offset: this.state.updateOffset, timeoutSec: 25 });
        for (const u of updates) {
            // Advanced before handling: a reply that fails is not retried,
            // because the person can ask again and a stale answer is worse.
            this.state.updateOffset = u.update_id + 1;
            this.persist();
            if (u.message) await this.handle(u.message, clock());
        }
    }

    async run() {
        let ready = false;
        let lastError = null;
        while (!this.stopped) {
            try {
                // Retried like a poll: Telegram being unreachable at start
                // must not switch the command off until the next restart.
                if (!ready) { await this.init(); ready = true; }
                await this.pollOnce();
                lastError = null;
            } catch (e) {
                if (e.message !== lastError) this.log.warn(`commands: ${e.message}`);
                lastError = e.message;
                await this.sleep(RETRY_MS);
            }
        }
    }

    async handle(m, nowMs) {
        if (!this.#isPoolStatus(m.text) || !this.#allowedChat(m.chat)) return;

        const chatKey = String(m.chat.id);
        const last = this.lastReply.get(chatKey);
        if (last != null && nowMs - last < this.cfg.commandCooldownSec * 1000) return;
        this.lastReply.set(chatKey, nowMs);

        const where = {
            chatId: m.chat.id,
            // In a forum, a command typed in a topic is answered there; one
            // typed in General carries no topic.
            threadId: m.is_topic_message ? m.message_thread_id : null,
            replyTo: m.message_id,
        };
        let html;
        try {
            const [status, blocks] = await Promise.all([this.dashboard.status(), this.dashboard.blocks()]);
            const nowSec = Math.floor(nowMs / 1000);
            html = msg.poolStatus({
                status, blocks, nowSec,
                flowing: this.#flowing(status, nowSec),
                poolName: this.cfg.poolName, publicUrl: this.cfg.publicUrl,
            });
        } catch (e) {
            this.log.warn(`commands: /pool_status could not read the dashboard: ${e.message}`);
            html = msg.statusUnavailable({ poolName: this.cfg.poolName });
        }
        await this.telegram.send(html, where);
    }

    /* "/pool_status", "/pool_status@this_bot", with or without arguments;
     * never "/pool_status@another_bot". */
    #isPoolStatus(text) {
        const match = /^\/pool_status(?:@(\w+))?(?:\s|$)/i.exec(text ?? '');
        if (!match) return false;
        return match[1] == null || this.username == null
            || match[1].toLowerCase() === this.username.toLowerCase();
    }

    #allowedChat(chat) {
        const want = String(this.cfg.chatId);
        if (String(chat.id) === want) return true;
        return chat.username != null && want.toLowerCase() === `@${chat.username}`.toLowerCase();
    }

    /* Same rule as the poster's liveness check, without its debounce: this
     * answers about right now. */
    #flowing(status, nowSec) {
        const last = status.pool?.last_share_ts;
        return last != null && nowSec - last < this.cfg.staleSharesSec;
    }
}

function defaultSleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}
