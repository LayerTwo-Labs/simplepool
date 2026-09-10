#define _POSIX_C_SOURCE 200809L
#include "coinbase.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdint.h>
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

/* ---------- byte-buffer helpers ---------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    int      oom;
} bbuf_t;

static void bbuf_init(bbuf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}
static void bbuf_free(bbuf_t *b) {
    free(b->data);
    bbuf_init(b);
}
static int bbuf_reserve(bbuf_t *b, size_t need) {
    if (b->cap >= need) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, nc);
    if (!p) { b->oom = 1; return -1; }
    b->data = p;
    b->cap = nc;
    return 0;
}
static int bbuf_push(bbuf_t *b, const void *src, size_t n) {
    if (bbuf_reserve(b, b->len + n) < 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}
static int bbuf_push_u8(bbuf_t *b, uint8_t v) { return bbuf_push(b, &v, 1); }
static int bbuf_push_u32_le(bbuf_t *b, uint32_t v) {
    uint8_t buf[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return bbuf_push(b, buf, 4);
}
static int bbuf_push_i32_le(bbuf_t *b, int32_t v) {
    return bbuf_push_u32_le(b, (uint32_t)v);
}
static int bbuf_push_u64_le(bbuf_t *b, uint64_t v) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)(v >> (8 * i));
    return bbuf_push(b, buf, 8);
}
static int bbuf_push_varint(bbuf_t *b, uint64_t n) {
    if (n < 0xfd) return bbuf_push_u8(b, (uint8_t)n);
    if (n <= 0xffff) {
        if (bbuf_push_u8(b, 0xfd) < 0) return -1;
        uint8_t buf[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
        return bbuf_push(b, buf, 2);
    }
    if (n <= 0xffffffffULL) {
        if (bbuf_push_u8(b, 0xfe) < 0) return -1;
        return bbuf_push_u32_le(b, (uint32_t)n);
    }
    if (bbuf_push_u8(b, 0xff) < 0) return -1;
    return bbuf_push_u64_le(b, n);
}

/* ---------- BIP34 height push ----------
 *
 * Bitcoin Core validates the coinbase scriptSig by comparing it against
 * `CScript() << nHeight`. That operator overload calls push_int64(), which
 * has three branches:
 *
 *   n == 0            -> OP_0  (single byte 0x00)
 *   1 <= n <= 16      -> OP_N  (single byte 0x50 + n)            <-- short form
 *   otherwise         -> length-prefixed CScriptNum::serialize(n)
 *
 * On a fresh chain (regtest / signet / drivechain testnet) the first 16
 * blocks therefore expect the OP_N short form. Encoding a height of 5 as
 * {0x01,0x05} instead of {0x55} causes ContextualCheckBlock to reject the
 * block with "bad-cb-height". Mainnet is unaffected because every accepted
 * block has height >= 17.
 *
 * Returns number of bytes written (always <= 6). */
