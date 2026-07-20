#define _POSIX_C_SOURCE 200809L
#include "thunder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/* Standard bitcoin base58 alphabet (Thunder uses the same alphabet). */
static const char b58_alpha[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

/* Plain base58 decode (no checksum). Mirrors coinbase.c's b58_decode but
 * we keep a local copy so thunder.c stays self-contained and unit-testable
 * without dragging in coinbase.c. */
static int b58_decode(const char *s, size_t slen,
                      uint8_t *out, size_t cap, size_t *out_len) {
    size_t zeros = 0;
    while (zeros < slen && s[zeros] == '1') zeros++;

    size_t bsize = slen * 733 / 1000 + 1;
    uint8_t *b = (uint8_t *)calloc(bsize, 1);
    if (!b) return -1;
    for (size_t i = 0; i < slen; i++) {
        const char *p = strchr(b58_alpha, s[i]);
        if (!p) { free(b); return -1; }
        unsigned carry = (unsigned)(p - b58_alpha);
        for (ssize_t j = (ssize_t)bsize - 1; j >= 0; j--) {
            carry += 58u * b[j];
            b[j] = (uint8_t)(carry & 0xff);
            carry >>= 8;
        }
        if (carry != 0) { free(b); return -1; }
    }
    size_t skip = 0;
    while (skip < bsize && b[skip] == 0) skip++;
    size_t total = zeros + (bsize - skip);
    if (total > cap) { free(b); return -1; }
    memset(out, 0, zeros);
    memcpy(out + zeros, b + skip, bsize - skip);
    *out_len = total;
    free(b);
    return 0;
}

int thunder_address_decode(const char *addr,
                           uint8_t out[20],
                           char *errbuf, size_t errlen) {
    if (!addr || !*addr) {
        set_err(errbuf, errlen, "empty thunder address");
        return -1;
    }
    size_t alen = strlen(addr);

    /* Accept ONLY the bare base58 form (raw base58 of 20 hash bytes).
     *
     * The deposit-format wrapper `s<sc>_<base58>_<hex6>` looks like a
     * Thunder address but is a display-only string that neither Thunder's
     * wallet nor its OP_RETURN parser recognizes at the byte level — a
     * pool that stores wrapper strings as workers.payout_address ends up
     * with unpayable balances (thunder.transfer rejects the wrapper).
     * To catch this at authorize time rather than payout time we treat
     * any character outside the base58 alphabet as a hard error.
     *
     * '_' isn't in base58, so the wrapper's underscores make b58_decode
     * fail below — but we bail early with a friendlier message so a
     * miner using the wrong form gets an actionable stratum reject. */
    if (strchr(addr, '_') != NULL) {
        set_err(errbuf, errlen,
                "thunder address contains '_' — looks like the "
                "'s<n>_<base58>_<hex6>' deposit-format wrapper; use "
                "the bare base58 (thunder-cli get-new-address) instead");
        return -1;
    }
    if (alen < 20 || alen > 40) {
        set_err(errbuf, errlen,
                "thunder address base58 length %zu out of range", alen);
        return -1;
    }

    uint8_t buf[64];
    size_t  dec_len = 0;
    if (b58_decode(addr, alen, buf, sizeof buf, &dec_len) < 0) {
        set_err(errbuf, errlen, "thunder address base58 decode failed");
        return -1;
    }
    if (dec_len != 20) {
        set_err(errbuf, errlen,
                "thunder address decoded length %zu (expected 20)", dec_len);
        return -1;
    }
    memcpy(out, buf, 20);
    return 0;
}
