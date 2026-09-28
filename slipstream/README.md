# simplepool-slipstream

Takes a raw transaction from anyone and gets it into the pool's blocks. That
includes transactions the network will not relay: BIP300/301 transactions
(deposits, withdrawal bundles, BMM requests) or any other consensus-valid
transaction.

```
submitter ──POST /api/tx──▶ slipstream ──testmempoolaccept, sendrawtransaction──▶ bitcoind
                                 │                                                  │ ZMQ mempool mirror
                                 │                                                  ▼
                                 └── reads the template ◀──────────────── enforcer :8122 ◀── simplepool proxy
```

Each transaction goes to the pool's own bitcoind, the node the enforcer syncs
its template mempool from. Once the node accepts it, it reaches the templates
the proxy mines with no further step and **no enforcer change**. The
enforcer's own BIP300 rules still apply to it on the way in.

## The node

- **Non-standard transactions** need the node to run **`-acceptnonstdtxn=1`**.
  Core refuses that flag on mainnet with `acceptnonstdtxn is not currently
  supported for main chain`. This was checked against the drivechain-patched,
  ecash betanet and stock builds. It works on signet and regtest.
  - drynet3 reports itself as `main`, so non-standard transactions there need a
    patched Core that lifts the check.
- **BIP300 deposits** are standard on a drivechain-patched node already, so
  they need no flag.
- **Transactions are relayed.** The node broadcasts an accepted transaction
  like any other, so another pool can mine it. `mined_by_pool` records whose
  block it was.
- **Nothing can be withdrawn.** Core has no RPC to remove a transaction from
  its mempool, which is why the fee rule is applied *before* broadcast.

## The fee rule

It follows Slipstream's rule: a transaction must pay **the higher of the
minimum submission rate** (`SLIPSTREAM_MIN_FEE_RATE`, default 1 sat/vB) **and
the current mineable rate**.

- **Mineable rate.** This is read from the template the proxy is mining now.
  While the template has room, it equals the floor. Once the template is full,
  it is the fee rate of the cheapest transaction in it.
- **Checked before anything is sent.** `testmempoolaccept` reports the fee and
  size without broadcasting, and only a transaction that passes both the node
  and the rule reaches `sendrawtransaction`.

## API

Every endpoint is readable cross-origin.

| | |
|---|---|
| `GET /info.json` (also `/api/info`) | The pool, for pool directories. |
| `GET /api/fees` | `{min_submission_rate, mineable_rate, required_rate, template_height, template_weight, updated_at}` |
| `POST /api/tx` (also `/tx`) | The raw transaction as hex, either as a text body or as JSON `{"hex": "..."}`. Returns `{accepted: true, txid, status, ...}` or `{accepted: false, reject_reason}`. Reject reasons are the node's (`missing-inputs`, `bad-txns-...`, `mandatory-script-verify-flag-failed ...`, `scriptpubkey` when the node does not run `-acceptnonstdtxn`) or this service's (`fee-rate-too-low`, `invalid-hex`, `tx-size`, `rate-limited`). |
| `GET /api/tx/:txid` | `{tx, events}`: where it stands now, and every status change it went through. |
| `GET /api/txs?status=&limit=` | Recent transactions, newest first. |
| `GET /healthz` | `503` while the enforcer is unreachable. |

```
curl -s -X POST --data-binary "$RAW_TX_HEX" https://slipstream.example/api/tx
```

## Statuses

| status | |
|---|---|
| `pending` | In the node's mempool, but not in the latest template. |
| `in_template` | In the latest template, so the pool's miners are working on it now. |
| `mined` | In a block, fewer than `SLIPSTREAM_CONFIRMATIONS` (default 6) deep. `mined_by_pool` says whose block it was (from `blocks_found`). |
| `confirmed` | That deep. |
| `dropped` | Left the mempool unmined, and the node refused it when it was sent again. `status_reason` is the node's reason, e.g. `insufficient fee, rejecting replacement ...` or `bad-txns-inputs-missingorspent`. |

- **Leaving the mempool.** A transaction can leave the mempool without being
  mined: evicted, expired, replaced, or its input spent by a block. The
  service then sends it again, and the node's answer decides between `pending`
  and `dropped`.
