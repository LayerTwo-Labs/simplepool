#ifndef SIMPLEPOOL_THUNDER_H
#define SIMPLEPOOL_THUNDER_H

#include <stddef.h>
#include <stdint.h>

/* Thunder sidechain address utilities.
 *
 * Per thunder-rust (`lib/types/address.rs`), a Thunder address is a
 * 20-byte hash, displayed as plain base58 (no version byte, no
 * checksum) — i.e. shorter than a P2PKH base58check address.
 *
 * There is also a display-only "deposit-format" wrapper
 * `s<sc>_<base58>_<3-byte sha256 prefix hex>` produced by
 * `thunder-cli format-deposit-address`. That form is for humans to
 * paste into a mainchain wallet's deposit UI; it is NOT a Thunder
 * address at the byte level and Thunder rejects it in every RPC that
 * takes an address. Live empirical evidence: transfer() to a wrapper
 * form fails, and the enforcer's OP_RETURN parser logs
 * "Ignoring invalid deposit address" for it. We therefore reject
 * the wrapper here so miners get a clear stratum error at authorize
 * time instead of unpayable PPS balances at payout time.
 *
 * The miner's bare Thunder address from the stratum username is
 * recorded for accounting; the PPS payout worker (separate service,
 * off-chain Thunder txs) uses it as the recipient of thunder.transfer.
 */

/* Decode a Thunder address string. Accepts ONLY the bare base58
 * form (no wrapper, no underscores). The mainchain-wallet deposit
 * wrapper is rejected with a message pointing at the correct form.
 *
 * `out[20]` receives the address bytes. Returns 0 ok, negative on error.
 * errbuf may be NULL. */
int thunder_address_decode(const char *addr,
                           uint8_t out[20],
                           char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_THUNDER_H */
