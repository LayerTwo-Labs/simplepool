# simplepool-broadcaster

Posts the pool's stats to a Telegram channel, for the people who follow the
pool there.

```
simplepool ──▶ shares.db ──▶ dashboard :8081 ──/api/status, /api/blocks──▶ broadcaster ──▶ api.telegram.org ──▶ channel
                                                                               │
                                                                        broadcaster.json
```

It reads the dashboard's public JSON API and nothing else, so **every number
it posts is the number the dashboard shows**. It never opens `shares.db`, and
if Telegram or the service goes down, the pool is unaffected.

## What it posts

| | when |
|---|---|
| **Daily digest** | once per UTC day at `DIGEST_UTC_HOUR` (default 12): hashrate 1h/24h, active workers, shares and reject rate, best share, blocks in the last 24h and all time, mode and fee |
| **Block found** | the first time a block appears in `/api/blocks` as `pending` or `confirmed` |
| **Block lost** | a block it announced turns `orphaned` or `rejected` |
| **Pool quiet / back** | no share accepted for `STALE_SHARES_SEC` (default 15m), and when shares resume |
| **Health failing / recovered** | `/health` changes state, with the failing checks listed |
| **Pinned live message** (`BROADCASTER_LIVE=1`) | one message, pinned unless `BROADCASTER_LIVE_PIN=0`, edited every `BROADCASTER_LIVE_MS` (default 5m) with current stats and the last block |
| **`/pool_status`** (`BROADCASTER_COMMANDS=1`) | when someone sends it in the chat: the current stats, as a reply in the same topic, at most once per `BROADCASTER_COMMAND_COOLDOWN_SEC` (default 30) |
| **Periodic update** (`BROADCASTER_SUMMARY_HOURS=N`) | the same stats as a new message every N hours, on UTC boundaries (6 → 00, 06, 12, 18 UTC) |

- **Edited is not posted.** Telegram does not notify anyone of an edit or
  move the message down the chat, so the pinned live message stays where it
  was first posted. Followers who want stats in the feed need the periodic
  update.
- **No repeats, no backlog.** `broadcaster.json` records what was already
  posted. On its very first run the service records every existing block
  and today's digest (if the hour has passed) without posting them, so it
  doesn't post the pool's history.
- **No flapping.** A quiet/back or health change has to hold for
  `HEALTH_DEBOUNCE` consecutive polls (default 3) before it is posted.
- **Failed posts are retried.** State is saved after each post, so a post
  that fails (network, Telegram down) is tried again on the next poll.

## Telegram setup

The bot is only a posting credential. Subscribers never talk to it, and posts
appear under the channel's name and avatar.

1. Message [@BotFather](https://t.me/BotFather), `/newbot`, keep the token.
   Optionally `/setjoingroups` → Disable, so the bot can't be added anywhere
   else.
2. In the channel: *Administrators → Add admin →* the bot. Grant **Post
   messages** only. For `BROADCASTER_LIVE=1` also grant **Edit messages of
   others** and **Pin messages**.
3. `TELEGRAM_CHAT_ID` is `@channelname` for a public channel. For a private
   one, it is the numeric id starting `-100`. Forward any channel post to
   `@userinfobot`, or read `chat.id` from `getUpdates`, to get it.
4. For a group with topics, set `TELEGRAM_THREAD_ID` to post into one topic.
   A message link `https://t.me/c/1518607784/23563` gives both: the chat is
   `-100` + the first number (`-1001518607784`), the topic is the second
   (`23563`). In a group, the bot posts as itself and needs no admin rights
   when members may send (and, for the live message, pin) messages.

### `/pool_status`

With `BROADCASTER_COMMANDS=1` the service also reads the chat, with
`getUpdates`, and answers `/pool_status` with the live message's numbers
plus the last block. It answers only in `TELEGRAM_CHAT_ID`, so adding the
bot to another group does not make it answer there.

- It registers the command in the bot's `/` menu. A bot in privacy mode
  (BotFather's default) always sees `/pool_status@yourbot`, which is what a
  tap in that menu sends, but not always a bare `/pool_status`. Turn privacy
  off (`/setprivacy` → Disable) if people type it by hand.
- Telegram allows one `getUpdates` reader per bot and none while a webhook
  is set. Run one broadcaster per bot, and don't poll the bot from anywhere
  else while it runs.
- The first start skips commands sent before it, and the read position is
  kept in `broadcaster.json`, so a restart answers nothing twice.

Try it without a token first:

```bash
cd broadcaster
BROADCASTER_DRY_RUN=1 DASHBOARD_URL=http://127.0.0.1:8081 \
BROADCASTER_STATE_PATH=/tmp/broadcaster.json node index.js
```

## Config

Environment only. The full list with defaults is in
[`lib/config.js`](lib/config.js).

| | |
|---|---|
| `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID` | required unless `BROADCASTER_DRY_RUN=1` |
| `TELEGRAM_THREAD_ID` | a forum group's topic to post in (default: its General topic) |
| `DASHBOARD_URL` | default `http://127.0.0.1:8081` |
| `PUBLIC_DASHBOARD_URL` | link included in posts |
| `POOL_NAME` | heading of every post (default `simplepool`) |
| `BROADCASTER_STATE_PATH` | default `../data/broadcaster.json` |
| `BROADCASTER_POLL_MS` | default 60000 |
| `DIGEST_UTC_HOUR` | 0–23, or `off` (default 12) |
| `BROADCASTER_LIVE`, `BROADCASTER_LIVE_MS` | pinned live message (default off, 300000) |
| `BROADCASTER_LIVE_PIN` | `0` = post the live message without pinning it (default 1) |
| `BROADCASTER_SUMMARY_HOURS` | 1–168, or `off` (default off) |
| `BROADCASTER_COMMANDS`, `BROADCASTER_COMMAND_COOLDOWN_SEC` | answer `/pool_status` (default off, 30) |
| `STALE_SHARES_SEC` | default 900 |
| `HEALTH_DEBOUNCE` | default 3 |

## Running it

- **Docker:** set `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` in
  `deploy/docker/.env`, then run `docker compose --profile broadcaster up -d`.
- **systemd:** [`deploy/systemd/simplepool-broadcaster.service`](../deploy/systemd/simplepool-broadcaster.service).
  Put the token in a `0600` drop-in, as the unit's comments describe, not in
  the unit itself.

It has no dependencies (Node 20's built-in `fetch`), so `npm install` is
not needed. `npm test` runs the tests.