- **Orphaned blocks.** When a transaction's block is orphaned, the node puts
  it back in its mempool itself, and the transaction becomes `pending` again.

## Storage

The service keeps its own database, `SLIPSTREAM_DB_PATH` (default
`../data/slipstream.db`). It **never writes to `shares.db`**, which it reads
only for `pool_meta` and `blocks_found`.

- `slipstream_submissions`: every POST exactly as it arrived, accepted or not.
- `slipstream_txs`: one row per accepted transaction, including the raw
  transaction so it can be sent again.
- `slipstream_events`: every status change, append-only.

## info.json

Facts about the pool are read from `pool_meta`, which the proxy writes: `mode`
(the exact `pool_mode`), `fee_bps`, `coinbase_tag`, `operator_address` and
`pool_btc_address`. They are never configured here, so they cannot disagree
with what the coinbase actually does. Presentation fields come from
environment variables:
- `POOL_NAME`, `POOL_OPERATOR`, `POOL_LOGO`, `POOL_CONTACT`
- `POOL_CHAIN`, for a name like `betanet` that `pool_meta.network` cannot express
- `POOL_PAYOUT_TEXT`, which overrides the default sentence for each mode
- `PUBLIC_STRATUM_URL`, `PUBLIC_DASHBOARD_URL`, `PUBLIC_SLIPSTREAM_URL`

`status_url` is the dashboard's `/api/status`.

## Install (systemd + nginx)

The installer does not set this up yet. The unit and the vhost are
templates in the repository; from the pool checkout (`ROOT`), as root:

```
cd ROOT/slipstream && sudo -u <pool user> npm ci --omit=dev

sed -e "s|@USER@|<pool user>|g" -e "s|@ROOT@|ROOT|g" \
    ROOT/deploy/systemd/simplepool-slipstream.service \
    > /etc/systemd/system/simplepool-slipstream.service
$EDITOR /etc/systemd/system/simplepool-slipstream.service   # RPC credentials, POOL_*, PUBLIC_*
systemctl daemon-reload && systemctl enable --now simplepool-slipstream

sed "s/slipstream\.example/<your host>/g" ROOT/deploy/nginx/slipstream.conf \
    > /etc/nginx/sites-available/<your host>
ln -s /etc/nginx/sites-available/<your host> /etc/nginx/sites-enabled/
nginx -t && systemctl reload nginx
certbot --nginx -d <your host>
```

The vhost overwrites `X-Forwarded-For` with the connecting address, and the
service (with `SLIPSTREAM_TRUST_PROXY=1`) keys its rate limit on the last hop
of that header, so a client cannot pick its own key. It reuses the
dashboard's `pool_dash` rate-limit zone from `deploy/nginx/pool-ratelimit.conf`,
which the installer already puts in `conf.d/`.

To show the Slipstream page on the dashboard, add to
`simplepool-dashboard.service` (or its `local.conf` drop-in) and restart it:

```
Environment=SLIPSTREAM_API_URL=http://127.0.0.1:8124
Environment=PUBLIC_SLIPSTREAM_URL=https://<your host>
```

Check it:

```
curl -s https://<your host>/healthz
curl -s https://<your host>/api/fees
curl -s https://<your host>/info.json
journalctl -u simplepool-slipstream -f
```

## Run by hand

```
cd slipstream && npm ci
BITCOIND_RPC_URL=http://127.0.0.1:8332 BITCOIND_RPC_COOKIE_FILE=/var/lib/bitcoind/.cookie \
ENFORCER_GBT_URL=http://127.0.0.1:8122 \
PROXY_DB_PATH=../data/shares.db \
PUBLIC_SLIPSTREAM_URL=https://slipstream.example \
node index.js
```

The full list of variables is in [`lib/config.js`](lib/config.js). The service
listens on `127.0.0.1:8124`.

## Tests

- `npm test`: unit tests. These run against a fake bitcoind and enforcer, and
  cover every status transition.
- `tests/test_slipstream_regtest.sh`: end to end, against a real node and the
  stock enforcer. It runs in CI.