static size_t bip34_height_push(uint32_t height, uint8_t out[8]) {
    if (height == 0) {
        out[0] = 0x00;        /* OP_0 */
        return 1;
    }
    if (height <= 16) {
        out[0] = (uint8_t)(0x50 + height);   /* OP_1 .. OP_16 */
        return 1;
    }
    /* CScriptNum minimal little-endian encoding with length prefix. The
     * extra zero byte handles the sign bit so high values aren't read as
     * negative. */
    uint8_t bytes[5];
    size_t n = 0;
    uint32_t v = height;
    while (v > 0) {
        bytes[n++] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    if (bytes[n - 1] & 0x80) {
        bytes[n++] = 0x00;
    }
    out[0] = (uint8_t)n;
    memcpy(out + 1, bytes, n);
    return n + 1;
}

/* ---------- hex ---------- */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len) {
    size_t n = strlen(hex);
    if (n % 2 != 0) return -1;
    if (n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return 0;
}

/* ---------- base58check ---------- */

static const char b58_alpha[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int b58_decode(const char *s, uint8_t *out, size_t cap, size_t *out_len) {
    size_t slen = strlen(s);
    /* Count leading '1's. */
    size_t zeros = 0;
    while (zeros < slen && s[zeros] == '1') zeros++;

    /* Process. */
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

    /* Skip leading zeros in big-int representation. */
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

/* ---------- bech32 / segwit (BIP173) ---------- */

static const char bech32_alpha[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int bech32_charpos(char c) {
    const char *p = strchr(bech32_alpha, c);
    if (!p) return -1;
    return (int)(p - bech32_alpha);
}

static uint32_t bech32_polymod(const uint8_t *values, size_t n) {
    static const uint32_t G[5] = {
        0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3
    };
    uint32_t chk = 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)(chk >> 25);
        chk = ((chk & 0x1ffffff) << 5) ^ values[i];
        for (int j = 0; j < 5; j++) {
            if ((b >> j) & 1) chk ^= G[j];
        }
    }
    return chk;
}

static void bech32_hrp_expand(const char *hrp, uint8_t *out, size_t *outlen) {
    size_t n = strlen(hrp);
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(hrp[i] >> 5);
    out[n] = 0;
    for (size_t i = 0; i < n; i++) out[n + 1 + i] = (uint8_t)(hrp[i] & 31);
    *outlen = 2 * n + 1;
}

/* Decode bech32 / bech32m. Returns 0 ok, negative on error.
 * out_data receives the 5-bit groups (after HRP and separator, excluding checksum).
 * encoding_out: 1 = bech32, 2 = bech32m.
 */
static int bech32_decode(const char *addr, char *hrp_out, size_t hrp_cap,
                         uint8_t *data_out, size_t *data_len, int *encoding_out) {
    size_t alen = strlen(addr);
    if (alen < 8 || alen > 90) return -1;

    /* Find separator '1' (last one). */
    ssize_t sep = -1;
    for (ssize_t i = (ssize_t)alen - 1; i >= 0; i--) {
        if (addr[i] == '1') { sep = i; break; }
    }
    if (sep < 1 || (size_t)(sep + 7) > alen) return -1;

    /* HRP. */
    size_t hrp_len = (size_t)sep;
    if (hrp_len + 1 > hrp_cap) return -1;
    int has_lower = 0, has_upper = 0;
    for (size_t i = 0; i < hrp_len; i++) {
        char c = addr[i];
        if (c < 33 || c > 126) return -1;
        if (c >= 'a' && c <= 'z') has_lower = 1;
        if (c >= 'A' && c <= 'Z') { has_upper = 1; hrp_out[i] = (char)(c - 'A' + 'a'); }
        else hrp_out[i] = c;
    }
    hrp_out[hrp_len] = '\0';

    /* Data part. */
    size_t data_n = alen - hrp_len - 1;
    if (data_n > 90) return -1;
    uint8_t values[90];
    for (size_t i = 0; i < data_n; i++) {
        char c = addr[hrp_len + 1 + i];
        if (c >= 'A' && c <= 'Z') { has_upper = 1; c = (char)(c - 'A' + 'a'); }
        else if (c >= 'a' && c <= 'z') has_lower = 1;
        int v = bech32_charpos(c);
        if (v < 0) return -1;
        values[i] = (uint8_t)v;
    }
    if (has_lower && has_upper) return -1;

    /* Compute polymod over hrp_expand || values. */
    uint8_t buf[200];
    size_t buflen = 0;
    bech32_hrp_expand(hrp_out, buf, &buflen);
    if (buflen + data_n > sizeof buf) return -1;
    memcpy(buf + buflen, values, data_n);
    uint32_t chk = bech32_polymod(buf, buflen + data_n);
    if (chk == 1) *encoding_out = 1;
    else if (chk == 0x2bc830a3) *encoding_out = 2;
    else return -1;

    if (*data_len < data_n - 6) return -1;
    memcpy(data_out, values, data_n - 6);
    *data_len = data_n - 6;
    return 0;
}

/* Convert from bits-per-element `from` to bits-per-element `to`. */
static int convertbits(const uint8_t *in, size_t in_len, int from, int to, int pad,
                       uint8_t *out, size_t *out_len) {
    uint32_t acc = 0;
    int bits = 0;
    size_t off = 0;
    uint32_t maxv = (1u << to) - 1;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t v = in[i];
        if (v >> from) return -1;
        acc = (acc << from) | v;
        bits += from;
        while (bits >= to) {
            bits -= to;
            if (off >= *out_len) return -1;
            out[off++] = (uint8_t)((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits > 0) {
            if (off >= *out_len) return -1;
            out[off++] = (uint8_t)((acc << (to - bits)) & maxv);
        }
    } else if (bits >= from || ((acc << (to - bits)) & maxv) != 0) {
        return -1;
    }
    *out_len = off;
    return 0;
}

/* ---------- address -> script ---------- */

int coinbase_address_to_script(const char *addr,
                               uint8_t *out, size_t cap, size_t *out_len,
                               char *errbuf, size_t errlen) {
    /* Try bech32 first if it has '1' separator and a known HRP prefix. */
    if (strncmp(addr, "bc1",   3) == 0 ||
        strncmp(addr, "tb1",   3) == 0 ||
        strncmp(addr, "bcrt1", 5) == 0 ||
        strncmp(addr, "BC1",   3) == 0 ||
        strncmp(addr, "TB1",   3) == 0 ||
        strncmp(addr, "BCRT1", 5) == 0) {
        char hrp[16];
        uint8_t data[90];
        size_t data_len = sizeof data;
        int enc = 0;
        if (bech32_decode(addr, hrp, sizeof hrp, data, &data_len, &enc) < 0) {
            set_err(errbuf, errlen, "bech32 decode failed for '%s'", addr);
            return -1;
        }
        if (data_len < 1) {
            set_err(errbuf, errlen, "bech32 data too short");
            return -1;
        }
        uint8_t witver = data[0];
        if (witver > 16) {
            set_err(errbuf, errlen, "bad witness version %u", witver);
            return -1;
        }
        /* v0 must use bech32; v1+ must use bech32m. */
        if ((witver == 0 && enc != 1) || (witver != 0 && enc != 2)) {
            set_err(errbuf, errlen, "bech32 encoding/version mismatch");
            return -1;
        }
        uint8_t prog[64];
        size_t prog_len = sizeof prog;
        if (convertbits(data + 1, data_len - 1, 5, 8, 0, prog, &prog_len) < 0) {
            set_err(errbuf, errlen, "bech32 5->8 convert failed");
            return -1;
        }
        /* Witness v0: P2WPKH (20-byte) or P2WSH (32-byte). Any other length is
         * invalid per BIP-141 and Core will not relay it. */
        if (witver == 0) {
            if (prog_len != 20 && prog_len != 32) {
                set_err(errbuf, errlen,
                        "v0 program must be 20 or 32 bytes, got %zu", prog_len);
                return -1;
            }
            /* OP_0 <push prog_len> <program>. */
            if (cap < prog_len + 2) return -1;
            out[0] = 0x00;
            out[1] = (uint8_t)prog_len;
            memcpy(out + 2, prog, prog_len);
            *out_len = prog_len + 2;
            return 0;
        }

        /* ⛔ WITNESS v1 ONLY, AND v2-v16 ARE REFUSED ON PURPOSE.
         *
         * An output to an unactivated witness version is ANYONE-CAN-SPEND under
         * current consensus: the script succeeds without a signature, so the
         * first party to notice takes the coins. `validateaddress` calls such an
         * address valid, and Core will let a user send to one, because there the
         * sender is choosing the risk in the moment for one payment.
         *
         * A pool coinbase is not that. The miner types an address once and every
         * block they ever find pays it, unattended — so a typo into a future
         * version, or an address from a wallet experimenting with one, would
         * hand block rewards to whoever is watching. Refusing at authorize costs
         * that miner one clear error message. Accepting costs them a block.
         *
         * This is deliberately STRICTER than the address being well-formed. When
         * a version activates and its outputs become spendable only by their
         * owner, adding it here is a one-line change made on purpose rather than
         * a door that was left open. */
        if (witver != 1) {
            set_err(errbuf, errlen,
                    "witness v%u is not spendable-only-by-its-owner under current "
                    "consensus; a coinbase must not pay it", witver);
            return -1;
        }
        if (prog_len != 32) {
            set_err(errbuf, errlen,
                    "v1 (taproot) program must be 32 bytes, got %zu", prog_len);
            return -1;
        }
        /* P2TR: OP_1 <32-byte push>. Cross-checked against bitcoind's own
         * validateaddress, which returns scriptPubKey 5120<program> for a
         * bc1p address. */
        if (cap < 34) return -1;
        out[0] = 0x51; /* OP_1 */
        out[1] = 0x20; /* push 32 */
        memcpy(out + 2, prog, 32);
        *out_len = 34;
        return 0;
    }

    /* Base58check: P2PKH or P2SH. */
    uint8_t dec[64];
    size_t dec_len = 0;
    if (b58_decode(addr, dec, sizeof dec, &dec_len) < 0 || dec_len < 5) {
        set_err(errbuf, errlen, "base58 decode failed for '%s'", addr);
        return -1;
    }
    /* Verify checksum. */
    uint8_t hash1[32], hash2[32];
    sha256(dec, dec_len - 4, hash1);
    sha256(hash1, 32, hash2);
    if (memcmp(hash2, dec + dec_len - 4, 4) != 0) {
        set_err(errbuf, errlen, "base58 checksum mismatch for '%s'", addr);
        return -1;
    }
    if (dec_len != 25) {
        set_err(errbuf, errlen, "unexpected base58 length %zu", dec_len);
        return -1;
    }
    uint8_t ver = dec[0];
    /* P2PKH: 0x00 (mainnet), 0x6f (testnet/regtest). */
    if (ver == 0x00 || ver == 0x6f) {
        if (cap < 25) return -1;
        out[0] = 0x76; /* OP_DUP */
        out[1] = 0xa9; /* OP_HASH160 */
        out[2] = 0x14; /* push 20 */
        memcpy(out + 3, dec + 1, 20);
        out[23] = 0x88; /* OP_EQUALVERIFY */
        out[24] = 0xac; /* OP_CHECKSIG */
        *out_len = 25;
        return 0;
    }
    /* P2SH: 0x05 (mainnet), 0xc4 (testnet/regtest). */
    if (ver == 0x05 || ver == 0xc4) {
        if (cap < 23) return -1;
        out[0] = 0xa9; /* OP_HASH160 */
        out[1] = 0x14; /* push 20 */
        memcpy(out + 2, dec + 1, 20);
        out[22] = 0x87; /* OP_EQUAL */
        *out_len = 23;
        return 0;
    }
    set_err(errbuf, errlen, "unsupported base58 version byte 0x%02x", ver);
    return -1;
}

/* ---------- address -> network ---------- */

/* The network an address encodes, from its bech32 HRP or base58 version
 * byte. Returns a static string, or NULL when the address parses as neither.
 *
 * Coarser than getblockchaininfo's `chain` on purpose. Testnet, signet and
 * regtest share the 0x6f/0xc4 base58 version bytes, and testnet and signet
 * share the `tb` HRP, so an address genuinely cannot tell those apart — this
 * returns what the encoding actually proves and nothing more. That is enough
 * for the one check worth making: mainnet vs not. Paying the operator fee to
 * a mainnet address on a test chain (or the reverse) burns it to a script
 * nobody on that chain controls. */
const char *coinbase_address_network(const char *addr) {
    if (!addr || !addr[0]) return NULL;
    if (strncmp(addr, "bcrt1", 5) == 0 || strncmp(addr, "BCRT1", 5) == 0)
        return "regtest";
    if (strncmp(addr, "bc1", 3) == 0 || strncmp(addr, "BC1", 3) == 0)
        return "main";
    if (strncmp(addr, "tb1", 3) == 0 || strncmp(addr, "TB1", 3) == 0)
        return "test/signet";

    uint8_t dec[64];
    size_t  dec_len = 0;
    if (b58_decode(addr, dec, sizeof dec, &dec_len) < 0 || dec_len != 25)
        return NULL;
    /* Checksum, so a typo reads as "unknown" rather than as a network. */
    uint8_t h1[32], h2[32];
    sha256(dec, dec_len - 4, h1);
    sha256(h1, 32, h2);
    if (memcmp(h2, dec + dec_len - 4, 4) != 0) return NULL;
    if (dec[0] == 0x00 || dec[0] == 0x05) return "main";
    if (dec[0] == 0x6f || dec[0] == 0xc4) return "test/signet/regtest";
    return NULL;
}

/* Whether a chain name — from either getblockchaininfo or the function above
 * — means mainnet. Anything that is not explicitly mainnet is treated as a
 * test chain, so an unrecognised name never silently reads as "main". */
int coinbase_network_is_mainnet(const char *network) {
    return network && strcmp(network, "main") == 0;
}

/* ---------- main builder ---------- */

/* Bitcoin's standard relay dust threshold for legacy outputs. Below this
 * the operator fee output would not be relayed; we collapse to a single
 * miner-only output in that case. */

void coinbase_parts_free(coinbase_parts_t *p) {
    if (!p) return;
    free(p->cb1); p->cb1 = NULL; p->cb1_len = 0;
    free(p->cb2); p->cb2 = NULL; p->cb2_len = 0;
}

int coinbase_build(uint32_t height, int64_t value_sats,
                   const char *payout_address,
                   const char *witness_commitment_hex,
                   const char *coinbase_tag,
                   size_t extranonce1_size, size_t extranonce2_size,
                   coinbase_parts_t *out, char *errbuf, size_t errlen) {
    return coinbase_build_split(height, value_sats,
                                payout_address, NULL, 0,
                                witness_commitment_hex, coinbase_tag,
                                extranonce1_size, extranonce2_size,
                                out, NULL, NULL, errbuf, errlen);
}

int coinbase_build_split(uint32_t height, int64_t value_sats,
                         const char *miner_address,
                         const char *operator_address,
                         int fee_bps,
                         const char *witness_commitment_hex,
                         const char *coinbase_tag,
                         size_t extranonce1_size, size_t extranonce2_size,
                         coinbase_parts_t *out,
                         int64_t *out_miner_sats, int64_t *out_fee_sats,
                         char *errbuf, size_t errlen) {
    if (!out || !miner_address) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;
    if (out_miner_sats) *out_miner_sats = value_sats;
    if (out_fee_sats)   *out_fee_sats   = 0;

    /* Resolve scriptPubKey for miner. */
    uint8_t miner_spk[64];
    size_t  miner_spk_len = 0;
    if (coinbase_address_to_script(miner_address, miner_spk, sizeof miner_spk,
                                   &miner_spk_len, errbuf, errlen) < 0) {
        return -1;
    }

    /* Compute fee split. The fee output is omitted entirely if it would be
     * zero, below dust, or the operator address is missing. */
    int64_t fee_sats   = 0;
    int64_t miner_sats = value_sats;
    uint8_t operator_spk[64];
    size_t  operator_spk_len = 0;
    int     has_operator = 0;
    if (operator_address && operator_address[0] &&
        fee_bps > 0 && value_sats > 0) {
        fee_sats = (value_sats * (int64_t)fee_bps) / 10000;
        if (fee_sats >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(operator_address, operator_spk,
                                           sizeof operator_spk,
                                           &operator_spk_len,
                                           errbuf, errlen) < 0) {
                return -1;
            }
            miner_sats = value_sats - fee_sats;
            has_operator = 1;
        } else {
            fee_sats = 0;
        }
    }
    if (out_miner_sats) *out_miner_sats = miner_sats;
    if (out_fee_sats)   *out_fee_sats   = fee_sats;

    /* Decode witness commitment if any. */
    uint8_t wc_buf[256];
    size_t wc_len = 0;
    int has_wc = 0;
    if (witness_commitment_hex && *witness_commitment_hex) {
        if (hex_decode(witness_commitment_hex, wc_buf, sizeof wc_buf, &wc_len) < 0) {
            set_err(errbuf, errlen, "bad witness commitment hex");
            return -1;
        }
        has_wc = 1;
    }

    /* Height push. */
    uint8_t height_push[8];
    size_t height_push_len = bip34_height_push(height, height_push);

    /* Tag push. */
    uint8_t tag_push[80];
    size_t tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }

    size_t en_total = extranonce1_size + extranonce2_size;
    size_t script_sig_len = height_push_len + tag_push_len + en_total;

    /* Consensus caps the coinbase scriptSig at 100 bytes; a block that
     * exceeds it is rejected outright. The budget is the BIP34 height push
     * plus the operator's coinbase_tag (up to 76 bytes with its length byte)
     * plus both extranonces, so a long tag and a wide extranonce can reach it
     * together. The coinbasetxn path below checks this already -- check here
     * too rather than emitting a coinbase that only fails at the network. */
    if (script_sig_len < 2 || script_sig_len > 100) {
        set_err(errbuf, errlen, "coinbase scriptSig length %zu out of range "
                "(height %zu + tag %zu + extranonce %zu)",
                script_sig_len, height_push_len, tag_push_len, en_total);
        return -1;
    }

    /* Build outputs blob: miner payout, [operator fee], [witness commitment]. */
    bbuf_t outs;
    bbuf_init(&outs);
    uint64_t n_outputs = 1;

    if (bbuf_push_u64_le(&outs, (uint64_t)miner_sats) < 0) goto oom;
    if (bbuf_push_varint(&outs, miner_spk_len) < 0) goto oom;
    if (bbuf_push(&outs, miner_spk, miner_spk_len) < 0) goto oom;

    if (has_operator) {
        if (bbuf_push_u64_le(&outs, (uint64_t)fee_sats) < 0) goto oom;
        if (bbuf_push_varint(&outs, operator_spk_len) < 0) goto oom;
        if (bbuf_push(&outs, operator_spk, operator_spk_len) < 0) goto oom;
        n_outputs++;
    }

    if (has_wc) {
        if (bbuf_push_u64_le(&outs, 0) < 0) goto oom;
        if (bbuf_push_varint(&outs, wc_len) < 0) goto oom;
        if (bbuf_push(&outs, wc_buf, wc_len) < 0) goto oom;
        n_outputs++;
    }

    /* Build c1. */
    bbuf_t c1;
    bbuf_init(&c1);
    if (bbuf_push_i32_le(&c1, 1) < 0) goto oom2;
    if (bbuf_push_varint(&c1, 1) < 0) goto oom2;
    static const uint8_t zero32[32] = {0};
    if (bbuf_push(&c1, zero32, 32) < 0) goto oom2;
    if (bbuf_push_u32_le(&c1, 0xffffffff) < 0) goto oom2;
    if (bbuf_push_varint(&c1, script_sig_len) < 0) goto oom2;
    if (bbuf_push(&c1, height_push, height_push_len) < 0) goto oom2;
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) goto oom2;

    /* Build c2. */
    bbuf_t c2;
    bbuf_init(&c2);
    if (bbuf_push_u32_le(&c2, 0xffffffff) < 0) goto oom3;
    if (bbuf_push_varint(&c2, n_outputs) < 0) goto oom3;
    if (bbuf_push(&c2, outs.data, outs.len) < 0) goto oom3;
    if (bbuf_push_u32_le(&c2, 0) < 0) goto oom3;

    bbuf_free(&outs);

    out->cb1 = c1.data;
    out->cb1_len = c1.len;
    out->cb2 = c2.data;
    out->cb2_len = c2.len;
    return 0;

oom3:
    bbuf_free(&c2);
oom2:
    bbuf_free(&c1);
oom:
    bbuf_free(&outs);
    set_err(errbuf, errlen, "out of memory");
    return -1;
}

