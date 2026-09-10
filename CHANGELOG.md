# Changelog

Notable changes per release. The newest version is first; each section is what
the release workflow publishes as that release's notes, above the install
boilerplate.

Anything that changes what a miner is paid, or what an operator has to tell
their miners, is called out explicitly — those are the changes that cost
somebody money if they go unread.

## 0.4.0 — three PPLNS modes, and coinbase-direct payouts

The headline is that a pool no longer has to hold miners' money to run PPLNS.

### Three new pool modes

`pool_mode` gains `pplns-thunder`, `pplns-btc` and `pplns-coinbase`, alongside
the existing `solo` and `pps-classic`. All five are documented in
[README](README.md#the-five-modes), with a sequence diagram each in
[docs/simplepool.html](docs/simplepool.html).

PPLNS divides a block among the shares that produced it, so **the pool never
owes more than it has just been paid**. There is no operator reserve to fund
and operator ruin is not a failure mode — the trade is that miners carry the
variance, which is why the fee is normally set lower than on PPS.

- **`pplns-thunder`** settles over Thunder, reusing the existing payout worker.
- **`pplns-btc`** settles on Bitcoin L1 through the enforcer's own wallet.
  Needs `bip300301_enforcer --enable-wallet` and `PAYOUT_RAIL=btc`.
- **`pplns-coinbase`** settles in the block itself.

### `pplns-coinbase`: the pool never receives the reward

The block's own coinbase pays the entire window, one output per miner. No pool
wallet, no payout worker, no ledger row, no maturity wait. A reorged block
simply never paid, so there is nothing to claw back.

**What operators must tell their miners.** A coinbase has a fixed budget of
bytes, so one block cannot pay everyone in a large window. Two limits decide
who it pays — `coinbase_max_bytes` (default 1000) and
`pplns_payout_floor_sats` (default 546, the dust limit).

A claim that clears neither is **shared out among the miners that block could
pay** — never the operator, who takes only its fee at every byte budget. The
skipped miner then goes **first in the queue** for the next block: a quarter of
every coinbase's payout slots are reserved for whoever has waited longest.

So being a small miner here costs **frequency, not money**. That is the single
sentence to put on a pool page, and the proxy states the floor at startup, per
template, per block, and on the dashboard before a miner connects.

The queue lives in `pplns_fractions`: a signed fraction of one block reward per
worker, summing to zero. **It is not a balance and the pool holds nothing
against it** — delete the table and nobody is owed a payment, the pool only
forgets whose turn it was. Rows are staged when a block is found and applied
only once it confirms, so an orphaned block rotates nobody.

`coinbase_max_bytes` is settable **per listener**, and usually should be: the
ceiling is a marketplace rule that binds only on the port rented hashrate
connects to, and every byte of it costs a payout.

```
coinbase_max_bytes = 3000
listener = port=3335 label=rental min_diff=500000 initial_diff=500000 max_coinbase_bytes=900
```

### Safety

- **The window walk is bounded.** Reading the PPLNS window used to re-scan the
  entire `shares` table on every template — 250 ms per million rows, on the
  template thread. It now walks back in bounded batches: flat in history size
  rather than linear (8 M rows: 1033 ms → 1.08 ms).
- **A walk that cannot prove it covered the window returns an error**, and the
  pool publishes no job rather than a wrong one. Miners keep working the last
  job until it recovers. In this mode a wrong window is mined into a coinbase
  and published, so there is no later pass that could notice.
- **The payout-slot estimate charges each address what it costs.** It decides
  how many slots are reserved for long-waiting miners; assuming a fixed 31
  bytes was over by 24 slots on a window of taproot addresses at a 3000-byte
  budget, reserving a third of the coinbase where a quarter was meant. Now
  within 2 slots across every budget and address type tested, and never over.
- **Store transactions are serialised.** The store shares one SQLite connection
  across three threads and nothing guarded it: `BEGIN IMMEDIATE` failed
  outright when another was mid-transaction, dropping the write with only a
  warning. `store_pplns_distribute` was affected too, surviving on being
  retried each tip. A single mutex is now held across each transaction, so a
  write waits for at most one batch instead of losing to it.

### Dashboard

- Every mode gets its own guidance on the "About the numbers" card. Previously
  all three PPLNS modes fell through to *"this pool has not published its mode
  yet"*, directly beneath a header that named the mode correctly.
- Three places answered "not `pps-classic`" with the word *solo*: the worker
  page's **Owed** field, the templates page's PPS rate, and the
  `pps_difficulty` health check.
- **Pool solvency** counted `blocks_found.reward_sats` as pool revenue in
  `pplns-coinbase`, where that is what the block paid the *miners* — reporting
  a healthy margin for a pool that holds nothing. Now skipped, with the reason.

### Testing

One end-to-end regtest suite per mode, all in CI, each mining a real chain —
including `solo`, which had none anywhere despite being the default. See
[tests/README.md](tests/README.md).

### Upgrading from 0.3.0

Nothing is required: `solo` and `pps-classic` are unchanged, and the new
`pool_meta` and `pplns_*` tables are created on open. To adopt a PPLNS mode,
set `pool_mode` and read that mode's section in
[INSTALL.md](INSTALL.md) — `pplns-coinbase` in particular refuses
`pool_btc_address`, because it has no pool wallet at all.

## 0.3.0 and earlier

See the [release list](https://github.com/LayerTwo-Labs/simplepool/releases).
