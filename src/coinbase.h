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

/* ---- coinbase-direct PPLNS ---------------------------------------------
 *
 * One miner's claim on this block's coinbase. */
typedef struct {
    const char *address;   /* L1 destination, as authorized on stratum */
    int64_t     sats;      /* what the window entitles this miner to */
} coinbase_payee_t;

/* What the builder actually managed to pay, and what it could not.
 *
 * `carry_sats` is the honest part. A payee below the dust limit, or past the
 * output cap, cannot be paid in THIS coinbase — but its value cannot simply
 * vanish either: a coinbase that pays out less than it is allowed forfeits
 * the difference to nobody. So the shortfall is added to the operator output
 * and reported here, which means the pool is holding it and owes it.
 *
 * That is the cost the design has to own: coinbase-direct removes custody for
 * everyone the block can pay, and replaces it with a small, bounded,
 * disclosable balance for everyone it cannot. It is not "zero custody"; it is
 * custody proportional to dust, and the number is right here rather than
 * implied. */
typedef struct {
    size_t  paid_count;        /* payees given an output */
    int64_t paid_sats;         /* summed across those outputs */
    size_t  dropped_dust;      /* payees below COINBASE_DUST_SATS */
    size_t  dropped_capped;    /* payees past max_payout_outputs */
    int64_t carry_sats;        /* owed to the dropped, paid to the operator */
    int64_t fee_sats;          /* the operator's actual fee, excluding carry */
} coinbase_window_result_t;

/* A practical ceiling on payout outputs, not a consensus one.
 *
 * Consensus bounds the coinbase by block weight; at ~31 bytes per P2WPKH
 * output even a thousand payees is a low single-digit percentage of the
 * budget. The real constraint is that some rented-hashrate marketplaces
 * verify a coinbase and reject one they consider oversized, and a delisting
 * costs more than paying a few small miners a block later. */
#define COINBASE_MAX_PAYOUT_OUTPUTS 200

/* Build cb1/cb2 paying the PPLNS window DIRECTLY, one output per miner.
 *
 * The point of the mode: the pool never receives the reward, so there is no
 * wallet, no payout worker, no write-ahead row and no credit-on-confirmation.
 * A reorged block simply never paid, which is also why this rail needs no
 * maturity gate — there is no credit to claw back.
 *
 * `payees` must sum to exactly (value_sats - fee), where fee is the same
 * fee_bps split every other builder applies. A caller whose arithmetic does
 * not add up is refused rather than silently underpaying the block.
 *
 * Payees are paid largest first, so the cap and the dust limit fall on the
 * smallest claims — the ones for whom waiting a block costs least, and whose
 * carried balance is smallest.
 *
 * Returns 0 ok, negative on error (errbuf populated). `res` may be NULL. */
int coinbase_build_window(uint32_t height, int64_t value_sats,
                          const coinbase_payee_t *payees, size_t n_payees,
                          const char *operator_address, int fee_bps,
                          const char *witness_commitment_hex,
                          const char *coinbase_tag,
                          size_t extranonce1_size, size_t extranonce2_size,
                          size_t max_payout_outputs,
                          coinbase_parts_t *out,
                          coinbase_window_result_t *res,
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

/* Count a serialized coinbase's outputs, split into spendable and OP_RETURN.
 * Either out-param may be NULL. The OP_RETURN count distinguishes a coinbase
 * we built (one output: the witness commitment) from one dictated by the CUSF
 * enforcer (plus the mandatory BIP300/301 commitments), which is what tells an
 * observer whether a sidechain can be merge-mined into these blocks.
 * Returns 0 ok, negative on malformed input. */
int coinbase_count_outputs(const char *tx_hex, int *spendable_out,
                           int *op_return_out);

/* The network an address encodes ("main", "regtest", "test/signet",
 * "test/signet/regtest"), or NULL when it parses as neither bech32 nor
 * base58check. Coarser than getblockchaininfo's `chain`: several networks
 * share version bytes and HRPs, and this reports only what the encoding
 * proves. Useful when the block-template backend cannot be asked. */
const char *coinbase_address_network(const char *addr);

/* Whether a chain name — from getblockchaininfo or from
 * coinbase_address_network() — means mainnet. Anything unrecognised counts
 * as a test chain, so a name we don't know never reads as "main". */
int coinbase_network_is_mainnet(const char *network);

/* Internal helpers exposed for tests. */
int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen);

#endif
