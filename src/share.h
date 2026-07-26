#ifndef SIMPLEPOOL_SHARE_H
#define SIMPLEPOOL_SHARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Double-SHA256. */
void dsha256(const uint8_t *in, size_t inlen, uint8_t out[32]);

/* Convert compact nbits -> 32-byte big-endian target. */
void nbits_to_target(uint32_t nbits, uint8_t target_be[32]);

/* Worker difficulty -> 32-byte big-endian target.
 * target = floor( DIFF1_TARGET / diff ) where DIFF1_TARGET corresponds to
 * nbits 0x1d00ffff. Implementation precision: ~1-2 ulp at usual diffs (we
 * compute the top 128 bits in f64 then place them, mirroring the Rust
 * pool-core::share implementation). */
void worker_diff_to_target(double diff, uint8_t target_be[32]);

/* 32-byte big-endian target -> difficulty (DIFF1_TARGET / target), from the
 * top 128 bits in f64. Saturates to HUGE_VAL when those bits are all zero. */
double target_to_diff(const uint8_t target_be[32]);

/* Fold coinbase txid (LE 32-byte) into a merkle root using branches
 * (each LE 32-byte). Always cur||branch order (Stratum: coinbase at idx 0). */
void merkle_root_from_branches(const uint8_t leaf_le[32],
                               const uint8_t (*branches)[32], size_t n,
                               uint8_t root_le[32]);

/* Build the 80-byte block header (LE on the wire).
 * prev_hash_le and merkle_root_le are in their NATURAL bytes-as-stored
 * little-endian form (i.e. the form that goes directly into the header). */
void build_header(int32_t version,
                  const uint8_t prev_hash_le[32],
                  const uint8_t merkle_root_le[32],
                  uint32_t ntime, uint32_t nbits, uint32_t nonce,
                  uint8_t header_out[80]);

/* Compare two 32-byte big-endian numbers. -1 if a<b, 0 if eq, +1 if a>b. */
int be32_cmp(const uint8_t a[32], const uint8_t b[32]);

/* Hash header, return 32-byte big-endian hash. */
void hash_header(const uint8_t header[80], uint8_t hash_be[32]);

#endif