/* ---------- coinbasetxn (server-provided coinbase) ---------- */

/* Read a little-endian Bitcoin varint from buf[*off..len). Advances *off. */
static int rd_varint(const uint8_t *buf, size_t len, size_t *off, uint64_t *val) {
    if (*off >= len) return -1;
    uint8_t b = buf[(*off)++];
    if (b < 0xfd) { *val = b; return 0; }
    size_t n = (b == 0xfd) ? 2 : (b == 0xfe) ? 4 : 8;
    if (*off + n > len) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += n;
    *val = v;
    return 0;
}

static int rd_u32(const uint8_t *buf, size_t len, size_t *off, uint32_t *val) {
    if (*off + 4 > len) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)buf[*off + i] << (8 * i);
    *off += 4;
    *val = v;
    return 0;
}

static int rd_u64(const uint8_t *buf, size_t len, size_t *off, uint64_t *val) {
    if (*off + 8 > len) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += 8;
    *val = v;
    return 0;
}

/* ---------- coinbase-direct PPLNS ---------- */

/* One concrete output the reward is being replaced with. */
typedef struct {
    uint8_t spk[64];
    size_t  spk_len;
    int64_t sats;
} cb_repl_out_t;

/* Turn a template's reward into the outputs that replace it.
 *
 * A callback rather than an argument because the split depends on the reward,
 * and the reward is only known once the template has been parsed -- so the
 * caller cannot compute it up front, and the parser should not have to know
 * whether it is paying one miner or a whole window. */
