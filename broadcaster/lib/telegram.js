/* Minimal Telegram Bot API client: the three calls a channel poster needs.
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
    constructor({ token, chatId, fetchImpl = fetch, sleep = defaultSleep, minGapMs = MIN_GAP_MS }) {
        this.base = `https://api.telegram.org/bot${token}`;
        this.chatId = chatId;
        this.fetch = fetchImpl;
        this.sleep = sleep;
        this.minGapMs = minGapMs;
        this.queue = Promise.resolve();
        this.lastSent = 0;
    }

    /* Returns the new message's id. */
    async send(html) {
        const r = await this.call('sendMessage', {
            chat_id: this.chatId,
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

            let res, json;
            try {
                res = await this.fetch(`${this.base}/${method}`, {
                    method: 'POST',
                    headers: { 'content-type': 'application/json' },
                    body: JSON.stringify(body),
                });
                json = await res.json();
            } catch (e) {
                // fetch errors can embed the URL; keep only the cause.
                throw new TelegramError('network', e.cause?.code || e.name || 'request failed');
            }
            if (json.ok) return json.result;

            const retryAfter = json.parameters?.retry_after;
            if (res.status === 429 && retryAfter && attempt < MAX_RETRIES) {
                await this.sleep(retryAfter * 1000);
                continue;
            }
            throw new TelegramError(json.error_code ?? res.status, json.description ?? 'unknown error');
        }
    }
}

/* Stands in for TelegramClient under BROADCASTER_DRY_RUN=1. */
export class DryRunClient {
    constructor({ out = console.log } = {}) {
        this.out = out;
        this.nextId = 1;
    }
    async send(html) {
        const id = this.nextId++;
        this.out(`--- post #${id} ---\n${html}\n`);
        return id;
    }
    async edit(messageId, html) { this.out(`--- edit #${messageId} ---\n${html}\n`); }
    async pin(messageId) { this.out(`--- pin #${messageId} ---`); }
}

function defaultSleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}
