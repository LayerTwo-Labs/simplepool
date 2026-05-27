#include "coinbase.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal varint reader. */
static int read_varint(const uint8_t *buf, size_t cap, size_t *off, uint64_t *val) {
    if (*off >= cap) return -1;
    uint8_t b = buf[(*off)++];
    if (b < 0xfd) { *val = b; return 0; }
    if (b == 0xfd) {
        if (*off + 2 > cap) return -1;
        *val = (uint64_t)buf[*off] | ((uint64_t)buf[*off + 1] << 8);
        *off += 2;
        return 0;
    }
    if (b == 0xfe) {
        if (*off + 4 > cap) return -1;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)buf[*off + i] << (8 * i);
        *off += 4;
        *val = v;
        return 0;
    }
    if (*off + 8 > cap) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[*off + i] << (8 * i);
    *off += 8;
    *val = v;
    return 0;
}

static void test_p2pkh_address(void) {
    /* mainnet P2PKH: 1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa (genesis coinbase) */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    assert(spk_len == 25);
    assert(spk[0] == 0x76 && spk[1] == 0xa9 && spk[2] == 0x14);
    assert(spk[23] == 0x88 && spk[24] == 0xac);
    printf("ok: p2pkh decode\n");
}

static void test_p2wpkh_address(void) {
    /* BIP173 test vector: bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4 */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    assert(spk_len == 22);
    assert(spk[0] == 0x00 && spk[1] == 0x14);
    printf("ok: p2wpkh decode\n");
}

static void test_regtest_p2wpkh(void) {
    /* A canonical regtest P2WPKH address. */
    uint8_t spk[64];
    size_t spk_len = 0;
    char err[128];
    int rc = coinbase_address_to_script("bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
                                        spk, sizeof spk, &spk_len, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "decode err: %s\n", err);
    }
    assert(rc == 0);
    assert(spk_len == 22);
    assert(spk[0] == 0x00 && spk[1] == 0x14);
    printf("ok: regtest p2wpkh decode\n");
}

static void test_build_coinbase_structural(void) {
    coinbase_parts_t parts = {0};
    char err[256];
    /* witness commitment: OP_RETURN OP_PUSHBYTES_36 aa21a9ed + 32 bytes */
    char wc_hex[2 + 2 + 8 + 64 + 1] = {0};
    /* "6a24aa21a9ed" + 32 bytes of "ab" */
    snprintf(wc_hex, sizeof wc_hex, "6a24aa21a9ed");
    for (int i = 0; i < 32; i++) {
        char tmp[3];
        snprintf(tmp, sizeof tmp, "ab");
        strcat(wc_hex, tmp);
    }

    int rc = coinbase_build(800000, 625000000,
                            "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
                            wc_hex, "/drivepool/", 4, 4,
                            &parts, err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "coinbase_build err: %s\n", err);
    }
    assert(rc == 0);

    /* Assemble cb1 || en1(4 zeros) || en2(4 zeros) || cb2. */
    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    /* Parse: version(4) | varint(in_count) | prev_hash(32) | prev_idx(4) |
     *        varint(scriptSig_len) | scriptSig | sequence(4) |
     *        varint(out_count) | outputs | locktime(4) */
    size_t off = 0;
    /* version */
    uint32_t version = 0;
    for (int i = 0; i < 4; i++) version |= (uint32_t)tx[off + i] << (8 * i);
    off += 4;
    assert(version == 1);

    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    assert(in_count == 1);

    /* prev hash: 32 zeros */
    for (int i = 0; i < 32; i++) assert(tx[off + i] == 0);
    off += 32;

    /* prev idx: 0xffffffff */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    /* scriptSig must start with BIP34 height push: 0x03 0x00 0x35 0x0c (800000 LE) */
    assert(tx[off] == 0x03);
    assert(tx[off + 1] == 0x00);
    assert(tx[off + 2] == 0x35);
    assert(tx[off + 3] == 0x0c);
    off += ss_len;

    /* sequence */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 2); /* payout + witness commitment */

    /* output 0: value */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 625000000);
    uint64_t spk_len = 0;
    assert(read_varint(tx, total, &off, &spk_len) == 0);
    assert(spk_len == 22); /* P2WPKH scriptPubKey */
    off += spk_len;

    /* output 1: zero value + OP_RETURN script */
    uint64_t v2 = 0;
    for (int i = 0; i < 8; i++) v2 |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v2 == 0);
    uint64_t spk2_len = 0;
    assert(read_varint(tx, total, &off, &spk2_len) == 0);
    assert(spk2_len == 38); /* 6a 24 aa21a9ed + 32 */
    assert(tx[off] == 0x6a);
    off += spk2_len;

    /* locktime */
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0);
    off += 4;
    assert(off == total);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: structural coinbase parse\n");
}

/* Split builder: at 100 bps (1%) on 50 BTC subsidy, miner gets
 * 4_950_000_000 sats and operator gets 50_000_000 sats. Below the dust
 * threshold the operator output is dropped and the miner gets everything. */
static void test_build_coinbase_split_fee_math(void) {
    coinbase_parts_t parts = {0};
    char err[256];
    int64_t miner_sats = 0, fee_sats = 0;

    /* Normal split: 1% of 50 BTC = 0.5 BTC. */
    int rc = coinbase_build_split(
        800000, 5000000000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        100, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 50000000LL);
    assert(miner_sats == 4950000000LL);
    coinbase_parts_free(&parts);

    /* fee_bps = 0 → no operator output, miner gets full value. */
    rc = coinbase_build_split(
        800000, 5000000000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        0, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 0);
    assert(miner_sats == 5000000000LL);
    coinbase_parts_free(&parts);

    /* fee below dust threshold (546 sats) → collapsed to miner-only. At
     * 100 bps, 30_000 sats subsidy gives 300 sats fee, below dust. */
    rc = coinbase_build_split(
        800000, 30000LL,
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
        100, NULL, "/simplepool/", 4, 4,
        &parts, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats   == 0);
    assert(miner_sats == 30000LL);
    coinbase_parts_free(&parts);

    printf("ok: coinbase split fee math\n");
}

/* BIP34 small-height regression: Bitcoin Core encodes heights 1..16 as
 * OP_N (single byte 0x50+n), not as the 2-byte push-data form. Getting
 * this wrong shows up on fresh regtest/signet chains as 'bad-cb-height'. */
static void test_bip34_small_height_uses_opn(void) {
    coinbase_parts_t parts;
    memset(&parts, 0, sizeof parts);
    char err[256] = {0};
    /* height = 5 should produce scriptSig starting with OP_5 = 0x55. */
    int rc = coinbase_build(5, 5000000000LL,
                            "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080",
                            NULL, "/simplepool/", 4, 4,
                            &parts, err, sizeof err);
    assert(rc == 0);

    /* Walk to the scriptSig as in the structural test. */
    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4; /* version */
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 32 + 4; /* prev hash + idx */
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    /* OP_5 (0x55) as the first byte of scriptSig. */
    assert(tx[off] == 0x55);
    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: bip34 small-height uses OP_N\n");
}

int main(void) {
    test_p2pkh_address();
    test_p2wpkh_address();
    test_regtest_p2wpkh();
    test_build_coinbase_structural();
    test_build_coinbase_split_fee_math();
    test_bip34_small_height_uses_opn();
    printf("test_coinbase: all tests passed\n");
    return 0;
}
