/* One poll of the dashboard, turned into channel posts.
 *
 * Each tick reads /api/status and the latest page of /api/blocks, then:
 *   - announces blocks it has not seen, and blocks it announced that were
 *     later orphaned or rejected
 *   - announces shares stopping / resuming, and ledger health failing /
 *     recovering, once the new state has held for `debounce` polls
 *   - posts the daily digest once per UTC day, at or after DIGEST_UTC_HOUR
 *   - refreshes the pinned live message (BROADCASTER_LIVE=1)
 *
 * State is saved after every post, so a failure halfway through a tick
 * retries only what was not sent.
 */

import * as msg from './messages.js';
import { TelegramError } from './telegram.js';
import { saveState } from './state.js';

const LIVE_STATUSES = new Set(['pending', 'confirmed']);
const LOST_STATUSES = new Set(['orphaned', 'rejected']);

export class Broadcaster {
    constructor({ dashboard, telegram, state, cfg, log, persist }) {
        this.dashboard = dashboard;
        this.telegram = telegram;
        this.state = state;
        this.cfg = cfg;
        this.log = log;
        this.persist = persist ?? (() => saveState(cfg.statePath, this.state));
        // Debounce counters. In memory on purpose: after a restart a state
        // change has to hold for `debounce` fresh polls again.
        this.candidate = { shares: null, health: null };
    }

    async tick(nowMs = Date.now()) {
        const nowSec = Math.floor(nowMs / 1000);
        const status = await this.dashboard.status();
        const blocks = await this.dashboard.blocks();

        if (!this.state.initialized) {
            this.#seed(status, blocks, nowMs);
        } else {
            await this.#blocks(blocks);
            await this.#liveness(status, nowSec);
            await this.#health(status);
            await this.#digest(status, blocks, nowMs);
        }
        await this.#live(status, nowMs);
    }

    /* First run: remember everything as already said. */
    #seed(status, blocks, nowMs) {
        for (const b of blocks) this.state.blocks[b.hash] = b.status;
        this.state.sharesFlowing = this.#flowing(status, Math.floor(nowMs / 1000));
        this.state.healthOk = this.#healthOk(status);
        const now = new Date(nowMs);
        if (this.cfg.digestHour != null && now.getUTCHours() >= this.cfg.digestHour) {
            this.state.lastDigestDate = utcDate(now);
        }
        this.state.initialized = true;
        this.persist();
        this.log.info(`seeded: ${blocks.length} existing block(s) will not be announced`);
    }

    async #blocks(blocks) {
        // Oldest first, so posts read in the order the blocks were found.
        for (const b of [...blocks].reverse()) {
            const prev = this.state.blocks[b.hash];
            if (prev === b.status) continue;
            if (prev === undefined && LIVE_STATUSES.has(b.status)) {
                await this.#post(msg.blockFound({ block: b, ...this.#ctx() }));
            } else if (LIVE_STATUSES.has(prev) && LOST_STATUSES.has(b.status)) {
                await this.#post(msg.blockLost({ block: b, ...this.#ctx() }));
            }
            // Anything else (pending -> confirmed, or a block first seen
            // already lost) is recorded without a post.
            this.state.blocks[b.hash] = b.status;
            this.persist();
        }
    }

    async #liveness(status, nowSec) {
        const flowing = this.#flowing(status, nowSec);
        if (!this.#settled('shares', flowing, this.state.sharesFlowing)) return;
        await this.#post(flowing
            ? msg.sharesResumed({ status, ...this.#ctx() })
            : msg.sharesStopped({ lastShareTs: status.pool.last_share_ts, nowSec, ...this.#ctx() }));
        this.state.sharesFlowing = flowing;
        this.persist();
    }

    async #health(status) {
        const ok = this.#healthOk(status);
        if (!this.#settled('health', ok, this.state.healthOk)) return;
        await this.#post(ok
            ? msg.healthRecovered(this.#ctx())
            : msg.healthFailing({ health: status.health, ...this.#ctx() }));
        this.state.healthOk = ok;
        this.persist();
    }

    async #digest(status, blocks, nowMs) {
        if (this.cfg.digestHour == null) return;
        const now = new Date(nowMs);
        const today = utcDate(now);
        if (now.getUTCHours() < this.cfg.digestHour || this.state.lastDigestDate === today) return;
        await this.#post(msg.digest({ status, blocks, nowSec: Math.floor(nowMs / 1000), ...this.#ctx() }));
        this.state.lastDigestDate = today;
        this.persist();
    }

    async #live(status, nowMs) {
        if (!this.cfg.live || nowMs - this.state.liveUpdatedAt < this.cfg.liveMs) return;
        const html = msg.live({
            status,
            nowSec: Math.floor(nowMs / 1000),
            flowing: this.state.sharesFlowing !== false,
            ...this.#ctx(),
        });
        if (this.state.liveMessageId != null) {
            try {
                await this.telegram.edit(this.state.liveMessageId, html);
            } catch (e) {
                // Deleted from the channel by hand: post a fresh one below.
                if (!(e instanceof TelegramError && /message to edit not found/i.test(e.description))) throw e;
                this.log.warn('pinned live message is gone; posting a new one');
                this.state.liveMessageId = null;
            }
        }
        if (this.state.liveMessageId == null) {
            this.state.liveMessageId = await this.telegram.send(html);
            this.persist();
            try {
                await this.telegram.pin(this.state.liveMessageId);
            } catch (e) {
                this.log.warn(`could not pin the live message (does the bot have "Pin messages"?): ${e.message}`);
            }
        }
        this.state.liveUpdatedAt = nowMs;
        this.persist();
    }

    /* True when `value` differs from what was last announced and has now
     * held for `debounce` consecutive polls. A null `value` (not knowable
     * this poll) neither counts nor resets. A null `announced` (nothing
     * known yet) is filled in silently. */
    #settled(key, value, announced) {
        if (value == null) return false;
        if (announced == null) {
            if (key === 'shares') this.state.sharesFlowing = value;
            else this.state.healthOk = value;
            this.persist();
            return false;
        }
        if (value === announced) {
            this.candidate[key] = null;
            return false;
        }
        const c = this.candidate[key];
        this.candidate[key] = c && c.value === value ? { value, n: c.n + 1 } : { value, n: 1 };
        if (this.candidate[key].n < this.cfg.debounce) return false;
        this.candidate[key] = null;
        return true;
    }

    #flowing(status, nowSec) {
        if (!status.pool?.db_ready) return null;
        const last = status.pool.last_share_ts;
        return last != null && nowSec - last < this.cfg.staleSharesSec;
    }

    #healthOk(status) {
        const h = status.health;
        if (!h || h.status === 'checking') return null;
        return Boolean(h.ok);
    }

    #ctx() {
        return { poolName: this.cfg.poolName, publicUrl: this.cfg.publicUrl };
    }

    async #post(html) {
        await this.telegram.send(html);
    }
}

function utcDate(d) {
    return d.toISOString().slice(0, 10);
}