typedef int (*cb_repl_fn)(void *ctx, int64_t reward_sats, size_t fixed_bytes,
                          cb_repl_out_t *out, size_t cap, size_t *out_n,
                          char *errbuf, size_t errlen);

/* Serialized size of one output: value + the scriptPubKey's length prefix +
 * the script itself. */
static size_t out_ser_size(size_t spk_len) {
    size_t vi = spk_len < 253 ? 1 : (spk_len <= 0xffff ? 3 : 5);
    return 8 + vi + spk_len;
}

/* Payees plus the operator. */
#define CB_MAX_REPL_OUTS (COINBASE_MAX_PAYOUT_OUTPUTS + 1)

/* Turn a window into concrete outputs: fee off the top, the payout floor and
 * the byte budget applied largest-first, and whatever cannot be paid
 * redistributed across the miners who could be.
 *
 * Shared by the from-scratch and from-template builders precisely so the two
 * cannot drift. A pool mining a drivechain template and one mining plain
 * bitcoind must split a window identically; the only difference between them
 * is which outputs are preserved around the payout, and that is not this
 * function's business. */
static int resolve_window_outputs(int64_t value_sats,
                                  const coinbase_payee_t *payees, size_t n_payees,
                                  const char *operator_address, int fee_bps,
                                  size_t max_coinbase_bytes, size_t fixed_bytes,
                                  int64_t payout_floor_sats,
                                  cb_repl_out_t *out, size_t cap, size_t *out_n,
                                  coinbase_window_result_t *res,
                                  char *errbuf, size_t errlen) {
    coinbase_window_result_t r;
    memset(&r, 0, sizeof r);
    if (res) *res = r;
    if (out_n) *out_n = 0;
    if (!out || cap < 2 || !payees || n_payees == 0) {
        set_err(errbuf, errlen, "window is empty: nobody to pay");
        return -1;
    }
    if (value_sats <= 0) {
        set_err(errbuf, errlen, "value_sats must be positive");
        return -1;
    }
    if (max_coinbase_bytes == 0) max_coinbase_bytes = COINBASE_DEFAULT_MAX_BYTES;
    /* Never below the relay dust limit, whatever the operator configured: an
     * output under it would not be relayed, so "paying" it pays nobody. */
    if (payout_floor_sats < COINBASE_DUST_SATS)
        payout_floor_sats = COINBASE_DUST_SATS;
    /* What is left for payouts once everything that is not a payout has been
     * paid for: the transaction envelope, the scriptSig, the operator output
     * and — the term that actually binds on a drivechain pool — the
     * commitment OP_RETURNs the template already carries. */
    size_t payout_budget = max_coinbase_bytes > fixed_bytes
                         ? max_coinbase_bytes - fixed_bytes : 0;

    int64_t fee_sats = 0;
    cb_repl_out_t op;
    memset(&op, 0, sizeof op);
    int has_operator = 0;
    if (operator_address && operator_address[0] && fee_bps > 0) {
        int64_t f = (value_sats * (int64_t)fee_bps) / 10000;
        if (f >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(operator_address, op.spk,
                                           sizeof op.spk, &op.spk_len,
                                           errbuf, errlen) < 0) return -1;
            fee_sats = f;
            has_operator = 1;
        }
    }

    int64_t payable = value_sats - fee_sats;
    int64_t claimed = 0;
    for (size_t i = 0; i < n_payees; ++i) {
        if (!payees[i].address || !payees[i].address[0]) {
            set_err(errbuf, errlen, "payee %zu has no address", i);
            return -1;
        }
        if (payees[i].sats < 0) {
            set_err(errbuf, errlen, "payee %zu has a negative amount", i);
            return -1;
        }
        claimed += payees[i].sats;
    }
    if (claimed != payable) {
        set_err(errbuf, errlen,
                "payees sum to %lld but the block pays %lld after a %lld fee",
                (long long)claimed, (long long)payable, (long long)fee_sats);
        return -1;
    }

    /* Paid in the order the CALLER gave, not in one chosen here.
     *
     * This used to sort largest-first internally, so the floor and the byte
     * budget always fell on the smallest claims. That is the right default and
     * it is still what pplns.c hands over -- but it can only ever be a
     * default, because "who gets the slots a coinbase has room for" is policy,
     * and a policy fixed inside the builder cannot be changed without changing
     * the builder. Specifically it made the slots unwinnable: a large miner's
     * share of the window beats any priority a small one can accumulate, so
     * the same addresses take the same slots every block for ever.
     *
     * The caller now owns the order and this pays greedily down it. See
     * pplns_order_claims(). */
    size_t  n = 0;
    size_t  payout_bytes = 0;
    int64_t dropped = 0;
    for (size_t k = 0; k < n_payees; ++k) {
        const coinbase_payee_t *pe = &payees[k];
        if (pe->sats < payout_floor_sats) {
            r.dropped_below_floor++; dropped += pe->sats; continue;
        }
        if (n + 1 >= cap) {          /* storage, not policy */
            r.dropped_capped++; dropped += pe->sats; continue;
        }
        /* Resolve first: an output's cost depends on its address type, and a
         * P2TR payout is 43 bytes against a P2WPKH one's 31. Budgeting at a
         * fixed per-output figure would let more through than actually fit. */
        if (coinbase_address_to_script(pe->address, out[n].spk,
                                       sizeof out[n].spk, &out[n].spk_len,
                                       errbuf, errlen) < 0) {
            return -1;
        }
        size_t cost = out_ser_size(out[n].spk_len);
        if (payout_bytes + cost > payout_budget) {
            /* No room. Keep going rather than breaking: a later payee may be
             * a cheaper address type and still fit, and dropping it would
             * forfeit money that could have been paid. */
            r.dropped_capped++; dropped += pe->sats; continue;
        }
        payout_bytes += cost;
        out[n].sats = pe->sats;
        r.paid_sats += pe->sats;
        n++; r.paid_count++;
    }

    if (r.paid_count == 0) {
        set_err(errbuf, errlen,
                "no payee fits: %zu-byte coinbase budget leaves %zu bytes for "
                "payouts after %zu bytes of transaction and commitments, and "
                "nothing clears the %lld-sat payout floor",
                max_coinbase_bytes, payout_budget, fixed_bytes,
                (long long)payout_floor_sats);
        return -1;
    }

    /* Whatever could not be paid is REDISTRIBUTED ACROSS THE MINERS WHO COULD.
     *
     * It used to go to the operator, on the reasoning that value not paid out
     * is value destroyed and the operator output is the only place left. The
     * first half is true; the second was wrong, and the measurement that
     * settled it is worth keeping. With 100 miners on a 1/n hashrate spread
     * and the default 1000-byte budget, 28 are paid, 72 are cut by the byte
     * cap, NONE by the dust floor -- and the operator received 25% of the
     * block on a 1% fee. The rule was defended as a dust policy and dust was
     * never involved.
     *
     * Two things made it indefensible rather than merely harsh. A miner's
     * window share tracks its hashrate, so the same miners fall below the
     * cut every block: it pays them nothing ever, rather than occasionally.
     * And it paid the operator MORE the smaller the coinbase, so an operator
     * maximised revenue by starving its own miners -- 46% of the block at a
     * 400-byte budget against 2% at 3000.
     *
     * Redistributing keeps every property the forfeit had. The block still
     * pays out to the satoshi, the pool still holds nothing, and no ledger
     * appears. What changes is only who receives what the coinbase had no
     * room for: the other miners, not the house.
     *
     * (LayerTwo-Labs/simplepool#76, and Wired4ncer, who ran the pool that
     * showed it.) */
    if (dropped > 0) {
        /* Scale the survivors up to spend `payable` exactly. Amounts do not
         * affect an output's size -- a value is 8 bytes whatever it holds --
         * so this cannot break the byte budget just measured. */
        int64_t assigned = 0;
        for (size_t i = 0; i < n; ++i) {
            out[i].sats = (int64_t)((double)payable *
                                    ((double)out[i].sats / (double)r.paid_sats));
            assigned += out[i].sats;
        }
        /* Truncation again, and the same rule as everywhere else: the
         * remainder goes to the largest surviving claim. Found by scanning
         * rather than assumed to be out[0] -- that was only true while this
         * function did its own largest-first sort, and a caller-supplied
         * order can put anyone first. */
        if (assigned < payable) {
            size_t big = 0;
            for (size_t i = 1; i < n; ++i)
                if (out[i].sats > out[big].sats) big = i;
            out[big].sats += payable - assigned;
        }
        else if (assigned > payable) {
            set_err(errbuf, errlen, "internal: redistribution overshot");
            return -1;
        }
        r.redistributed_sats = dropped;
        r.paid_sats = payable;
    }

    /* The operator now receives its fee and nothing else. */
    int64_t operator_out = fee_sats;
    if (operator_out > 0 && !has_operator) {
        if (!operator_address || !operator_address[0]) {
            set_err(errbuf, errlen,
                    "a %lld-sat operator fee has no operator_address to "
                    "receive it", (long long)operator_out);
            return -1;
        }
        if (coinbase_address_to_script(operator_address, op.spk, sizeof op.spk,
                                       &op.spk_len, errbuf, errlen) < 0) return -1;
        has_operator = 1;
    }
    if (has_operator && operator_out > 0) {
        if (n >= cap) { set_err(errbuf, errlen, "internal: repl cap"); return -1; }
        op.sats = operator_out;
        out[n++] = op;
    }

    r.fee_sats = fee_sats;
    if (out_n) *out_n = n;
    if (res) *res = r;
    return 0;
}

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
                          char *errbuf, size_t errlen) {
    if (res) { coinbase_window_result_t z; memset(&z, 0, sizeof z); *res = z; }
    if (!out) { set_err(errbuf, errlen, "null arg"); return -1; }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;

    /* Everything the coinbase costs before a single miner is paid. The byte
     * budget is the whole transaction, so the payouts get what is left. */
    size_t wc_probe_len = 0;
    if (witness_commitment_hex && *witness_commitment_hex)
        wc_probe_len = strlen(witness_commitment_hex) / 2;
    size_t tag_len_probe = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t t = strlen(coinbase_tag);
        tag_len_probe = (t > 75 ? 75 : t) + 1;
    }
    uint8_t hp_probe[8];
    size_t ss_probe = bip34_height_push(height, hp_probe) + tag_len_probe
                    + extranonce1_size + extranonce2_size;
    size_t fixed = 4 + 1 + 36 + (ss_probe < 253 ? 1 : 3) + ss_probe
                 + 4 + 3 /* output-count varint, conservatively */ + 4;
    if (wc_probe_len) fixed += out_ser_size(wc_probe_len);
    /* Reserve the operator output whether or not it turns out to be needed:
     * forfeits land on it, and forfeits are exactly what happens when the
     * budget bites. Conservative by ~31 bytes if it is absent. */
    if (operator_address && operator_address[0]) fixed += out_ser_size(34);

    /* Same resolver the template builder uses: the split is one rule, in one
     * place, whatever kind of coinbase it ends up in. */
    cb_repl_out_t repl[CB_MAX_REPL_OUTS];
    size_t n_repl = 0;
    if (resolve_window_outputs(value_sats, payees, n_payees, operator_address,
                               fee_bps, max_coinbase_bytes, fixed,
                               payout_floor_sats, repl,
                               CB_MAX_REPL_OUTS, &n_repl, res,
                               errbuf, errlen) < 0) {
        return -1;
    }

    bbuf_t outs;
    bbuf_init(&outs);
    uint64_t n_outputs = 0;
    int64_t  emitted = 0;
    for (size_t i = 0; i < n_repl; ++i) {
        if (bbuf_push_u64_le(&outs, (uint64_t)repl[i].sats) < 0) goto oom;
        if (bbuf_push_varint(&outs, repl[i].spk_len) < 0) goto oom;
        if (bbuf_push(&outs, repl[i].spk, repl[i].spk_len) < 0) goto oom;
        n_outputs++;
        emitted += repl[i].sats;
    }

    uint8_t wc_buf[256];
    size_t  wc_len = 0;
    if (witness_commitment_hex && *witness_commitment_hex) {
        if (hex_decode(witness_commitment_hex, wc_buf, sizeof wc_buf, &wc_len) < 0) {
            bbuf_free(&outs);
            set_err(errbuf, errlen, "bad witness commitment hex");
            return -1;
        }
        if (bbuf_push_u64_le(&outs, 0) < 0) goto oom;
        if (bbuf_push_varint(&outs, wc_len) < 0) goto oom;
        if (bbuf_push(&outs, wc_buf, wc_len) < 0) goto oom;
        n_outputs++;
    }

    /* Every satoshi is accounted for, or the block burns the difference. */
    if (emitted != value_sats) {
        bbuf_free(&outs);
        set_err(errbuf, errlen, "internal: outputs sum to %lld, block pays %lld",
                (long long)emitted, (long long)value_sats);
        return -1;
    }

    uint8_t height_push[8];
    size_t  height_push_len = bip34_height_push(height, height_push);
    uint8_t tag_push[80];
    size_t  tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }
    size_t en_total = extranonce1_size + extranonce2_size;
    size_t script_sig_len = height_push_len + tag_push_len + en_total;
    if (script_sig_len < 2 || script_sig_len > 100) {
        bbuf_free(&outs);
        set_err(errbuf, errlen, "coinbase scriptSig length %zu out of range "
                "(height %zu + tag %zu + extranonce %zu)",
                script_sig_len, height_push_len, tag_push_len, en_total);
        return -1;
    }

    bbuf_t c1, c2;
    bbuf_init(&c1);
    bbuf_init(&c2);
    if (bbuf_push_u32_le(&c1, 2) < 0) goto oom2;
    if (bbuf_push_varint(&c1, 1) < 0) goto oom2;
    for (int i = 0; i < 32; ++i) if (bbuf_push_u8(&c1, 0) < 0) goto oom2;
    if (bbuf_push_u32_le(&c1, 0xffffffffu) < 0) goto oom2;
    if (bbuf_push_varint(&c1, script_sig_len) < 0) goto oom2;
    if (bbuf_push(&c1, height_push, height_push_len) < 0) goto oom2;
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) goto oom2;
    if (bbuf_push_u32_le(&c2, 0xffffffffu) < 0) goto oom2;
    if (bbuf_push_varint(&c2, n_outputs) < 0) goto oom2;
    if (bbuf_push(&c2, outs.data, outs.len) < 0) goto oom2;
    if (bbuf_push_u32_le(&c2, 0) < 0) goto oom2;

    bbuf_free(&outs);
    out->cb1 = c1.data; out->cb1_len = c1.len;
    out->cb2 = c2.data; out->cb2_len = c2.len;
    return 0;

