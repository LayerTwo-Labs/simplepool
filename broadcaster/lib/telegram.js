/* Minimal Telegram Bot API client: what a channel poster needs, plus the
 * long poll that brings in /pool_status (BROADCASTER_COMMANDS=1).
 *
 * Every call goes through one queue, spaced MIN_GAP_MS apart. Telegram allows
 * about 20 posts a minute into one chat; the broadcaster never comes close,
 * but a burst (several blocks found while the service was down) should queue
 * rather than earn a 429.
 *
 * The token is part of the request URL, so no error raised here ever carries
 * the URL — only Telegram's own description.
 */

const MIN_GAP_MS = 3000;
const MAX_RETRIES = 3;

export class TelegramError extends Error {
    constructor(code, description) {
        super(`telegram ${code}: ${description}`);
        this.code = code;
        this.description = description;
    }
}

export class TelegramClient {
    constructor({ token, chatId, threadId = null, fetchImpl = fetch, sleep = defaultSleep, minGapMs = MIN_GAP_MS }) {
        this.base = `https://api.telegram.org/bot${token}`;
        this.chatId = chatId;
        this.threadId = threadId;
        this.fetch = fetchImpl;
        this.sleep = sleep;
        this.minGapMs = minGapMs;
        this.queue = Promise.resolve();
        this.lastSent = 0;
    }

    /* Returns the new message's id. Only a send names the topic: edits and
     * pins address a message id, which already belongs to one.
     *
     * A command reply overrides the destination: `chatId` and `threadId` are
     * where the command was typed, and `replyTo` quotes it. */
    async send(html, { chatId = this.chatId, threadId = this.threadId, replyTo = null } = {}) {
        const r = await this.call('sendMessage', {
            chat_id: chatId,
            ...(threadId != null && { message_thread_id: threadId }),
            ...(replyTo != null && {
                reply_parameters: { message_id: replyTo, allow_sending_without_reply: true },
            }),
            text: html,
            parse_mode: 'HTML',
            link_preview_options: { is_disabled: true },
        });
        return r.message_id;
    }

    async edit(messageId, html) {
        try {
            await this.call('editMessageText', {
                chat_id: this.chatId,
                message_id: messageId,
                text: html,
                parse_mode: 'HTML',
                link_preview_options: { is_disabled: true },
            });
        } catch (e) {
            // Nothing changed since the last refresh: not a failure.
            if (e instanceof TelegramError && /message is not modified/i.test(e.description)) return;
            throw e;
        }
    }

    async pin(messageId) {
        await this.call('pinChatMessage', {
            chat_id: this.chatId,
            message_id: messageId,
            disable_notification: true,
        });
    }

    async getMe() {
        return this.call('getMe', {});
    }

    /* Puts the commands in the "/" menu, so a tap sends "/pool_status@bot",
     * which a bot in privacy mode is always shown. */
    async setMyCommands(commands) {
        await this.call('setMyCommands', { commands });
    }

    /* Long poll for new messages. NOT queued: it holds the connection open
     * for up to `timeoutSec`, and a post must not wait behind it. Telegram
     * allows one getUpdates at a time per bot, and none while a webhook is
     * set (409 Conflict). */
    async getUpdates({ offset, timeoutSec = 25 }) {
        const { res, json } = await this.#request('getUpdates', {
            offset, timeout: timeoutSec, allowed_updates: ['message'],
        }, AbortSignal.timeout((timeoutSec + 15) * 1000));
        if (json.ok) return json.result;
        throw new TelegramError(json.error_code ?? res.status, json.description ?? 'unknown error');
    }

    call(method, body) {
        const run = this.queue.then(() => this.#callNow(method, body));
        this.queue = run.catch(() => {});
        return run;
    }

    async #callNow(method, body) {
        for (let attempt = 0; ; attempt++) {
            const wait = this.lastSent + this.minGapMs - Date.now();
            if (wait > 0) await this.sleep(wait);
            this.lastSent = Date.now();

            const { res, json } = await this.#request(method, body);
            if (json.ok) return json.result;

            const retryAfter = json.parameters?.retry_after;
            if (res.status === 429 && retryAfter && attempt < MAX_RETRIES) {
                await this.sleep(retryAfter * 1000);
                continue;
            }
            throw new TelegramError(json.error_code ?? res.status, json.description ?? 'unknown error');
        }
    }

    async #request(method, body, signal) {
        try {
            const res = await this.fetch(`${this.base}/${method}`, {
                method: 'POST',
                headers: { 'content-type': 'application/json' },
                body: JSON.stringify(body),
                ...(signal && { signal }),
            });
            return { res, json: await res.json() };
        } catch (e) {
            // fetch errors can embed the URL; keep only the cause.
            throw new TelegramError('network', e.cause?.code || e.name || 'request failed');
        }
    }
}

/* Stands in for TelegramClient under BROADCASTER_DRY_RUN=1. */
export class DryRunClient {
    constructor({ out = console.log } = {}) {
        this.out = out;
        this.nextId = 1;
    }
    async send(html, { replyTo = null } = {}) {
        const id = this.nextId++;
        this.out(`--- post #${id}${replyTo != null ? ` (reply to #${replyTo})` : ''} ---\n${html}\n`);
        return id;
    }
    async edit(messageId, html) { this.out(`--- edit #${messageId} ---\n${html}\n`); }
    async pin(messageId) { this.out(`--- pin #${messageId} ---`); }
}

function defaultSleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}
