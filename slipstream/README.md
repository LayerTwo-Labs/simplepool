# simplepool-slipstream

Takes a raw transaction from anyone and gets it into the pool's blocks
**without relaying it**. The first use is BIP300/301 transactions that the
network will not relay, such as deposits, withdrawal bundles and BMM requests,
but any consensus-valid transaction qualifies.

```
submitter ──POST /api/tx──▶ slipstream ──submitslipstreamtx──▶ enforcer :8122 ◀── simplepool proxy
                                 │                              (template mempool)       (mines it)
                                 └── follows it: template → block → confirmed
```

The transaction goes straight to the enforcer's block template server, the
same one the proxy mines from. The enforcer has the node check it for
consensus validity and keeps it in its template mempool. It is never handed to
the node's mempool and never relayed. The node's relay policy does not apply,
but consensus does, and so do the enforcer's own BIP300 rules: a deposit that
skips the sidechain's CTIP is refused.

**Requires** `bip300301_enforcer` running with `--enable-block-template-server
--enable-slipstream` (LayerTwo-Labs/bip300301_enforcer#642). The enforcer's
template server has no authentication, so keep `:8122` on loopback. This
service is the public face.

## The fee rule

It follows Slipstream's rule: a transaction must pay **the higher of the
minimum submission rate** (`SLIPSTREAM_MIN_FEE_RATE`, default 1 sat/vB) **and
the current mineable rate**.

- **Mineable rate.** This is read from the template the proxy is mining now.
  While the template has room, it equals the floor. Once the template is full,
  it is the fee rate of the cheapest transaction in it.
- **When it's checked.** The rate is checked once, at submission. Accepted
  transactions then compete for the block by fee rate like any other. A
  transaction that loses its place stays pending until it is mined or expires.

A transaction that passes the enforcer but pays too little is removed from the
enforcer again before the refusal is returned, so it cannot be mined for less
than was asked.

## API

Every endpoint is readable cross-origin.

| | |
|---|---|
| `GET /info.json` (also `/api/info`) | The pool, for pool directories. |
| `GET /api/fees` | `{min_submission_rate, mineable_rate, required_rate, template_height, template_weight, updated_at}` |
| `POST /api/tx` (also `/tx`) | The raw transaction as hex, either as a text body or as JSON `{"hex": "..."}`. Returns `{accepted: true, txid, status, ...}` or `{accepted: false, reject_reason}`. Reject reasons are the node's (`missing-inputs`, `bad-txns-...`, `mandatory-script-verify-flag-failed ...`) or this service's (`fee-rate-too-low`, `invalid-hex`, `tx-size`, `rate-limited`, `rejected-by-enforcer`). |
| `GET /api/tx/:txid` | `{tx, events}`: where it stands now, and every status change it went through. |
| `GET /api/txs?status=&limit=` | Recent transactions, newest first. |
| `GET /healthz` | `503` while the enforcer is unreachable. |

```
curl -s -X POST --data-binary "$RAW_TX_HEX" https://slipstream.example/api/tx
```

## What "propagated" means here

Nothing is ever relayed, so the statuses track only two things: whether the
transaction reached a template, and whether a block carried it.

| status | |
|---|---|
| `pending` | In the enforcer's mempool, but not in the latest template. |
| `in_template` | In the latest template, so miners are working on it now. |
| `mined` | In a block, fewer than `SLIPSTREAM_CONFIRMATIONS` (default 6) deep. `mined_by_pool` says whether the block was ours (from `blocks_found`). |
| `confirmed` | That deep. |
| `dropped` | Will not be mined. `status_reason` says why: `conflict_mined` (a block spent one of its inputs), `parent_removed`, `rejected_by_enforcer`, `withdrawn`, or the node's reason for refusing a resubmission. |
| `expired` | Waited `SLIPSTREAM_EXPIRY_BLOCKS` (default 144) without being mined, and was withdrawn. |

This service keeps the record, and the enforcer keeps only its mempool. When
the enforcer loses a transaction, the service resubmits it and counts it in
`resubmissions`. That covers:
- an enforcer restart
- a reorg, because the enforcer evicts every slipstream transaction when a block disconnects
- the transaction's block being orphaned

## Storage

The service keeps its own database, `SLIPSTREAM_DB_PATH` (default
`../data/slipstream.db`). It **never writes to `shares.db`**, which it reads
only for `pool_meta` and `blocks_found`.

- `slipstream_submissions`: every POST exactly as it arrived, accepted or not.
- `slipstream_txs`: one row per accepted transaction, including the raw
  transaction so it can be resubmitted.
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

## Run

```
cd slipstream && npm ci
ENFORCER_GBT_URL=http://127.0.0.1:8122 \
BITCOIND_RPC_URL=http://127.0.0.1:8332 BITCOIND_RPC_COOKIE_FILE=/var/lib/bitcoind/.cookie \
PROXY_DB_PATH=../data/shares.db \
PUBLIC_SLIPSTREAM_URL=https://slipstream.example \
node index.js
```

Without `BITCOIND_RPC_URL`, a mined transaction's depth is only estimated
from the template height, and an orphaned block goes unnoticed. The full list
of variables is in [`lib/config.js`](lib/config.js). The service listens on
`127.0.0.1:8124`, and is published through nginx with
`SLIPSTREAM_TRUST_PROXY=1`, so the rate limit applies per client.

## Tests

- `npm test`: unit tests. These run against a fake enforcer and bitcoind, and
  cover every status transition.
- `tests/test_slipstream_regtest.sh`: end to end, against a real enforcer
  (see [`tests/README.md`](../tests/README.md)).
