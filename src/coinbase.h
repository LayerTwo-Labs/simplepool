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

/* The binding limit on payouts is BYTES, not a count.
 *
 * The first version of this capped the number of outputs at 200, which was
 * wrong in a way only production evidence showed. A rented-hashrate
 * marketplace verifies the coinbase and refuses a job whose coinbase it
 * considers oversized, and it measures bytes. At ~31 bytes per P2WPKH output,
 * 200 payouts is over 6000 bytes of outputs alone — roughly eight times what
 * a real coinbase-direct pool is observed to get away with.
 *
 * Reported from a coinbase-direct PPLNS pool running on the ECX alpha network
 * since 2026-08-19 (LayerTwo-Labs/simplepool#61): up to 16 miners paid per
 * block, whole coinbases measuring 721–817 bytes. Crucially, the binding term
 * there is not the payouts — it is the drivechain OP_RETURNs sharing the same
 * transaction. The same 16 payouts cost 817 bytes against four of them and
 * 769 against three. A count cap cannot express that; a byte budget can,
 * because the commitments are simply part of what has already been spent.
 *
 * 1000 covers the observed working range with headroom. Configurable via
 * coinbase_max_bytes, because the number that matters belongs to whichever
 * marketplace an operator is selling to, not to us. */
#define COINBASE_DEFAULT_MAX_BYTES 1000

/* Below this an output is not relayable, so it is the floor under every other
 * floor. In the header rather than coinbase.c because main.c reports the
 * effective payout floor to the operator and config.c documents it, and three
 * copies of 546 is three chances to disagree. */
#define COINBASE_DUST_SATS 546

/* Array bound only. The byte budget is what actually decides how many miners
 * are paid; this exists so the builders can use fixed-size storage, and is set
 * far above anything the budget will admit. */
#define COINBASE_MAX_PAYOUT_OUTPUTS 200

/* What the builder actually managed to pay, and what it could not.
 *
 * A coinbase has a fixed budget of bytes, so a window with more miners in it
 * than the budget admits cannot pay them all in one block. `redistributed_sats`
 * is what the ones it could not pay were owed.
 *
 * That value goes TO THE OTHER MINERS, not to the operator. The block still
 * pays out to the satoshi and the pool still holds nothing; the only question
 * a byte budget forces is which miners receive what it had no room for, and
 * the honest answer is the rest of the window rather than the house.
 *
 * It used to go to the operator. See the long note at the redistribution in
 * coinbase.c for the measurement that ended that: the rule was defended as a
 * dust policy, and dust turned out not to be involved at all. */
typedef struct {
    size_t  paid_count;          /* payees given an output */
    int64_t paid_sats;           /* summed across those outputs */
    size_t  dropped_below_floor; /* payees under payout_floor_sats */
    size_t  dropped_capped;      /* payees the byte budget had no room for */
    /* What the dropped payees were owed, now spread across the ones that were
     * paid. Reported so an operator can see how much of a block is landing on
     * miners other than the ones who earned it -- a large figure here means
     * the byte budget is too tight for the size of the pool, and is the number
     * to raise coinbase_max_bytes against.
     *
     * Nobody is left out of pocket by a single block being unable to pay them,
     * but nobody is made whole by it either: this is the unfairness the
     * per-worker fraction ledger exists to even out over time. */
    int64_t redistributed_sats;
    int64_t fee_sats;            /* the operator's fee, and nothing else */
} coinbase_window_result_t;


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
 * Payees are paid largest first, so the byte budget and the payout floor fall
 * on the smallest claims. Those are forfeited to the operator, not carried:
 * see coinbase_window_result_t.
 *
 * `payout_floor_sats` is clamped UP to COINBASE_DUST_SATS — below the dust
 * limit an output is not relayable, so there is no floor lower than that to
 * have.
 *
 * `max_coinbase_bytes` is the whole serialized coinbase, commitments and all,
 * not just the payouts. 0 means COINBASE_DEFAULT_MAX_BYTES.
 *
 * Returns 0 ok, negative on error (errbuf populated). `res` may be NULL. */