oom2:
    bbuf_free(&c1); bbuf_free(&c2);
oom:
    bbuf_free(&outs);
    set_err(errbuf, errlen, "oom");
    return -1;
}

static int build_from_template_impl(const char *coinbase_tx_hex,
                                    cb_repl_fn repl_fn, void *repl_ctx,
                                    const char *coinbase_tag,
                                    size_t extranonce1_size,
                                    size_t extranonce2_size,
                                    coinbase_parts_t *out,
                                    int *out_has_witness,
                                    char *errbuf, size_t errlen) {
    if (!out || !coinbase_tx_hex || !repl_fn) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    out->cb1 = NULL; out->cb1_len = 0;
    out->cb2 = NULL; out->cb2_len = 0;
    if (out_has_witness) *out_has_witness = 0;

    cb_repl_out_t repl[CB_MAX_REPL_OUTS];
    size_t n_repl = 0;

    struct cb_out { uint64_t value; size_t spk_off; size_t spk_len; int op_return; };

    uint8_t *tx = NULL;
    struct cb_out *outs = NULL;
    bbuf_t c1, ob, c2;
    bbuf_init(&c1); bbuf_init(&ob); bbuf_init(&c2);
    int ret = -1;

    /* Decode the coinbase tx hex into bytes. */
    size_t hexlen = strlen(coinbase_tx_hex);
    if (hexlen % 2 != 0) { set_err(errbuf, errlen, "odd coinbasetxn hex"); goto done; }
    size_t txlen = hexlen / 2;
    tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) { set_err(errbuf, errlen, "oom"); goto done; }
    size_t dl = 0;
    if (hex_decode(coinbase_tx_hex, tx, txlen, &dl) < 0 || dl != txlen) {
        set_err(errbuf, errlen, "bad coinbasetxn hex");
        goto done;
    }

    /* Parse: version | [marker flag] | vin | input | vout | outputs |
     *        [witness] | locktime. */
    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }

    int has_witness = 0;
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) {
        has_witness = 1;     /* segwit marker (0x00) + flag (nonzero) */
        off += 2;
    }

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) { set_err(errbuf, errlen, "truncated coinbasetxn"); goto done; }
    if (vin != 1) { set_err(errbuf, errlen, "coinbasetxn input count %llu != 1", (unsigned long long)vin); goto done; }

    size_t prevout_off = off;            /* 32-byte hash + 4-byte index */
    if (off + 36 > txlen) { set_err(errbuf, errlen, "truncated coinbasetxn input"); goto done; }
    off += 36;
    uint64_t ss_len = 0;
    if (rd_varint(tx, txlen, &off, &ss_len) < 0) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    size_t ss_off = off;
    if (off + ss_len > txlen) { set_err(errbuf, errlen, "truncated scriptSig"); goto done; }
    off += ss_len;
    uint32_t sequence;
    if (rd_u32(tx, txlen, &off, &sequence) < 0) { set_err(errbuf, errlen, "truncated sequence"); goto done; }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) { set_err(errbuf, errlen, "truncated output count"); goto done; }
    if (vout == 0) { set_err(errbuf, errlen, "coinbasetxn has no outputs"); goto done; }

    outs = (struct cb_out *)calloc((size_t)vout, sizeof(*outs));
    if (!outs) { set_err(errbuf, errlen, "oom"); goto done; }

    int64_t reward_idx = -1;
    int reward_count = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) { set_err(errbuf, errlen, "truncated output value"); goto done; }
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        if (off + spk_len > txlen) { set_err(errbuf, errlen, "truncated scriptPubKey"); goto done; }
        outs[i].value = val;
        outs[i].spk_off = off;
        outs[i].spk_len = (size_t)spk_len;
        outs[i].op_return = (spk_len >= 1 && tx[off] == 0x6a); /* OP_RETURN */
        off += spk_len;
        if (!outs[i].op_return) { reward_idx = (int64_t)i; reward_count++; }
    }

    /* Skip the input witness (single input) to reach the locktime. */
    if (has_witness) {
        uint64_t stack = 0;
        if (rd_varint(tx, txlen, &off, &stack) < 0) { set_err(errbuf, errlen, "truncated witness"); goto done; }
        for (uint64_t i = 0; i < stack; i++) {
            uint64_t il = 0;
            if (rd_varint(tx, txlen, &off, &il) < 0) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            if (off + il > txlen) { set_err(errbuf, errlen, "truncated witness item"); goto done; }
            off += il;
        }
    }

    uint32_t locktime;
    if (rd_u32(tx, txlen, &off, &locktime) < 0) { set_err(errbuf, errlen, "truncated locktime"); goto done; }
    if (off != txlen) { set_err(errbuf, errlen, "coinbasetxn trailing bytes"); goto done; }

    /* The server pays the reward to exactly one spendable output; we hand
     * that value to the miner. Refuse to guess if that assumption breaks. */
    if (reward_count != 1) {
        set_err(errbuf, errlen, "coinbasetxn has %d spendable outputs (expected 1)", reward_count);
        goto done;
    }
    int64_t reward = (int64_t)outs[reward_idx].value;

    /* What this coinbase costs before any miner is paid, so the resolver can
     * spend what is left. On a drivechain pool the dominant term here is the
     * commitment OP_RETURNs the enforcer put in the template: they are why
     * the same 16 payouts can fit under one budget and not another, and why
     * a cap counted in outputs cannot express the limit at all. */
    size_t tag_probe = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t t = strlen(coinbase_tag);
        tag_probe = (t > 75 ? 75 : t) + 1;
    }
    size_t ss_probe = (size_t)ss_len + tag_probe
                    + extranonce1_size + extranonce2_size;
    size_t fixed_bytes = 4 + 1 + 36 + (ss_probe < 253 ? 1 : 3) + ss_probe
                       + 4 + 3 /* output-count varint, conservatively */ + 4;
    for (uint64_t i = 0; i < vout; i++) {
        if ((int64_t)i == reward_idx) continue;
        fixed_bytes += out_ser_size(outs[i].spk_len);
    }

    /* Hand the reward to the caller's resolver: one miner and a fee, or a
     * whole PPLNS window. Either way it comes back as concrete outputs, and
     * this function does not care which it was. */
    if (repl_fn(repl_ctx, reward, fixed_bytes, repl, CB_MAX_REPL_OUTS, &n_repl,
                errbuf, errlen) < 0) goto done;
    if (n_repl == 0) {
        set_err(errbuf, errlen, "resolver produced no outputs");
        goto done;
    }
    /* Whatever the split, it must spend the reward exactly: paying out less
     * than the template allows forfeits the difference to nobody. */
    int64_t repl_total = 0;
    for (size_t i = 0; i < n_repl; ++i) repl_total += repl[i].sats;
    if (repl_total != reward) {
        set_err(errbuf, errlen,
                "replacement outputs sum to %lld, template reward is %lld",
                (long long)repl_total, (long long)reward);
        goto done;
    }

    /* Optional coinbase tag, appended into the scriptSig. */
    uint8_t tag_push[80]; size_t tag_push_len = 0;
    if (coinbase_tag && *coinbase_tag) {
        size_t tlen = strlen(coinbase_tag);
        if (tlen > 75) tlen = 75;
        tag_push[0] = (uint8_t)tlen;
        memcpy(tag_push + 1, coinbase_tag, tlen);
        tag_push_len = tlen + 1;
    }

    /* New scriptSig = server scriptSig (BIP34 height + any server data) +
     * tag + extranonce placeholder. Coinbase scriptSig is capped at 100. */
    size_t en_total = extranonce1_size + extranonce2_size;
    uint64_t new_ss_len = ss_len + tag_push_len + en_total;
    if (new_ss_len < 2 || new_ss_len > 100) {
        set_err(errbuf, errlen, "coinbase scriptSig length %llu out of range",
                (unsigned long long)new_ss_len);
        goto done;
    }

    /* cb1: version | vin(1) | prevout(36) | varint(scriptSig_len) |
     *      server scriptSig | tag   (extranonce slots follow). */
    if (bbuf_push_u32_le(&c1, version) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, 1) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c1, tx + prevout_off, 36) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c1, new_ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (ss_len && bbuf_push(&c1, tx + ss_off, (size_t)ss_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (tag_push_len && bbuf_push(&c1, tag_push, tag_push_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    /* Outputs: replace the reward output, preserve everything else in order. */
    uint64_t new_vout = vout - 1u + (uint64_t)n_repl;
    for (uint64_t i = 0; i < vout; i++) {
        if ((int64_t)i == reward_idx) {
            for (size_t k = 0; k < n_repl; ++k) {
                if (bbuf_push_u64_le(&ob, (uint64_t)repl[k].sats) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push_varint(&ob, repl[k].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
                if (bbuf_push(&ob, repl[k].spk, repl[k].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            }
        } else {
            if (bbuf_push_u64_le(&ob, outs[i].value) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push_varint(&ob, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
            if (bbuf_push(&ob, tx + outs[i].spk_off, outs[i].spk_len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
        }
    }

    /* cb2: sequence | varint(out_count) | outputs | locktime. */
    if (bbuf_push_u32_le(&c2, sequence) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_varint(&c2, new_vout) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push(&c2, ob.data, ob.len) < 0) { set_err(errbuf, errlen, "oom"); goto done; }
    if (bbuf_push_u32_le(&c2, locktime) < 0) { set_err(errbuf, errlen, "oom"); goto done; }

    /* Success — hand ownership of c1/c2 buffers to the caller. */
    out->cb1 = c1.data; out->cb1_len = c1.len; c1.data = NULL;
    out->cb2 = c2.data; out->cb2_len = c2.len; c2.data = NULL;
    if (out_has_witness) *out_has_witness = has_witness;
    ret = 0;

done:
    bbuf_free(&c1);
    bbuf_free(&ob);
    bbuf_free(&c2);
    free(outs);
    free(tx);
    return ret;
}


/* ---- resolvers ---------------------------------------------------------- */

/* One miner and an optional operator fee: the original behaviour, unchanged. */
typedef struct {
    const char *miner_address;
    const char *operator_address;
    int         fee_bps;
    int64_t    *out_miner_sats;
    int64_t    *out_fee_sats;
} repl_single_ctx_t;

static int repl_single(void *vctx, int64_t reward, size_t fixed_bytes,
                       cb_repl_out_t *out, size_t cap, size_t *out_n,
                       char *errbuf, size_t errlen) {
    repl_single_ctx_t *c = vctx;
    (void)fixed_bytes;   /* one miner and a fee always fit */
    if (cap < 2) { set_err(errbuf, errlen, "internal: repl cap"); return -1; }
    size_t n = 0;
    int64_t fee_sats = 0, miner_sats = reward;
    cb_repl_out_t op;
    memset(&op, 0, sizeof op);
    if (c->operator_address && c->operator_address[0] && c->fee_bps > 0 && reward > 0) {
        int64_t f = (reward * (int64_t)c->fee_bps) / 10000;
        if (f >= COINBASE_DUST_SATS) {
            if (coinbase_address_to_script(c->operator_address, op.spk,
                                           sizeof op.spk, &op.spk_len,
                                           errbuf, errlen) < 0) return -1;
            fee_sats = f;
            miner_sats = reward - f;
            op.sats = f;
        }
    }
    if (coinbase_address_to_script(c->miner_address, out[n].spk,
                                   sizeof out[n].spk, &out[n].spk_len,
                                   errbuf, errlen) < 0) return -1;
    out[n].sats = miner_sats;
    n++;
    if (fee_sats > 0) out[n++] = op;
    *out_n = n;
    if (c->out_miner_sats) *c->out_miner_sats = miner_sats;
    if (c->out_fee_sats)   *c->out_fee_sats   = fee_sats;
    return 0;
}

/* A whole PPLNS window, on exactly the terms coinbase_build_window() uses --
 * the same resolver, so a drivechain pool and a plain-bitcoind pool cannot
 * split a window differently. */
typedef struct {
    const coinbase_payee_t   *payees;
    size_t                    n_payees;
    const char               *operator_address;
    int                       fee_bps;
    size_t                    max_coinbase_bytes;
    int64_t                   payout_floor_sats;
    coinbase_window_result_t *res;
} repl_window_ctx_t;

static int repl_window(void *vctx, int64_t reward, size_t fixed_bytes,
                       cb_repl_out_t *out, size_t cap, size_t *out_n,
                       char *errbuf, size_t errlen) {
    repl_window_ctx_t *c = vctx;
    return resolve_window_outputs(reward, c->payees, c->n_payees,
                                  c->operator_address, c->fee_bps,
                                  c->max_coinbase_bytes, fixed_bytes,
                                  c->payout_floor_sats,
                                  out, cap, out_n, c->res, errbuf, errlen);
}

/* ---- public template builders ------------------------------------------- */

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
                                 char *errbuf, size_t errlen) {
    if (!miner_address) { set_err(errbuf, errlen, "null arg"); return -1; }
    if (out_miner_sats) *out_miner_sats = 0;
    if (out_fee_sats)   *out_fee_sats   = 0;
    repl_single_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.miner_address = miner_address;
    ctx.operator_address = operator_address;
    ctx.fee_bps = fee_bps;
    ctx.out_miner_sats = out_miner_sats;
    ctx.out_fee_sats = out_fee_sats;
    return build_from_template_impl(coinbase_tx_hex, repl_single, &ctx,
                                    coinbase_tag, extranonce1_size,
                                    extranonce2_size, out, out_has_witness,
                                    errbuf, errlen);
}

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
                                        char *errbuf, size_t errlen) {
    if (res) { coinbase_window_result_t z; memset(&z, 0, sizeof z); *res = z; }
    if (!payees || n_payees == 0) {
        set_err(errbuf, errlen, "window is empty: nobody to pay");
        return -1;
    }
    repl_window_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.payees = payees;
    ctx.n_payees = n_payees;
    ctx.operator_address = operator_address;
    ctx.fee_bps = fee_bps;
    ctx.max_coinbase_bytes = max_coinbase_bytes;
    ctx.payout_floor_sats = payout_floor_sats;
    ctx.res = res;
    return build_from_template_impl(coinbase_tx_hex, repl_window, &ctx,
                                    coinbase_tag, extranonce1_size,
                                    extranonce2_size, out, out_has_witness,
                                    errbuf, errlen);
}

/* Count the outputs of a serialized coinbase, split into spendable and
 * OP_RETURN.
 *
 * The OP_RETURN count is the interesting one: a coinbase built from
 * "coinbasevalue" carries exactly one (the witness commitment), while one
 * dictated by the CUSF enforcer carries the mandatory BIP300/301 commitments
 * alongside it. Surfacing the number is how an observer can tell, from the
 * outside, whether the blocks this pool produces can have a sidechain
 * merge-mined into them at all.
 *
 * Parses only far enough to walk the output list. Returns 0 on success,
 * negative on malformed input; counts are untouched on failure. */
/* The reward the template's coinbase actually pays. See coinbase.h for why a
 * caller must divide this rather than the node's `coinbasevalue` field.
 *
 * Implemented on top of the replacement parser rather than a second walk of
 * the transaction, so it cannot disagree with the builder about which output
 * is the spendable one -- disagreeing there is the entire failure this
 * function exists to prevent. */
static int cb_reward_probe(void *ctx, int64_t reward_sats, size_t fixed_bytes,
                           cb_repl_out_t *out, size_t cap, size_t *out_n,
                           char *errbuf, size_t errlen);

size_t coinbase_expected_payout_slots(size_t max_coinbase_bytes,
                                      const char *coinbase_tx_hex,
                                      const char *const *addresses,
                                      size_t n_addresses)
{
    size_t budget = max_coinbase_bytes ? max_coinbase_bytes
                                       : (size_t)COINBASE_DEFAULT_MAX_BYTES;
    /* The envelope the builder always pays: version, input, scriptSig with a
     * generous extranonce and tag, output count, locktime, and a reserved
     * operator output. Deliberately on the pessimistic side -- reserving one
     * slot too few costs a rotation, reserving one too many costs a payout. */
    size_t fixed = 160;
    if (coinbase_tx_hex) {
        /* Everything the template already spends: its commitments, and the
         * output being replaced. Counted from the transaction rather than
         * guessed, because the same 16 payouts cost 817 bytes against four
         * drivechain OP_RETURNs and 769 against three. */
        size_t hexlen = strlen(coinbase_tx_hex);
        fixed += hexlen / 2;
        /* The spendable output goes away as the payouts arrive, so do not
         * charge for it twice. One P2WPKH-shaped output, approximately. */
        if (fixed > 31) fixed -= 31;
    }
    if (budget <= fixed) return 1;
    size_t room = budget - fixed;

    /* Charge each address what it actually costs, in the order the caller
     * means to pay them. Assuming a fixed 31 bytes was wrong by 24 slots on a
     * window of taproot addresses. */
    if (addresses && n_addresses > 0) {
        size_t used = 0, slots = 0;
        for (size_t i = 0; i < n_addresses; ++i) {
            uint8_t spk[64];
            size_t spk_len = 0;
            size_t cost;
            if (!addresses[i] ||
                coinbase_address_to_script(addresses[i], spk, sizeof spk,
                                           &spk_len, NULL, 0) < 0) {
                cost = out_ser_size(22);       /* unreadable: assume P2WPKH */
            } else {
                cost = out_ser_size(spk_len);
            }
            if (used + cost > room) break;
            used += cost;
            slots++;
        }
        if (slots < 1) slots = 1;
        if (slots > COINBASE_MAX_PAYOUT_OUTPUTS) slots = COINBASE_MAX_PAYOUT_OUTPUTS;
        return slots;
    }

    /* No window in hand: assume the common case. */
    size_t slots = room / 31;
    if (slots < 1) slots = 1;
    if (slots > COINBASE_MAX_PAYOUT_OUTPUTS) slots = COINBASE_MAX_PAYOUT_OUTPUTS;
    return slots;
}

int coinbase_template_reward(const char *coinbase_tx_hex, int64_t *out_sats) {
    if (!coinbase_tx_hex || !out_sats) return -1;
    int spendable = 0;
    if (coinbase_count_outputs(coinbase_tx_hex, &spendable, NULL) < 0) return -1;
    if (spendable != 1) return -1;

    int64_t reward = -1;
    coinbase_parts_t throwaway;
    char err[256] = {0};
    /* The replacement machinery hands the callback the reward it computed;
     * we keep the number and put the output back unchanged. */
    if (build_from_template_impl(coinbase_tx_hex, cb_reward_probe, &reward,
                                 NULL, 4, 4, &throwaway, NULL,
                                 err, sizeof err) < 0) {
        return -1;
    }
    coinbase_parts_free(&throwaway);
    if (reward < 0) return -1;
    *out_sats = reward;
    return 0;
}

/* Records the reward and re-emits the output the builder would have replaced,
 * so the throwaway coinbase this produces is byte-identical to the input. */
static int cb_reward_probe(void *ctx, int64_t reward_sats, size_t fixed_bytes,
                           cb_repl_out_t *out, size_t cap, size_t *out_n,
                           char *errbuf, size_t errlen) {
    (void)fixed_bytes;
    if (!ctx || !out || cap < 1) {
        set_err(errbuf, errlen, "reward probe: bad arg");
        return -1;
    }
    *(int64_t *)ctx = reward_sats;
    /* An OP_TRUE output: never broadcast, only measured. */
    out[0].sats = reward_sats;
    out[0].spk[0] = 0x51;
    out[0].spk_len = 1;
    if (out_n) *out_n = 1;
    return 0;
}

int coinbase_count_outputs(const char *tx_hex, int *spendable_out,
                           int *op_return_out) {
    if (!tx_hex) return -1;
    size_t hexlen = strlen(tx_hex);
    if (hexlen % 2 != 0) return -1;
    size_t txlen = hexlen / 2;
    uint8_t *tx = (uint8_t *)malloc(txlen ? txlen : 1);
    if (!tx) return -1;

    int ret = -1;
    size_t dl = 0;
    if (hex_decode(tx_hex, tx, txlen, &dl) < 0 || dl != txlen) goto done;

    size_t off = 0;
    uint32_t version;
    if (rd_u32(tx, txlen, &off, &version) < 0) goto done;
    /* Optional segwit marker+flag. */
    if (off + 2 <= txlen && tx[off] == 0x00 && tx[off + 1] != 0x00) off += 2;

    uint64_t vin = 0;
    if (rd_varint(tx, txlen, &off, &vin) < 0) goto done;
    for (uint64_t i = 0; i < vin; i++) {
        if (off + 36 > txlen) goto done;
        off += 36;
        uint64_t ss = 0;
        if (rd_varint(tx, txlen, &off, &ss) < 0) goto done;
        if (off + ss + 4 > txlen) goto done;
        off += (size_t)ss + 4;   /* scriptSig + sequence */
    }

    uint64_t vout = 0;
    if (rd_varint(tx, txlen, &off, &vout) < 0) goto done;
    int spendable = 0, op_returns = 0;
    for (uint64_t i = 0; i < vout; i++) {
        uint64_t val = 0, spk_len = 0;
        if (rd_u64(tx, txlen, &off, &val) < 0) goto done;
        if (rd_varint(tx, txlen, &off, &spk_len) < 0) goto done;
        if (off + spk_len > txlen) goto done;
        if (spk_len >= 1 && tx[off] == 0x6a) op_returns++;   /* OP_RETURN */
        else spendable++;
        off += (size_t)spk_len;
    }

    if (spendable_out)  *spendable_out  = spendable;
    if (op_return_out)  *op_return_out  = op_returns;
    ret = 0;
done:
    free(tx);
    return ret;
}
