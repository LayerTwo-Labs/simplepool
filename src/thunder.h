#ifndef SIMPLEPOOL_THUNDER_H
#define SIMPLEPOOL_THUNDER_H

#include <stddef.h>
#include <stdint.h>

/* Thunder sidechain address utilities.
 *
 * Per thunder-rust (`lib/types/address.rs`), a Thunder address is a
 * 20-byte hash, displayed as plain base58 (no version byte, no
 * checksum) — i.e. shorter than a P2PKH base58check address. There is
 * also a deposit-format string `s9_<base58>_<3-byte sha256 prefix hex>`
 * used by the mainchain wallet UI; we accept either form and surface
 * the raw 20 bytes.
 *
 * The mainchain coinbase carries the *pool's* Thunder reserve address
 * (every deposit accrues to the pool), not the miner's. The miner's
 * Thunder address from the stratum username is recorded for accounting
 * so the PPS payout worker (separate service, off-chain Thunder txs)
 * can credit the right account.
 */

/* Decode a Thunder address string. Accepts:
 *   - bare base58:     `<28-34 chars>`            -> 20 raw bytes
 *   - deposit-format:  `s<num>_<base58>_<hex6>`   -> 20 raw bytes
 *
 * `out[20]` receives the address bytes. Returns 0 ok, negative on error.
 * errbuf may be NULL. */
int thunder_address_decode(const char *addr,
                           uint8_t out[20],
                           char *errbuf, size_t errlen);

#endif /* SIMPLEPOOL_THUNDER_H */
