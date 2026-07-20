#!/usr/bin/env bash
# Record a mainchain-to-Thunder deposit in the pool's deposits ledger so
# it shows up in the admin dashboard.
#
# The enforcer's CreateDepositTransaction RPC returns a mainchain txid;
# after Ctip confirms the deposit, invoke this script with what you know
# and it inserts a row into `deposits`.
#
# Usage:
#   scripts/log-deposit.sh \
#     --db /home/forknet/pps-thunder-test/data/shares.db \
#     --txid <mainchain btc txid> \
#     --sats <sats_deposited> \
#     --fee <fee_sats> \
#     --recipient <bare Thunder base58 address> \
#     --ctip-before <n> --ctip-after <n> \
#     [--note "reason / notes"]

set -euo pipefail
DB=""
TXID=""
SATS=""
FEE=""
RECIP=""
BEFORE=""
AFTER=""
NOTE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --db)          DB="$2"; shift 2 ;;
        --txid)        TXID="$2"; shift 2 ;;
        --sats)        SATS="$2"; shift 2 ;;
        --fee)         FEE="$2"; shift 2 ;;
        --recipient)   RECIP="$2"; shift 2 ;;
        --ctip-before) BEFORE="$2"; shift 2 ;;
        --ctip-after)  AFTER="$2"; shift 2 ;;
        --note)        NOTE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

for req in DB TXID SATS FEE RECIP; do
    if [[ -z "${!req}" ]]; then
        echo "missing required flag: --$(echo $req | tr A-Z a-z)" >&2
        exit 2
    fi
done

if [[ "$RECIP" == *_* ]]; then
    echo "recipient '$RECIP' contains '_' — looks like a deposit-format wrapper;" >&2
    echo "use the bare base58 (thunder-cli get-new-address). Aborting." >&2
    exit 3
fi

NOW=$(date +%s)
sqlite3 "$DB" <<SQL
INSERT INTO deposits
  (ts, btc_txid, sats_deposited, fee_sats, thunder_recipient,
   ctip_seq_before, ctip_seq_after, notes)
VALUES
  ($NOW, '$TXID', $SATS, $FEE, '$RECIP',
   $(if [[ -n "$BEFORE" ]]; then echo $BEFORE; else echo NULL; fi),
   $(if [[ -n "$AFTER"  ]]; then echo $AFTER;  else echo NULL; fi),
   $(if [[ -n "$NOTE"   ]]; then echo "'$NOTE'"; else echo NULL; fi));
SQL

echo "logged deposit: $SATS sats to $RECIP (txid $TXID)"
