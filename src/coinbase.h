#ifndef SIMPLEPOOL_COINBASE_H
#define SIMPLEPOOL_COINBASE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *cb1;
    size_t   cb1_len;
    uint8_t *cb2;
    size_t   cb2_len;
} coinbase_parts_t;

/* Build coinbase1/coinbase2 halves around the extranonce placeholder,
 * single-payout — the entire value_sats goes to payout_address.
 *
 * Equivalent to coinbase_build_split with operator_address=NULL / fee_bps=0.
 * Kept for tests and simple solo configurations.
 *
 * `witness_commitment_hex` may be NULL.
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build(uint32_t height, int64_t value_sats,
                   const char *payout_address,
                   const char *witness_commitment_hex,
                   const char *coinbase_tag,
                   size_t extranonce1_size, size_t extranonce2_size,
                   coinbase_parts_t *out, char *errbuf, size_t errlen);

/* Build coinbase1/coinbase2 with a two-way split:
 *   fee_sats   = value_sats * fee_bps / 10000  (rounded down)
 *   miner_sats = value_sats - fee_sats
 *
 * If `operator_address` is NULL/empty, fee_bps is 0, or `fee_sats` would
 * be below the dust threshold (~546 sats), the whole reward goes to the
 * miner and *out_fee_sats is set to 0. Otherwise both outputs are emitted.
 *
 * *out_miner_sats and *out_fee_sats receive the final split (may be NULL).
 *
 * Returns 0 ok, negative on error. */
int coinbase_build_split(uint32_t height, int64_t value_sats,
                         const char *miner_address,
                         const char *operator_address,
                         int fee_bps,
                         const char *witness_commitment_hex,
                         const char *coinbase_tag,
                         size_t extranonce1_size, size_t extranonce2_size,
                         coinbase_parts_t *out,
                         int64_t *out_miner_sats, int64_t *out_fee_sats,
                         char *errbuf, size_t errlen);

/* Build coinbase1/coinbase2 halves from a server-provided coinbase
 * transaction (BIP22 "coinbasetxn", e.g. from the CUSF enforcer), rather
 * than constructing the coinbase from scratch.
 *
 * `coinbase_tx_hex` is the full serialized coinbase tx (segwit or legacy).
 * The builder:
 *   - appends the extranonce placeholder to the existing scriptSig, after
 *     the BIP34 height push the server already placed there;
 *   - replaces the single spendable (non-OP_RETURN) output — which the
 *     server pays to its own reward address — with the miner payout and an
 *     optional operator-fee output (same split rule as coinbase_build_split);
 *   - preserves every other output byte-for-byte and in order (BIP300/301
 *     commitment OP_RETURNs and the segwit witness commitment).
 *
 * cb1/cb2 are the legacy (no-witness) serialization. *out_has_witness (if
 * non-NULL) reports whether the source tx was segwit-serialized, so the
 * caller can re-attach the witness reserved value when it assembles the
 * block. *out_miner_sats / *out_fee_sats receive the split (may be NULL).
 *
 * `operator_address` / `coinbase_tag` may be NULL. Returns 0 ok, negative on
 * error (errbuf populated). */
int coinbase_build_from_template(const char *coinbase_tx_hex,
                                 const char *miner_address,
                                 const char *operator_address,
                                 int fee_bps,
                                 const char *coinbase_tag,
                                 size_t extranonce1_size,
                                 size_t extranonce2_size,
                                 coinbase_parts_t *out,
                                 int *out_has_witness,
                                 int64_t *out_miner_sats,
                                 int64_t *out_fee_sats,
                                 char *errbuf, size_t errlen);

void coinbase_parts_free(coinbase_parts_t *p);

/* Internal helpers exposed for tests. */
int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen);

/* Build a drivechain-deposit coinbase. The outputs are emitted in this
 * fixed order (the BIP300 enforcer requires the OP_RETURN destination
 * output to immediately follow the OP_DRIVECHAIN output for the same
 * sidechain):
 *
 *   [0] OP_DRIVECHAIN (OP_NOP5 PUSH1 <sidechain> OP_TRUE), value = miner_sats
 *   [1] OP_RETURN <op_return_payload>                     value = 0
 *   [2] operator BTC fee (P2WPKH/P2PKH/P2SH from operator_address)
 *                                                         value = fee_sats
 *   [3] witness commitment OP_RETURN (if witness_commitment_hex set)
 *                                                         value = 0
 *
 * Where:
 *   miner_sats = value_sats - fee_sats
 *   fee_sats   = value_sats * fee_bps / 10000 (dropped if < dust)
 *
 * The deposit's "destination" on the Thunder side is encoded in
 * op_return_payload — the caller is responsible for the bytes (typically
 * either the ASCII of the pool's base58 Thunder address, matching
 * Thunder's wallet convention, or the raw 20-byte hash). For PPS this is
 * the pool's reserve address; per-miner accounting happens off-chain.
 *
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build_drivechain(uint32_t height, int64_t value_sats,
                              int sidechain_number,
                              const uint8_t *op_return_payload,
                              size_t op_return_payload_len,
                              const char *operator_address,
                              int fee_bps,
                              const char *witness_commitment_hex,
                              const char *coinbase_tag,
                              size_t extranonce1_size, size_t extranonce2_size,
                              coinbase_parts_t *out,
                              int64_t *out_miner_sats, int64_t *out_fee_sats,
                              char *errbuf, size_t errlen);

/* Drivechain variant of coinbase_build_from_template. Takes a backend-supplied
 * coinbase (e.g. from the CUSF enforcer, which already speaks BIP300 for the
 * mainchain side), parses its outputs, and rewrites the single spendable
 * output into the drivechain-deposit triplet:
 *
 *   <preserved server outputs ... >
 *   [k+0] OP_DRIVECHAIN(side)     value = miner_sats
 *   [k+1] OP_RETURN <payload>     value = 0
 *   [k+2] operator fee (optional) value = fee_sats
 *   <preserved server outputs ... >
 *
 * Other server outputs (BIP301 ack commitments, witness commitment, etc.)
 * are preserved byte-for-byte and in order. has_witness mirrors the source
 * tx's serialization (caller re-attaches the witness reserved value at
 * block-assembly time).
 *
 * Returns 0 ok, negative on error (errbuf populated). */
int coinbase_build_drivechain_from_template(const char *coinbase_tx_hex,
                                            int sidechain_number,
                                            const uint8_t *op_return_payload,
                                            size_t op_return_payload_len,
                                            const char *operator_address,
                                            int fee_bps,
                                            const char *coinbase_tag,
                                            size_t extranonce1_size,
                                            size_t extranonce2_size,
                                            coinbase_parts_t *out,
                                            int *out_has_witness,
                                            int64_t *out_miner_sats,
                                            int64_t *out_fee_sats,
                                            char *errbuf, size_t errlen);

#endif