int coinbase_build_window(uint32_t height, int64_t value_sats,
                          const coinbase_payee_t *payees, size_t n_payees,
                          const char *operator_address, int fee_bps,
                          const char *witness_commitment_hex,
                          const char *coinbase_tag,
                          size_t extranonce1_size, size_t extranonce2_size,
                          size_t max_coinbase_bytes,
                          int64_t payout_floor_sats,
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
/* How many payouts a coinbase of `max_coinbase_bytes` will hold.
 *
 * Used to decide how many payout slots to reserve for long-waiting miners, so
 * it wants to match what the builder will actually admit. It charges each
 * address at its real serialized size rather than assuming one: a P2TR output
 * is 43 bytes against a P2WPKH one's 31, and assuming 31 for a window of
 * taproot addresses overestimated by 24 slots at a 3000-byte budget — enough
 * to reserve a third of the coinbase for rotation where a quarter was meant.
 *
 * `addresses` may be NULL, in which case it falls back to assuming P2WPKH;
 * that is only for callers with no window in hand.
 *
 * `coinbase_tx_hex` may be NULL, for a coinbase built from scratch; when given,
 * its existing outputs are charged against the budget the way the builder
 * charges them, because on a drivechain the commitment OP_RETURNs are what
 * actually decide how many miners fit.
 *
 * Still only an estimate of the builder's answer, and deliberately so: getting
 * it wrong changes the fairness of the rotation and never the arithmetic.
 * Everyone in the order still receives their own claim, and whatever the
 * budget cuts is still redistributed. */
size_t coinbase_expected_payout_slots(size_t max_coinbase_bytes,
                                      const char *coinbase_tx_hex,
                                      const char *const *addresses,
                                      size_t n_addresses);

/* The reward a server-provided coinbasetxn actually pays, in sats: the value
 * of its single spendable output, which is the one the window replaces.
 *
 * A caller splitting a window has to divide THIS number, not the template's
 * `coinbasevalue`. The two normally agree, but nothing makes them: the field
 * is what the node says the block may pay, and the transaction is what its
 * coinbase does pay. When they disagree the builders refuse the split -- the
 * payees no longer sum to the reward -- and since that happens per job, on
 * every connection, a pool would simply stop publishing work with a warning
 * per render and no single cause to find.
 *
 * Returns 0 ok, negative if the tx is malformed or does not have exactly one
 * spendable output (in which case there is no single reward to speak of). */
int coinbase_template_reward(const char *coinbase_tx_hex, int64_t *out_sats);

/* coinbase_build_window(), but replacing the single spendable output of a
 * server-provided coinbasetxn instead of building one from scratch.
 *
 * This is the drivechain path, and it is the one a real pool needs: when the
 * CUSF enforcer serves the template, its coinbase already carries the
 * BIP300/301 commitment OP_RETURNs and the witness commitment, and those are
 * preserved byte-for-byte and in order exactly as
 * coinbase_build_from_template() does. Only the reward output is replaced --
 * by the whole window rather than by one miner.
 *
 * The splitting rules are SHARED with coinbase_build_window() rather than
 * reimplemented, so a pool mining a drivechain template and one mining plain
 * bitcoind cannot divide the same window differently.
 *
 * Returns 0 ok, negative on error. `res` and `out_has_witness` may be NULL. */
int coinbase_build_window_from_template(const char *coinbase_tx_hex,
                                        const coinbase_payee_t *payees,
                                        size_t n_payees,
                                        const char *operator_address,
                                        int fee_bps,
                                        const char *coinbase_tag,
                                        size_t extranonce1_size,
                                        size_t extranonce2_size,
                                        size_t max_coinbase_bytes,
                                        int64_t payout_floor_sats,
                                        coinbase_parts_t *out,
                                        int *out_has_witness,
                                        coinbase_window_result_t *res,
                                        char *errbuf, size_t errlen);

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
