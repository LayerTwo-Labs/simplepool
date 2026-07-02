# Pool operator guide (pps-thunder-classic)

Everything you need to run this pool day-to-day. Assumes the branch
already deployed (see `PPS_THUNDER.md` and `CLASSIC_PAYOUTS.md` for
background on why the design looks like this).

---

## Quick reference

Substitute your own pool's public IP/hostname for `<pool-host>` and your
SSH key file for `<ssh-key>` below. On the deployment I built alongside
this doc, the real values are stashed in a gitignored `FORKNET_CHEATSHEET.md`
at the repo root — do NOT paste real hosts / passwords into anything
tracked by git.

| what | where | credentials |
| --- | --- | --- |
| Public dashboard | `http://<pool-host>:8081/` | none |
| **Admin dashboard** | `http://<pool-host>:8081/admin` | **`admin` / `<see /root/simplepool-admin-cred.txt on the box>`** |
| Admin JSON API | `http://<pool-host>:8081/api/admin/summary` | same basic auth |
| Stratum (miner endpoint) | `stratum+tcp://<pool-host>:3334` | username = Thunder base58 address |
| SSH | `root@<pool-host>` | `<ssh-key>` |

The admin password is stashed at `/root/simplepool-admin-cred.txt` on the
box (root-only). To rotate, edit
`/etc/systemd/system/simplepool-dashboard.service.d/pps-thunder.conf`
and `systemctl daemon-reload && systemctl restart simplepool-dashboard`
— there's a scripted version in [Rotating the admin
password](#rotating-the-admin-password) below.

---

## Where things live on the box

- **Root**: `/home/forknet/pps-thunder-test/`
  - `simplepool/` — this repo, checked out at `pps-thunder-classic`
  - `data/shares.db` — pool state (SQLite, WAL mode)
  - `data/archive/` — old DB snapshots from mode switches
  - `logs/` — pool + payout + Thunder logs (plus stashed Thunder mnemonic)
  - `thunder-data/` — Thunder node chain data

- **Services** (all `systemctl status <name>`):
  - `simplepool.service` — the stratum proxy (managed manually right now;
    launched via `nohup` from the deploy). Systemd unit exists but not
    always enabled.
  - `simplepool-payout.service` — Node payout worker
  - `simplepool-dashboard.service` — Node public + admin dashboards

- **Config**: `/home/forknet/pps-thunder-test/simplepool/proxy.conf`
  (gitignored). Current `pool_mode = pps-classic`.

---

## The value flow, end to end

```
    ┌──────────┐   diff × 1000 sats
    │  miner   │─────────────────────────┐
    │  ASIC    │                         ▼
    └────┬─────┘                  ┌────────────────┐
         │ stratum share          │  pps_credits   │
         │                        │  accrued_sats  │
         ▼                        └────────┬───────┘
   ┌───────────┐  block           payout worker
   │ simplepool│  found           ↓ every 30s
   │  :3334    │──────►coinbase ──┘ IF Thunder reserve has funds
   └───────────┘        │
                        │ pays pool_btc_address (BTC P2WPKH)
                        │
                        ▼
                ┌───────────────────┐
                │ pool BTC wallet   │  accumulates BTC over blocks
                │                   │
                └────────┬──────────┘
                         │ operator triggers deposit
                         │ (admin dashboard button OR grpcurl)
                         ▼
                ┌──────────────────────┐
                │ BIP300 deposit tx    │
                │ CreateDepositTx      │  mainchain → sidechain
                └────────┬─────────────┘
                         │ Ctip updates on enforcer
                         ▼
                ┌──────────────────────┐
                │ Thunder wallet       │
                │ available_sats > 0   │
                └────────┬─────────────┘
                         │ payout worker's next tick
                         │ thunder.transfer(miner_addr, owed, fee)
                         ▼
                ┌──────────────────────┐
                │ miner Thunder wallet │
                └──────────────────────┘
```

Everything before `pool_btc_address` runs automatically. Everything
after is either automatic (payout worker) or one operator action
per deposit (moving BTC into Thunder).

---

## Daily operations

### 1. Health check (30 seconds)

Open the admin dashboard. Look at the top three cards:

- **Thunder reserve** — how much you can pay out right now
- **PPS ledger totals** — how much you *owe* (owed = accrued − paid)
- **⚠ Reserve is short** banner — appears when owed > available; means
  the payout worker will keep skipping ticks until you deposit more BTC

If you see a `⚠` banner AND owed is significant, do a deposit (below).

Also check the **In-flight payouts** card — should be empty. Any row
with a set `txid` means a payout crashed mid-flight and needs manual
reconciliation.

### 2. Deposit BTC into Thunder (when reserve is short)

**Currently manual — no admin button yet.** The runbook:

```sh
ssh -i <ssh-key> root@<pool-host>

# 1. Get a Thunder-owned deposit address (formatted for the mainchain wallet)
TCLI=/home/forknet/forknet-software/thunder-rust/target/debug/thunder_app_cli
POOL_ADDR=$(sudo -u forknet $TCLI get-new-address)
DEP=$(sudo -u forknet $TCLI format-deposit-address "$POOL_ADDR")
echo "deposit target: $DEP"

# 2. Trigger the deposit via the enforcer's wallet. AMOUNT + FEE are yours to pick.
GRPCURL=/home/forknet/forknet-software/grpcurl
AMOUNT_SATS=1000000     # 0.01 BTC = 1_000_000 sats
FEE_SATS=1000
$GRPCURL -plaintext \
  -d "{\"sidechain_id\":9,\"address\":\"$DEP\",\"value_sats\":$AMOUNT_SATS,\"fee_sats\":$FEE_SATS}" \
  127.0.0.1:50051 cusf.mainchain.v1.WalletService/CreateDepositTransaction

# 3. Wait for a mainchain block. If you want to force one on the test:
$GRPCURL -plaintext -d '{"blocks":1}' \
  127.0.0.1:50051 cusf.mainchain.v1.WalletService/GenerateBlocks

# 4. Confirm the Ctip moved
$GRPCURL -plaintext -d '{"sidechain_number":9}' \
  127.0.0.1:50051 cusf.mainchain.v1.ValidatorService/GetCtip
```

**About the "enforcer's wallet"**: the enforcer runs its own on-box BTC
wallet, seeded during the setup phase. Its balance is visible via
`WalletService/GetBalance`. Right now it's ~1 BTC because we mined some
funding blocks during activation. **Long-term**, you want the pool
coinbase to accumulate straight into an enforcer-wallet-owned address —
that means changing `pool_btc_address` in `proxy.conf` to an address
generated by `WalletService/CreateNewAddress` on the enforcer. Until
that's done, the operator's own BTC (at the current `pool_btc_address`)
sits in an external wallet the enforcer can't spend directly.

### 3. Watch payouts land

After a deposit completes:

```sh
# tail the payout worker
sudo journalctl -u simplepool-payout.service -f
# or read the log directly:
tail -f /home/forknet/pps-thunder-test/logs/payout.log
```

You should see lines like:
```
payout: 3Z6z1hPySN…rig1 -> 3Z6z1hPySN… 96085 sats txid=…
```

Refresh the admin dashboard — `owed` should drop, `paid` should rise,
`in-flight` should stay empty.

**Known Thunder-side gotcha:** for `payout: reserve short — available=0`
to actually go away, Thunder itself has to mine a sidechain block (BMM)
that recognizes the deposit. Deposits are always visible on the
enforcer's Ctip, but Thunder's own wallet balance stays 0 until BMM
runs. On this box Thunder's `mine` RPC is currently blocking without
producing a sidechain block — likely a BMM-side infrastructure issue
that needs investigation. In the meantime, the pool correctly logs the
short-reserve state and does not double-pay or lose accrual data.

### 4. Reconciling stuck payouts

If the admin dashboard's **In-flight payouts** card is non-empty, each
row means a payout crashed between broadcast and DB finalize. Two cases:

- **`txid = —`** (no txid): the broadcast never happened. Safe to just
  delete the row and let the worker retry next tick:
  ```sh
  sudo -u forknet sqlite3 /home/forknet/pps-thunder-test/data/shares.db \
    "DELETE FROM payouts_in_flight WHERE id = <ID>;"
  ```

- **`txid = <something>`**: the Thunder tx went out; the DB finalize
  crashed. Verify the tx is on Thunder (via `thunder_app_cli
  get-transaction <txid>` or a Thunder wallet viewer). If confirmed,
  finalize by hand:
  ```sh
  sudo -u forknet sqlite3 /home/forknet/pps-thunder-test/data/shares.db "
    BEGIN;
    UPDATE pps_credits SET paid_sats = paid_sats + (SELECT sats FROM payouts_in_flight WHERE id = <ID>)
                       WHERE worker_id = (SELECT worker_id FROM payouts_in_flight WHERE id = <ID>);
    DELETE FROM payouts_in_flight WHERE id = <ID>;
    COMMIT;
  "
  ```

The pool never auto-resolves in-flight rows because it can't safely
tell apart "broadcast didn't happen" from "broadcast happened,
finalize crashed" without Thunder-side mempool/chain lookup.

---

## Onboarding a new miner

Nothing on the pool side. The miner points their ASIC firmware at:

```
stratum+tcp://<pool-host>:3334
username: <their-Thunder-address>[.<rig_label>]
password: (anything)
```

The username **must** be a valid Thunder address — base58 of a 20-byte
hash. If they don't have a Thunder wallet, tell them to run a Thunder
node and do `thunder-cli get-new-address`.

A Bitcoin address as the username produces `invalid thunder address:
thunder address decoded length 25 (expected 20)` on authorize and no
shares accrue. Rejects show up in the admin dashboard's implicit
`rejects` table (surfaced on the per-worker page) so you can help
someone debug their config.

**Same address, multiple rigs:** append `.<rig_label>` (e.g.
`3Z6z1hPySN….basement`, `3Z6z1hPySN….garage`). They show as separate
rows on the workers page but the same payout address on the admin
view.

---

## Rotating the admin password

```sh
ssh -i <ssh-key> root@<pool-host>
NEW_PASS=$(openssl rand -base64 21 | tr -d '=+/')
sed -i "s/^Environment=ADMIN_PASSWORD=.*/Environment=ADMIN_PASSWORD=$NEW_PASS/" \
  /etc/systemd/system/simplepool-dashboard.service.d/pps-thunder.conf
echo "admin / $NEW_PASS" > /root/simplepool-admin-cred.txt
systemctl daemon-reload
systemctl restart simplepool-dashboard.service
cat /root/simplepool-admin-cred.txt
```

Old password stops working immediately; the dashboard is up in ~1s.

---

## Rolling back / switching modes

To pause and restore the old solo pool exactly as it was:

```sh
# stop pps-classic pool
pkill -9 -x simplepool
# start the pre-existing solo unit (still installed)
systemctl start simplepool.service
```

That points at the OLD proxy.conf at `~/forknet-software/simplepool/`.
The dashboard drop-in stays pointed at the pps-classic DB by default;
edit `/etc/systemd/system/simplepool-dashboard.service.d/pps-thunder.conf`
if you want to swing it back to the solo DB.

---

## Troubleshooting

### "invalid thunder address" reject storm

A miner is using a BTC address. Have them update their firmware config
to use a Thunder address. Until they do, they burn hashrate for zero
credit — the pool has no fallback.

### Payout worker log shows `reserve short — available=0` forever

Either (a) you haven't done a deposit yet (see runbook above), or (b)
you deposited but Thunder hasn't seen the credit yet due to BMM not
running. Check with:

```sh
# enforcer says the deposit was consensus-accepted?
grpcurl -plaintext -d '{"sidechain_number":9}' \
  127.0.0.1:50051 cusf.mainchain.v1.ValidatorService/GetCtip
# Thunder wallet sees the balance?
sudo -u forknet /home/forknet/forknet-software/thunder-rust/target/debug/thunder_app_cli balance
```

If Ctip shows the deposit but Thunder wallet balance is 0, it's a
BMM/Thunder-side sync issue. Escalate to Thunder infra.

### Dashboard shows the OLD DB (pre-pps-classic reset)

Check the drop-in override:
```sh
cat /etc/systemd/system/simplepool-dashboard.service.d/pps-thunder.conf
# PROXY_DB_PATH should be /home/forknet/pps-thunder-test/data/shares.db
```
Fix + `systemctl daemon-reload && systemctl restart simplepool-dashboard`.

### Simplepool won't start — `Address already in use`

There's a stale binary still bound. **DO NOT** `pkill -f simplepool`
from an SSH session — that pattern matches your own command line and
kills your SSH. Use exact-name match instead:

```sh
pkill -9 -x simplepool
sleep 2
ss -tlnp | grep :3334 || echo ":3334 free"
```

### Miner shares accrue but payouts never fire

Read the payout log. Common causes:
- Reserve short (fix: deposit)
- Thunder RPC unreachable (fix: `systemctl status`, log tail)
- Worker's Thunder address is `NULL` in the `workers` table (means it
  never authorized cleanly — check the `rejects` table for that
  worker_name)

---

## Open items on the operator side

1. **Point `pool_btc_address` at an enforcer-wallet-owned address** so
   the enforcer can spend accumulated coinbase directly into deposit
   txs. Right now it's set to the operator's external wallet — every
   deposit currently spends the enforcer's own seed funds.
2. **BMM / Thunder sidechain block production** on this box currently
   doesn't complete. Deposits are consensus-recognized (Ctip updates)
   but Thunder wallet balance stays 0 until BMM runs. Investigating
   this is a Thunder-side task, not a pool task.
3. **Admin "Deposit to Thunder" form** — CLASSIC_PAYOUTS.md sketches
   the endpoint (`POST /admin/deposit`) but it's not implemented yet.
   Until it lands, deposits are the CLI runbook above.

None of these block the pool from running correctly — they're
improvements to the operator UX and end-to-end value flow, not
correctness gaps.
