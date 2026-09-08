#include "coinbase.h"
#include "stratum.h"

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

/* A server-built coinbase like the CUSF enforcer returns: segwit-serialized,
 * version 2, scriptSig = BIP34 height push for 800000 only, three outputs —
 * a BIP301 commitment OP_RETURN, the spendable reward (50 BTC), and the segwit
 * witness commitment — plus a single 32-byte (all-zero) input witness. */
static const char *ENF_COINBASE_HEX =
    "02000000"                                                            /* version 2 */
    "0001"                                                                /* segwit marker + flag */
    "01"                                                                  /* vin = 1 */
    "0000000000000000000000000000000000000000000000000000000000000000"    /* prevout hash */
    "ffffffff"                                                            /* prevout index */
    "04" "0300350c"                                                       /* scriptSig: height 800000 */
    "ffffffff"                                                            /* sequence */
    "03"                                                                  /* vout = 3 */
    "0000000000000000" "06" "6a04deadbeef"                                /* out0: BIP301 commitment */
    "00f2052a01000000" "16" "0014" "1111111111111111111111111111111111111111" /* out1: reward 50 BTC */
    "0000000000000000" "26" "6a24aa21a9ed"
        "2222222222222222222222222222222222222222222222222222222222222222"    /* out2: witness commitment */
    "0120" "0000000000000000000000000000000000000000000000000000000000000000" /* input witness */
    "00000000";                                                           /* locktime */

#define ENF_ADDR "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"

/* Rebuild from a server-provided coinbase: the reward output is redirected to
 * the miner, the extranonce is spliced into the scriptSig, and the mandatory
 * commitment + witness-commitment outputs are preserved verbatim. */
static void test_build_from_template(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int has_witness = -1;
    int64_t miner_sats = 0, fee_sats = 0;

    int rc = coinbase_build_from_template(
        ENF_COINBASE_HEX, ENF_ADDR, NULL, 0, "/x/", 4, 4,
        &parts, &has_witness, &miner_sats, &fee_sats, err, sizeof err);
    if (rc != 0) fprintf(stderr, "build_from_template err: %s\n", err);
    assert(rc == 0);
    assert(has_witness == 1);
    assert(miner_sats == 5000000000LL);
    assert(fee_sats == 0);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);       /* extranonce1 */
    memset(tx + parts.cb1_len + 4, 0xbb, 4);   /* extranonce2 */
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 0;
    uint32_t version = 0;
    for (int i = 0; i < 4; i++) version |= (uint32_t)tx[off + i] << (8 * i);
    off += 4;
    assert(version == 2); /* preserved from the template, not forced to 1 */

    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    assert(in_count == 1);
    for (int i = 0; i < 32; i++) assert(tx[off + i] == 0);
    off += 32;
    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff);
    off += 4;

    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    assert(ss_len == 4 + 4 + 8); /* height(4) + tag "/x/"(4) + extranonce(8) */
    assert(tx[off] == 0x03 && tx[off + 1] == 0x00 &&
           tx[off + 2] == 0x35 && tx[off + 3] == 0x0c);     /* BIP34 height kept */
    assert(tx[off + 4] == 0x03 && tx[off + 5] == '/' &&
           tx[off + 6] == 'x' && tx[off + 7] == '/');       /* tag push */
    assert(tx[off + 8] == 0xaa && tx[off + 11] == 0xaa);    /* extranonce1 */
    assert(tx[off + 12] == 0xbb && tx[off + 15] == 0xbb);   /* extranonce2 */
    off += ss_len;

    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0xff); /* sequence */
    off += 4;

    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 3); /* commitment + redirected reward + witness commitment */

    /* out0: BIP301 commitment preserved. */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    uint64_t l = 0;
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 6 && tx[off] == 0x6a && tx[off + 1] == 0x04);
    off += l;

    /* out1: reward redirected to the miner (50 BTC, 22-byte P2WPKH). */
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 5000000000ULL);
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 22 && tx[off] == 0x00 && tx[off + 1] == 0x14);
    off += l;

    /* out2: witness commitment preserved. */
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)tx[off + i] << (8 * i);
    off += 8;
    assert(v == 0);
    assert(read_varint(tx, total, &off, &l) == 0);
    assert(l == 38 && tx[off] == 0x6a && tx[off + 1] == 0x24);
    off += l;

    for (int i = 0; i < 4; i++) assert(tx[off + i] == 0); /* locktime */
    off += 4;
    assert(off == total);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template (redirect + preserve commitments)\n");
}

/* With an operator fee, a fourth output (the operator payout) is inserted and
 * the reward is split, while the commitments are still preserved. */
static void test_build_from_template_fee_split(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int has_witness = 0;
    int64_t miner_sats = 0, fee_sats = 0;

    int rc = coinbase_build_from_template(
        ENF_COINBASE_HEX, ENF_ADDR, ENF_ADDR, 100, "/x/", 4, 4,
        &parts, &has_witness, &miner_sats, &fee_sats, err, sizeof err);
    assert(rc == 0);
    assert(fee_sats == 50000000LL);       /* 1% of 50 BTC */
    assert(miner_sats == 4950000000LL);

    size_t total = parts.cb1_len + 8 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts.cb1, parts.cb1_len);
    memset(tx + parts.cb1_len, 0xaa, 4);
    memset(tx + parts.cb1_len + 4, 0xbb, 4);
    memcpy(tx + parts.cb1_len + 8, parts.cb2, parts.cb2_len);

    size_t off = 4;
    uint64_t in_count = 0;
    assert(read_varint(tx, total, &off, &in_count) == 0);
    off += 36;
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);
    off += ss_len + 4;
    uint64_t out_count = 0;
    assert(read_varint(tx, total, &off, &out_count) == 0);
    assert(out_count == 4); /* commitment + miner + operator + witness commitment */

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: coinbase_build_from_template fee split\n");
}


/* coinbase_count_outputs: the OP_RETURN count is what tells an observer
 * whether these blocks can have a sidechain merge-mined into them. One
 * OP_RETURN is a bare witness commitment (a coinbase we built ourselves);
 * more means BIP300/301 commitments came down with the template.
 *
 * Counted against the real fixtures above rather than hand-written hex — a
 * miscounted length byte in a literal silently desyncs the parse and the
 * assertion that catches it tells you nothing about which byte was wrong. */
static void test_count_outputs(void) {
    int spend = -1, opret = -1;

    /* Server-provided coinbase: BIP301 commitment + reward + witness
     * commitment, and segwit-serialized, so the marker/flag and the trailing
     * witness must both be stepped over correctly. */
    assert(coinbase_count_outputs(ENF_COINBASE_HEX, &spend, &opret) == 0);
    assert(spend == 1);
    assert(opret == 2);

    /* A coinbase we built: reward + operator fee + witness commitment only.
     * One OP_RETURN means no sidechain commitments — the state that left
     * Thunder unable to advance. */
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                /* witness_commitment_hex */
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                /* coinbase_tag */ NULL,
                                4, 4, &parts, NULL, NULL, err, sizeof err) == 0);
    /* cb1 + extranonce1 + extranonce2 + cb2 is the coinbase a miner submits. */
    size_t n = parts.cb1_len * 2 + 16 + parts.cb2_len * 2 + 1;
    char *hex = (char *)malloc(n);
    assert(hex);
    size_t o = 0;
    for (size_t i = 0; i < parts.cb1_len; i++) o += (size_t)sprintf(hex + o, "%02x", parts.cb1[i]);
    o += (size_t)sprintf(hex + o, "%s", "0011223344556677");   /* en1 + en2 */
    for (size_t i = 0; i < parts.cb2_len; i++) o += (size_t)sprintf(hex + o, "%02x", parts.cb2[i]);
    hex[o] = '\0';

    spend = -1; opret = -1;
    assert(coinbase_count_outputs(hex, &spend, &opret) == 0);
    assert(spend == 2);      /* miner + operator */
    assert(opret == 1);      /* witness commitment only */
    free(hex);
    coinbase_parts_free(&parts);

    /* Malformed input must fail rather than report a plausible count. */
    assert(coinbase_count_outputs("00", &spend, &opret) < 0);
    assert(coinbase_count_outputs("abc", &spend, &opret) < 0);   /* odd length */
    assert(coinbase_count_outputs(NULL, &spend, &opret) < 0);

    /* Out-params are optional. */
    assert(coinbase_count_outputs(ENF_COINBASE_HEX, NULL, NULL) == 0);

    printf("ok: coinbase_count_outputs\n");
}

/* The network an address encodes, used when the block-template backend
 * cannot be asked (the CUSF enforcer answers only getblocktemplate and
 * submitblock). Deliberately coarse: several networks share version bytes
 * and HRPs, so the test pins that it reports what the encoding proves and
 * refuses to over-claim. */
static void test_address_network(void) {
    assert(strcmp(coinbase_address_network(
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4"), "main") == 0);
    assert(strcmp(coinbase_address_network(
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa"), "main") == 0);
    assert(strcmp(coinbase_address_network(
        "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"), "regtest") == 0);
    /* testnet and signet share the `tb` HRP — reporting either one alone
     * would be a guess dressed up as a fact. */
    assert(strcmp(coinbase_address_network(
        "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx"), "test/signet") == 0);

    /* A typo must read as "no idea", not as a network: it is the input to a
     * mainnet-vs-testnet mismatch warning, and a false negative there is a
     * burnt fee output. */
    assert(coinbase_address_network(
        "1A1zP1eP5QGefi2DMPTfTL5SLmv7Divfna") == NULL);
    assert(coinbase_address_network("") == NULL);
    assert(coinbase_address_network(NULL) == NULL);

    assert(coinbase_network_is_mainnet("main") == 1);
    assert(coinbase_network_is_mainnet("signet") == 0);
    assert(coinbase_network_is_mainnet("test") == 0);
    assert(coinbase_network_is_mainnet("regtest") == 0);
    /* Unrecognised must not read as mainnet. */
    assert(coinbase_network_is_mainnet("who-knows") == 0);
    assert(coinbase_network_is_mainnet(NULL) == 0);
    printf("ok: address -> network\n");
}

/* The scriptSig length varint lives in cb1, and is computed from
 * en1_size + en2_size at render time. If the extranonces spliced in later
 * are not exactly that wide, the varint disagrees with the bytes that follow
 * and the transaction is malformed -- valid-looking to a hasher, rejected by
 * the network. Pin the agreement at the width the pool actually advertises,
 * so a change to STRATUM_EXTRANONCE2_SIZE that misses a call site fails here
 * rather than on a found block. */
static void test_scriptsig_length_matches_advertised_extranonce(void) {
    const size_t en1 = STRATUM_EXTRANONCE1_SIZE;
    const size_t en2 = STRATUM_EXTRANONCE2_SIZE;

    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                "/simplepool/",
                                en1, en2, &parts, NULL, NULL,
                                err, sizeof err) == 0);

    /* Assemble the coinbase exactly as handle_submit does. */
    size_t total = parts.cb1_len + en1 + en2 + parts.cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    size_t o = 0;
    memcpy(tx + o, parts.cb1, parts.cb1_len); o += parts.cb1_len;
    memset(tx + o, 0xaa, en1);                o += en1;
    memset(tx + o, 0xbb, en2);                o += en2;
    memcpy(tx + o, parts.cb2, parts.cb2_len);

    /* Walk to the scriptSig varint: version(4) | varint(vin) | prevout(36). */
    size_t off = 4;
    uint64_t vin = 0;
    assert(read_varint(tx, total, &off, &vin) == 0 && vin == 1);
    off += 36;
    uint64_t ss_len = 0;
    assert(read_varint(tx, total, &off, &ss_len) == 0);

    /* The declared length must cover the real scriptSig contents, and the
     * extranonce bytes must be the last thing inside it. */
    assert(ss_len <= 100);
    size_t ss_end = off + (size_t)ss_len;
    assert(ss_end <= total);
    for (size_t i = 0; i < en1; i++) assert(tx[ss_end - en1 - en2 + i] == 0xaa);
    for (size_t i = 0; i < en2; i++) assert(tx[ss_end - en2 + i] == 0xbb);
    /* Sequence follows immediately -- proof the varint did not run short. */
    for (int i = 0; i < 4; i++) assert(tx[ss_end + i] == 0xff);

    free(tx);
    coinbase_parts_free(&parts);
    printf("ok: scriptSig length agrees with en1=%zu en2=%zu\n", en1, en2);
}

/* The same cb1 assembled with a wrong-width extranonce2 -- what a miner that
 * ignored mining.subscribe would produce -- must not parse as a well-formed
 * coinbase. This is the failure the stratum-side length check prevents. */
static void test_wrong_width_extranonce_desyncs_the_parse(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    assert(coinbase_build_split(800000, 5000000000LL, ENF_ADDR, ENF_ADDR, 100,
                                "6a24aa21a9ed2222222222222222222222222222"
                                "222222222222222222222222222222222222",
                                "/simplepool/",
                                STRATUM_EXTRANONCE1_SIZE,
                                STRATUM_EXTRANONCE2_SIZE, &parts, NULL, NULL,
                                err, sizeof err) == 0);

    /* Splice in the classic 4-byte extranonce2 instead of the reserved width. */
    size_t n = parts.cb1_len * 2 + (STRATUM_EXTRANONCE1_SIZE + 4) * 2
             + parts.cb2_len * 2 + 1;
    char *hex = (char *)malloc(n);
    assert(hex);
    size_t o = 0;
    for (size_t i = 0; i < parts.cb1_len; i++)
        o += (size_t)sprintf(hex + o, "%02x", parts.cb1[i]);
    for (size_t i = 0; i < STRATUM_EXTRANONCE1_SIZE + 4; i++)
        o += (size_t)sprintf(hex + o, "%02x", 0xcc);
    for (size_t i = 0; i < parts.cb2_len; i++)
        o += (size_t)sprintf(hex + o, "%02x", parts.cb2[i]);
    hex[o] = '\0';

    int spend = -1, opret = -1;
    int rc = coinbase_count_outputs(hex, &spend, &opret);
    /* Either the parse fails outright or it reports something other than the
     * real output set -- never a clean, correct read. */
    assert(!(rc == 0 && spend == 2 && opret == 1));

    free(hex);
    coinbase_parts_free(&parts);
    printf("ok: wrong-width extranonce2 does not parse as a valid coinbase\n");
}

/* Consensus caps the coinbase scriptSig at 100 bytes. Unreachable through
 * config today (height push + a 76-byte tag + 12 extranonce bytes is 93),
 * but the guard is what keeps a future widening from emitting a coinbase
 * that only fails at the network. */
static void test_scriptsig_over_100_is_rejected(void) {
    coinbase_parts_t parts = {0};
    char err[256] = {0};
    int rc = coinbase_build_split(800000, 5000000000LL, ENF_ADDR, NULL, 0,
                                  NULL, "/simplepool/",
                                  /* en1 */ 4, /* en2 */ 90,
                                  &parts, NULL, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "scriptSig length") != NULL);
    printf("ok: oversized coinbase scriptSig rejected (%s)\n", err);
}


/* ---------------------------------------------------------------------------
 * BIP-350 conformance.
 *
 * The tables below are VERIFIED AGAINST bip-0350.mediawiki. They are
 * deliberately adversarial — two differ from a neighbour only in the checksum,
 * another carries a single uppercase letter mid-string to test case rejection —
 * so a hand transcription is a test that passes for the wrong reason the moment
 * one character slips. What stands behind them is an independent differential:
 * every entry was decoded with a from-scratch BIP-173/350 reference
 * implementation and checked against what each row asserts, and that reference
 * was diffed against coinbase_address_to_script() over 1,224 generated
 * addresses (witness versions 0-16 x program lengths {2,16,20,21,31,32,33,40} x
 * {bc,tb,bcrt} x {lowercase, uppercase, bad checksum}) with zero mismatches.
 * The tables are static, so the suite has no network dependency.
 *
 * ⚠️ "VALID PER BIP-350" AND "WE WILL PAY IT" ARE DIFFERENT QUESTIONS, and the
 * split into two tables is the point. Three of the eight addresses BIP-350
 * lists as valid are ones this pool REFUSES on purpose — witness v2, v16, and a
 * v1 with a 40-byte program. They encode correctly and `validateaddress` calls
 * them valid; they are also anyone-can-spend under current consensus, so a
 * coinbase paying one hands the reward to whoever notices first. Refusing costs
 * that miner an error message at authorize. Accepting costs them a block.
 * (BIP-341: a v1 program of any length other than 32 remains unencumbered.)
 * ------------------------------------------------------------------------- */
/* Verified against bip-0350.mediawiki — see the note above. */
static const struct { const char *addr; const char *spk; } bip350_supported[] = {
    { "BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4",
      "0014751e76e8199196d454941c45d1b3a323f1433bd6" },  /* v0, 20-byte */
    { "tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7",
      "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262" },  /* v0, 32-byte */
    { "tb1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesrxh6hy",
      "0020000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433" },  /* v0, 32-byte */
    { "tb1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesf3hn0c",
      "5120000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433" },  /* v1, 32-byte */
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
      "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798" },  /* v1, 32-byte */
};

static const struct { const char *addr; int witver; size_t proglen; } bip350_refused[] = {
    { "bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y", 1, 40 },
    { "BC1SW50QGDZ25J", 16, 2 },
    { "bc1zw508d6qejxtdg4y5r3zarvaryvaxxpcs", 2, 16 },
};

static const struct { const char *addr; const char *why; } bip350_invalid[] = {
    /* NOT REACHED BY THE BECH32 PATH — no bc1/tb1/bcrt1 prefix, so it falls
       through to base58 and is rejected there. Still a rejection. */
    { "tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut",
      "Invalid human-readable part" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqh2y7hd",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "tb1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqglt7rf",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "BC1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ54WELL",
      "Invalid checksum (Bech32 instead of Bech32m)" },
    { "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kemeawh",
      "Invalid checksum (Bech32m instead of Bech32)" },
    { "tb1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq24jc47",
      "Invalid checksum (Bech32m instead of Bech32)" },
    { "bc1p38j9r5y49hruaue7wxjce0updqjuyyx0kh56v8s25huc6995vvpql3jow4",
      "Invalid character in checksum" },
    { "BC130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ7ZWS8R",
      "Invalid witness version" },
    { "bc1pw5dgrnzv",
      "Invalid program length (1 byte)" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav253zgeav",
      "Invalid program length (41 bytes)" },
    { "BC1QR508D6QEJXTDG4Y5R3ZARVARYV98GJ9P",
      "Invalid program length for witness version 0 (per BIP141)" },
    { "tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq47Zagq",
      "Mixed case" },
    { "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07qwwzcrf",
      "zero padding of more than 4 bits" },
    { "tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vpggkg4j",
      "Non-zero padding in 8-to-5 conversion" },
    { "bc1gmk9yu",
      "Empty data section" },
};

static void hex_of(const uint8_t *b, size_t n, char *out) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = H[b[i] >> 4]; out[i*2+1] = H[b[i] & 15]; }
    out[n*2] = 0;
}

static void test_bip350_supported(void) {
    for (size_t i = 0; i < sizeof bip350_supported / sizeof bip350_supported[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192], got[160];
        int rc = coinbase_address_to_script(bip350_supported[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc != 0) {
            printf("FAIL: %s rejected: %s\n", bip350_supported[i].addr, err);
            assert(rc == 0);
        }
        hex_of(spk, spk_len, got);
        if (strcmp(got, bip350_supported[i].spk) != 0) {
            printf("FAIL: %s\n  want %s\n  got  %s\n",
                   bip350_supported[i].addr, bip350_supported[i].spk, got);
            assert(0);
        }
    }
    printf("ok: bip350 supported vectors (%zu) produce the spec's exact scriptPubKey\n",
           sizeof bip350_supported / sizeof bip350_supported[0]);
}

static void test_bip350_refused_by_policy(void) {
    /* These must fail, and the message must say WHY — a miner who pastes a v2
       address needs to learn something other than "invalid". */
    for (size_t i = 0; i < sizeof bip350_refused / sizeof bip350_refused[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192];
        err[0] = 0;
        int rc = coinbase_address_to_script(bip350_refused[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc == 0) {
            printf("FAIL: %s was PAID; witness v%d/%zu-byte is anyone-can-spend\n",
                   bip350_refused[i].addr, bip350_refused[i].witver,
                   bip350_refused[i].proglen);
            assert(rc != 0);
        }
        assert(err[0] != 0);
    }
    printf("ok: bip350 well-formed-but-unsafe vectors (%zu) refused with a reason\n",
           sizeof bip350_refused / sizeof bip350_refused[0]);
}

static void test_bip350_invalid(void) {
    for (size_t i = 0; i < sizeof bip350_invalid / sizeof bip350_invalid[0]; i++) {
        uint8_t spk[64];
        size_t  spk_len = 0;
        char    err[192];
        err[0] = 0;
        int rc = coinbase_address_to_script(bip350_invalid[i].addr, spk, sizeof spk,
                                            &spk_len, err, sizeof err);
        if (rc == 0) {
            printf("FAIL: accepted invalid address %s (%s)\n",
                   bip350_invalid[i].addr, bip350_invalid[i].why);
            assert(rc != 0);
        }
        assert(err[0] != 0);
    }
    printf("ok: bip350 invalid vectors (%zu) all rejected\n",
           sizeof bip350_invalid / sizeof bip350_invalid[0]);
}

/* The regression the weight constant exists to prevent: a taproot payout costs
   43 bytes on the wire, not 31, and the headroom divider has to know it. */
static void test_payout_txout_weight_matches_p2tr(void) {
    uint8_t spk[64];
    size_t  spk_len = 0;
    char    err[192];
    int rc = coinbase_address_to_script(
        "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0",
        spk, sizeof spk, &spk_len, err, sizeof err);
    assert(rc == 0);
    /* 8-byte value + 1-byte script length + scriptPubKey, non-witness so x4. */
    size_t wu = (8 + 1 + spk_len) * 4;
    assert(spk_len == 34);
    assert(wu == 172);
    /* Recorded because it is the number any weight accounting has to use once
     * taproot payouts are possible: a P2TR payout output costs 172 WU against
     * a P2WPKH one's 124. Anything that budgets coinbase room per output at
     * the P2WPKH figure will let more outputs through than actually fit. */
    printf("ok: a P2TR payout output is %zu bytes / %zu WU\n", spk_len, wu);
}

/* ---- coinbase-direct PPLNS ---------------------------------------------
 *
 * The rail where the pool never receives the reward: the window is paid
 * straight out of the coinbase of the block it produced. No wallet, no payout
 * worker, no maturity gate — a reorged block simply never paid.
 *
 * The property these are really defending is conservation. A coinbase that
 * pays out less than it is allowed does not leave the remainder anywhere; it
 * destroys it. So every satoshi of the block has to leave in an output, and
 * anything the window cannot be paid has to be visibly carried rather than
 * quietly dropped. */

#define WA "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
#define WB "bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
#define WC "bcrt1qyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zs4w3j0"
#define WOP "bcrt1qxvenxvenxvenxvenxvenxvenxvenxvenztev8a"

/* Count outputs and sum their values out of the assembled coinbase, so the
 * assertions read the transaction rather than the builder's own report. */
static void window_outputs(const coinbase_parts_t *p, size_t en_total,
                           uint64_t *n_out, int64_t *sum_out) {
    /* cb2 = sequence(4) | varint n_outputs | outputs | locktime(4) */
    const uint8_t *b = p->cb2;
    size_t off = 4;
    uint64_t n = b[off++];          /* every case here is < 253 outputs */
    int64_t sum = 0;
    for (uint64_t i = 0; i < n; ++i) {
        int64_t v = 0;
        for (int k = 0; k < 8; ++k) v |= ((int64_t)b[off + k]) << (8 * k);
        off += 8;
        size_t spk_len = b[off++];
        off += spk_len;
        sum += v;
    }
    (void)en_total;
    *n_out = n;
    *sum_out = sum;
}

static void test_window_pays_each_miner_its_own_output(void) {
    coinbase_parts_t parts; char err[256];
    coinbase_window_result_t res;
    /* 1% of 5,000,000,000 is 50,000,000, leaving 4,950,000,000 to split. */
    const coinbase_payee_t payees[] = {
        { WA, 2475000000LL }, { WB, 1485000000LL }, { WC, 990000000LL },
    };
    int rc = coinbase_build_window(800000, 5000000000LL, payees, 3,
                                   WOP, 100, NULL, "/simplepool/", 4, 8,
                                   0, 0, &parts, &res, err, sizeof err);
    assert(rc == 0);
    assert(res.paid_count == 3);
    assert(res.fee_sats == 50000000LL);
    assert(res.forfeited_sats == 0);
    assert(res.paid_sats == 4950000000LL);

    uint64_t n = 0; int64_t sum = 0;
    window_outputs(&parts, 12, &n, &sum);
    assert(n == 4);                         /* three miners + the operator */
    assert(sum == 5000000000LL);            /* the whole block, nothing burnt */
    coinbase_parts_free(&parts);
    printf("ok: window pays each miner its own coinbase output\n");
}

/* A split that does not add up is a caller bug, and the honest response is to
 * refuse: emitting it would silently forfeit the difference to nobody. */
static void test_a_split_that_does_not_add_up_is_refused(void) {
    coinbase_parts_t parts; char err[256];
    const coinbase_payee_t short_[] = { { WA, 1000000LL } };
    int rc = coinbase_build_window(800000, 5000000000LL, short_, 1,
                                   WOP, 100, NULL, NULL, 4, 8,
                                   0, 0, &parts, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "payees sum to") != NULL);

    const coinbase_payee_t over[] = { { WA, 9000000000LL } };
    rc = coinbase_build_window(800000, 5000000000LL, over, 1,
                               WOP, 100, NULL, NULL, 4, 8,
                               0, 0, &parts, NULL, err, sizeof err);
    assert(rc < 0);
    printf("ok: a window split that does not sum to the block is refused\n");
}

/* Below the floor. The value has to go somewhere -- a coinbase paying out
 * less than it may forfeits the difference to nobody -- and the somewhere is
 * the operator output. It is income, not a debt: nothing records it and
 * nothing settles it later. This is the design's harshest edge, so it is
 * pinned rather than left implied. */
static void test_a_payee_below_the_floor_is_forfeited_to_the_operator(void) {
    coinbase_parts_t parts; char err[256];
    coinbase_window_result_t res;
    /* fee 1% of 100,000,000 = 1,000,000; payable 99,000,000. */
    const coinbase_payee_t payees[] = {
        { WA, 98999900LL },
        { WB, 100LL },              /* far below the 546-sat dust limit */
    };
    int rc = coinbase_build_window(800000, 100000000LL, payees, 2,
                                   WOP, 100, NULL, NULL, 4, 8,
                                   0, 0, &parts, &res, err, sizeof err);
    assert(rc == 0);
    assert(res.paid_count == 1);
    assert(res.dropped_below_floor == 1);
    assert(res.forfeited_sats == 100LL);
    /* Forfeits are reported apart from the fee. The operator output carries
     * both, but an operator publishing its take has to be able to say which
     * part was the advertised fee and which part was somebody's lost claim. */
    assert(res.fee_sats == 1000000LL);

    uint64_t n = 0; int64_t sum = 0;
    window_outputs(&parts, 12, &n, &sum);
    assert(n == 2);                          /* one miner + the operator */
    assert(sum == 100000000LL);              /* still the whole block */
    /* And the operator output is fee + forfeit, not just the fee. */
    assert(sum - res.paid_sats == res.fee_sats + res.forfeited_sats);
    coinbase_parts_free(&parts);
    printf("ok: a payee below the floor is forfeited to the operator\n");
}

/* The floor is configurable, and raising it forfeits claims that the dust
 * limit alone would have paid. That is the knob an operator uses to trade
 * coinbase bytes against how small a miner it is willing to serve. */
static void test_the_payout_floor_is_configurable(void) {
    coinbase_parts_t parts; char err[256];
    coinbase_window_result_t res;
    /* 10,000 sats: comfortably relayable, so only an explicit floor drops it. */
    const coinbase_payee_t payees[] = { { WA, 99990000LL }, { WB, 10000LL } };

    /* Default floor (dust): both are paid. */
    assert(coinbase_build_window(800000, 100000000LL, payees, 2,
                                 WOP, 0, NULL, NULL, 4, 8,
                                 0, 0, &parts, &res, err, sizeof err) == 0);
    assert(res.paid_count == 2);
    assert(res.forfeited_sats == 0);
    coinbase_parts_free(&parts);

    /* Floor above the small claim: it is forfeited, not carried. */
    assert(coinbase_build_window(800000, 100000000LL, payees, 2,
                                 WOP, 0, NULL, NULL, 4, 8,
                                 0, 50000, &parts, &res, err, sizeof err) == 0);
    assert(res.paid_count == 1);
    assert(res.dropped_below_floor == 1);
    assert(res.forfeited_sats == 10000LL);
    coinbase_parts_free(&parts);

    /* A floor below the dust limit is clamped up to it rather than honoured:
     * an output under 546 sats is not relayable, so there is no lower floor
     * to have and pretending otherwise would build an unspendable block. */
    const coinbase_payee_t dusty[] = { { WA, 99999900LL }, { WB, 100LL } };
    assert(coinbase_build_window(800000, 100000000LL, dusty, 2,
                                 WOP, 0, NULL, NULL, 4, 8,
                                 0, 1, &parts, &res, err, sizeof err) == 0);
    assert(res.paid_count == 1);
    assert(res.dropped_below_floor == 1);
    coinbase_parts_free(&parts);
    printf("ok: the payout floor is configurable and clamped up to dust\n");
}

/* The byte budget is about marketplaces rejecting an oversized coinbase, so
 * it has to fall on the smallest claims: paying largest-first means the
 * forfeit lands on whoever has least at stake in it. */
static void test_the_cap_falls_on_the_smallest_claims(void) {
    coinbase_parts_t parts; char err[256];
    coinbase_window_result_t res;
    const coinbase_payee_t payees[] = {
        { WA, 1000000LL }, { WB, 3000000LL }, { WC, 6000000LL },
    };
    /* fee 1% of 10,101,010 ~ 101,010; make the numbers exact instead. */
    int64_t value = 1000000LL + 3000000LL + 6000000LL;   /* fee_bps 0: no fee */
    /* The operator address is still required: capping produces a forfeit,
     * and a forfeit needs somewhere to go even when there is no fee. */
    int rc = coinbase_build_window(800000, value, payees, 3,
                                   WOP, 0, NULL, NULL, 4, 8,
                                   /* Byte budget admitting exactly two of the
                                    * three payouts: the envelope, scriptSig
                                    * and reserved operator output come to
                                    * 112 bytes, and each P2WPKH payout costs
                                    * 31, so 174 fits two and 205 would fit
                                    * three. */
                                   180, 0,
                                   &parts, &res, err, sizeof err);
    assert(rc == 0);
    assert(res.paid_count == 2);
    assert(res.dropped_capped == 1);
    /* The 1,000,000 claim is the one that loses out, not the 6,000,000 one. */
    assert(res.forfeited_sats == 1000000LL);
    assert(res.paid_sats == 9000000LL);
    coinbase_parts_free(&parts);
    printf("ok: the output cap drops the smallest claims first\n");
}

/* With no operator address there is nowhere for a forfeit to go, so a window
 * that cannot be paid in full has to be refused rather than silently burn the
 * difference into the void. */
static void test_a_forfeit_without_an_operator_address_is_refused(void) {
    coinbase_parts_t parts; char err[256];
    const coinbase_payee_t payees[] = {
        { WA, 999900LL }, { WB, 100LL },
    };
    int rc = coinbase_build_window(800000, 1000000LL, payees, 2,
                                   NULL, 0, NULL, NULL, 4, 8,
                                   0, 0, &parts, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "no operator_address to receive") != NULL);
    printf("ok: a forfeit with nowhere to go is refused, not burnt\n");
}

/* If nobody clears the floor, paying the operator the whole block and calling
 * it a fee would be the worst possible outcome -- forfeits are meant to be the
 * edge of the distribution, never the whole of it. Refuse the block instead. */
static void test_a_window_of_only_dust_is_refused(void) {
    coinbase_parts_t parts; char err[256];
    const coinbase_payee_t payees[] = { { WA, 100LL }, { WB, 100LL } };
    int rc = coinbase_build_window(800000, 200LL, payees, 2,
                                   WOP, 0, NULL, NULL, 4, 8,
                                   0, 0, &parts, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "payout floor") != NULL);
    printf("ok: a window of nothing but dust is refused\n");
}

static void test_an_empty_window_is_refused(void) {
    coinbase_parts_t parts; char err[256];
    int rc = coinbase_build_window(800000, 5000000000LL, NULL, 0,
                                   WOP, 100, NULL, NULL, 4, 8,
                                   0, 0, &parts, NULL, err, sizeof err);
    assert(rc < 0);
    assert(strstr(err, "nobody to pay") != NULL);
    printf("ok: an empty window is refused\n");
}

/* The witness commitment has to survive byte-for-byte and stay last, exactly
 * as the other builders keep it -- a block whose commitment moved or changed
 * is invalid. */
static void test_the_witness_commitment_is_preserved(void) {
    coinbase_parts_t parts; char err[256];
    coinbase_window_result_t res;
    const char *wc = "6a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf9";
    const coinbase_payee_t payees[] = { { WA, 5000000000LL } };
    int rc = coinbase_build_window(800000, 5000000000LL, payees, 1,
                                   NULL, 0, wc, NULL, 4, 8,
                                   0, 0, &parts, &res, err, sizeof err);
    assert(rc == 0);
    uint64_t n = 0; int64_t sum = 0;
    window_outputs(&parts, 12, &n, &sum);
    assert(n == 2);                 /* the miner, then the commitment */
    assert(sum == 5000000000LL);    /* the commitment output carries 0 */
    coinbase_parts_free(&parts);
    printf("ok: the witness commitment is preserved and stays last\n");
}

/* Same window, same bytes, twice. A miner checking the block it was paid from
 * has to get the same answer as the pool did. */
static void test_the_coinbase_is_deterministic(void) {
    coinbase_parts_t a, b; char err[256];
    const coinbase_payee_t payees[] = {
        { WA, 1000000LL }, { WB, 1000000LL }, { WC, 3000000LL },
    };
    int64_t value = 5000000LL;
    assert(coinbase_build_window(800000, value, payees, 3, NULL, 0, NULL,
                                 "/sp/", 4, 8, 0, 0, &a, NULL, err, sizeof err) == 0);
    assert(coinbase_build_window(800000, value, payees, 3, NULL, 0, NULL,
                                 "/sp/", 4, 8, 0, 0, &b, NULL, err, sizeof err) == 0);
    assert(a.cb2_len == b.cb2_len);
    assert(memcmp(a.cb2, b.cb2, a.cb2_len) == 0);
    coinbase_parts_free(&a);
    coinbase_parts_free(&b);
    printf("ok: the same window builds the same coinbase twice\n");
}

/* Assemble cb1 + extranonce + cb2 and tally the outputs, the same way the
 * single-payee template test walks the bytes. Reads the transaction rather
 * than trusting the builder's own report. */
static void parts_outputs(const coinbase_parts_t *parts, size_t en_total,
                          int *spendable, int *op_returns, int64_t *sum) {
    size_t total = parts->cb1_len + en_total + parts->cb2_len;
    uint8_t *tx = (uint8_t *)malloc(total);
    assert(tx);
    memcpy(tx, parts->cb1, parts->cb1_len);
    memset(tx + parts->cb1_len, 0xaa, en_total);
    memcpy(tx + parts->cb1_len + en_total, parts->cb2, parts->cb2_len);

    size_t off = 4;                      /* version */
    uint64_t n = 0;
    assert(read_varint(tx, total, &off, &n) == 0 && n == 1);
    off += 32 + 4;                       /* prevout */
    uint64_t ss = 0;
    assert(read_varint(tx, total, &off, &ss) == 0);
    off += ss + 4;                       /* scriptSig + sequence */
    uint64_t outs = 0;
    assert(read_varint(tx, total, &off, &outs) == 0);

    *spendable = 0; *op_returns = 0; *sum = 0;
    for (uint64_t i = 0; i < outs; ++i) {
        int64_t v = 0;
        for (int k = 0; k < 8; ++k) v |= ((int64_t)tx[off + k]) << (8 * k);
        off += 8;
        uint64_t spk_len = 0;
        assert(read_varint(tx, total, &off, &spk_len) == 0);
        if (spk_len > 0 && tx[off] == 0x6a) (*op_returns)++;
        else { (*spendable)++; *sum += v; }
        off += spk_len;
    }
    free(tx);
}

/* The drivechain path: a window paid straight out of an enforcer-served
 * coinbase, with the BIP300/301 commitments preserved around it.
 *
 * This is the one a real pool needs. Every simplepool deployment mines on a
 * template the enforcer builds, so a rail that only works against plain
 * bitcoind is a rail that does not work. */
static void test_window_from_template_preserves_commitments(void) {
    coinbase_parts_t parts; char err[256] = {0};
    coinbase_window_result_t res;
    int has_witness = -1;

    /* What the template pays, learned from the single-payee builder so the
     * split below is exact without hardcoding the fixture's reward. */
    coinbase_parts_t probe; int64_t reward = 0, unused = 0;
    assert(coinbase_build_from_template(ENF_COINBASE_HEX, ENF_ADDR, NULL, 0,
                                        NULL, 4, 4, &probe, NULL, &reward,
                                        &unused, err, sizeof err) == 0);
    coinbase_parts_free(&probe);
    assert(reward > 0);

    /* Two miners, 60/40, no operator fee so the arithmetic is exact. */
    int64_t a = (reward * 6) / 10;
    const coinbase_payee_t payees[] = { { WA, a }, { WB, reward - a } };
    int rc = coinbase_build_window_from_template(
        ENF_COINBASE_HEX, payees, 2, NULL, 0, "/x/", 4, 4, 0, 0,
        &parts, &has_witness, &res, err, sizeof err);
    if (rc != 0) fprintf(stderr, "window_from_template err: %s\n", err);
    assert(rc == 0);
    assert(res.paid_count == 2);
    assert(res.forfeited_sats == 0);
    assert(res.paid_sats == reward);

    /* The enforcer's own outputs must survive: one spendable output was
     * replaced by two, and every OP_RETURN it carried is still there. */
    int base_spendable = 0, base_op_returns = 0;
    assert(coinbase_count_outputs(ENF_COINBASE_HEX, &base_spendable,
                                  &base_op_returns) == 0);
    assert(base_spendable == 1);

    int spendable = 0, op_returns = 0;
    int64_t sum = 0;
    parts_outputs(&parts, 8, &spendable, &op_returns, &sum);
    assert(spendable == 2);                 /* one output became two miners */
    assert(op_returns == base_op_returns);  /* every commitment survived */
    assert(sum == reward);                  /* and the whole reward left */
    coinbase_parts_free(&parts);
    printf("ok: window from template pays N miners and keeps the commitments\n");
}

/* coinbase_template_reward() has one job: hand a caller the number the
 * builders will insist the payees sum to. So it is asserted against the
 * builder, not against a constant — a constant would still be "right" on the
 * day the two stopped agreeing, which is the only day it matters.
 *
 * If they ever diverge, main.c divides one number and the builder checks
 * against another, so every job is refused on every connection and the pool
 * stops publishing work with nothing but a repeated warning. */
static void test_the_template_reward_matches_what_the_builder_splits(void) {
    char err[256] = {0};
    coinbase_parts_t probe; int64_t builder_reward = 0, unused = 0;
    assert(coinbase_build_from_template(ENF_COINBASE_HEX, ENF_ADDR, NULL, 0,
                                        NULL, 4, 4, &probe, NULL,
                                        &builder_reward, &unused,
                                        err, sizeof err) == 0);
    coinbase_parts_free(&probe);
    assert(builder_reward > 0);

    int64_t reward = 0;
    assert(coinbase_template_reward(ENF_COINBASE_HEX, &reward) == 0);
    assert(reward == builder_reward);

    /* And the number is usable: a window split against it is accepted, which
     * is the whole point of asking. */
    coinbase_parts_t parts;
    coinbase_window_result_t res;
    const coinbase_payee_t payees[] = { { WA, reward / 2 },
                                        { WB, reward - reward / 2 } };
    assert(coinbase_build_window_from_template(ENF_COINBASE_HEX, payees, 2,
                                               NULL, 0, NULL, 4, 4, 0, 0,
                                               &parts, NULL, &res,
                                               err, sizeof err) == 0);
    assert(res.paid_sats == reward);
    coinbase_parts_free(&parts);

    /* Garbage in, refusal out — never a plausible-looking zero, which would
     * make main.c divide nothing across the window and pay everyone dust. */
    assert(coinbase_template_reward("not hex", &reward) < 0);
    assert(coinbase_template_reward(NULL, &reward) < 0);
    assert(coinbase_template_reward(ENF_COINBASE_HEX, NULL) < 0);
    printf("ok: the template reward is exactly what the builder splits\n");
}

/* The two builders must divide a window identically. They share a resolver
 * precisely so that a drivechain pool and a plain-bitcoind pool cannot pay
 * the same miners different amounts. */
static void test_both_window_builders_split_identically(void) {
    char err[256] = {0};
    coinbase_parts_t p1, p2;
    coinbase_window_result_t r1, r2;

    coinbase_parts_t probe; int64_t reward = 0, unused = 0;
    assert(coinbase_build_from_template(ENF_COINBASE_HEX, ENF_ADDR, NULL, 0,
                                        NULL, 4, 4, &probe, NULL, &reward,
                                        &unused, err, sizeof err) == 0);
    coinbase_parts_free(&probe);

    /* Three claims, one below the floor, so forfeiting is exercised too.
     * They must sum to the payable amount, i.e. net of the 1% fee. */
    int64_t fee = (reward * 100) / 10000;
    int64_t payable = reward - fee;
    const coinbase_payee_t payees[] = {
        { WA, payable - 40000 - 100 }, { WB, 40000 }, { WC, 100 },
    };
    assert(coinbase_build_window(800000, reward, payees, 3, WOP, 100, NULL,
                                 "/x/", 4, 4, 0, 0, &p1, &r1, err, sizeof err) == 0);
    assert(coinbase_build_window_from_template(ENF_COINBASE_HEX, payees, 3,
                                               WOP, 100, "/x/", 4, 4, 0, 0,
                                               &p2, NULL, &r2, err, sizeof err) == 0);
    assert(r1.paid_count   == r2.paid_count);
    assert(r1.paid_sats    == r2.paid_sats);
    assert(r1.fee_sats     == r2.fee_sats);
    assert(r1.forfeited_sats     == r2.forfeited_sats);
    assert(r1.dropped_below_floor == r2.dropped_below_floor);
    assert(r1.dropped_below_floor == 1);
    assert(r1.forfeited_sats     >= 100);
    coinbase_parts_free(&p1);
    coinbase_parts_free(&p2);
    printf("ok: both window builders split a window identically\n");
}

/* The budget is BYTES, and the commitments are part of what spends them.
 *
 * This is the correction that production evidence forced. The first version
 * capped payouts at a count, which cannot express the thing that actually
 * binds: a coinbase-direct pool reports the same 16 payouts costing 817 bytes
 * against four drivechain OP_RETURNs and 769 against three
 * (LayerTwo-Labs/simplepool#61). The commitments are not payouts and a count
 * cap cannot see them; a byte budget spends them first and pays whoever is
 * left over.
 *
 * Asserted as a relationship rather than against somebody else's absolute
 * numbers: the same window, the same budget, a template carrying more
 * commitment bytes -> strictly fewer miners paid. */
static void test_commitments_eat_the_payout_budget(void) {
    char err[256] = {0};
    coinbase_parts_t parts;
    coinbase_window_result_t res;

    coinbase_parts_t probe; int64_t reward = 0, unused = 0;
    assert(coinbase_build_from_template(ENF_COINBASE_HEX, ENF_ADDR, NULL, 0,
                                        NULL, 4, 4, &probe, NULL, &reward,
                                        &unused, err, sizeof err) == 0);
    coinbase_parts_free(&probe);

    /* Eight equal claims, all comfortably above dust. */
    enum { N = 8 };
    coinbase_payee_t payees[N];
    int64_t each = reward / N;
    for (int i = 0; i < N; ++i) {
        payees[i].address = (i % 2) ? WA : WB;
        payees[i].sats = each;
    }
    payees[0].sats += reward - each * N;    /* exact */

    /* Generous enough to admit several of the eight, tight enough that the
     * commitments make a visible difference. Measured: at this budget the
     * enforcer template admits 5 and a bare coinbase admits 6. */
    const size_t BUDGET = 300;
    assert(coinbase_build_window_from_template(ENF_COINBASE_HEX, payees, N,
                                               WOP, 0, NULL, 4, 4, BUDGET, 0,
                                               &parts, NULL, &res,
                                               err, sizeof err) == 0);
    size_t paid_with_template = res.paid_count;
    assert(paid_with_template > 0 && paid_with_template < N);
    /* The block is still fully spent: whatever did not fit was forfeited to
     * the operator rather than left unpaid in the coinbase. */
    assert(res.dropped_capped == N - paid_with_template);
    assert(res.paid_sats + res.forfeited_sats + res.fee_sats == reward);
    coinbase_parts_free(&parts);

    /* The same window and the same budget, built from scratch — no template,
     * so no commitment OP_RETURNs spending the budget. More miners fit. */
    assert(coinbase_build_window(800000, reward, payees, N, WOP, 0, NULL,
                                 NULL, 4, 4, BUDGET, 0, &parts, &res,
                                 err, sizeof err) == 0);
    assert(res.paid_count > paid_with_template);
    coinbase_parts_free(&parts);
    printf("ok: commitments spend the byte budget, so fewer miners fit (%zu vs %zu)\n",
           paid_with_template, res.paid_count);
}

/* Whatever the budget says, the coinbase must actually come in under it —
 * the number is only worth having if it is true of the bytes on the wire. */
static void test_the_built_coinbase_respects_its_budget(void) {
    char err[256] = {0};
    coinbase_parts_t parts;
    coinbase_window_result_t res;

    enum { N = 12 };
    coinbase_payee_t payees[N];
    int64_t total = 5000000000LL;
    int64_t each = total / N;
    for (int i = 0; i < N; ++i) {
        payees[i].address = (i % 2) ? WA : WB;
        payees[i].sats = each;
    }
    payees[0].sats += total - each * N;

    for (size_t budget = 200; budget <= 600; budget += 100) {
        assert(coinbase_build_window(800000, total, payees, N, WOP, 0, NULL,
                                     "/sp/", 4, 8, budget, 0, &parts, &res,
                                     err, sizeof err) == 0);
        /* cb1 + extranonce + cb2 is the whole serialized coinbase. */
        size_t built = parts.cb1_len + 12 + parts.cb2_len;
        if (built > budget) {
            fprintf(stderr, "FAIL: budget %zu produced %zu bytes\n", budget, built);
            assert(0);
        }
        coinbase_parts_free(&parts);
    }
    printf("ok: a built coinbase never exceeds its byte budget\n");
}

int main(void) {
    test_p2pkh_address();
    test_the_built_coinbase_respects_its_budget();
    test_commitments_eat_the_payout_budget();
    test_the_template_reward_matches_what_the_builder_splits();
    test_both_window_builders_split_identically();
    test_window_from_template_preserves_commitments();
    test_window_pays_each_miner_its_own_output();
    test_a_split_that_does_not_add_up_is_refused();
    test_a_payee_below_the_floor_is_forfeited_to_the_operator();
    test_the_payout_floor_is_configurable();
    test_the_cap_falls_on_the_smallest_claims();
    test_a_forfeit_without_an_operator_address_is_refused();
    test_a_window_of_only_dust_is_refused();
    test_an_empty_window_is_refused();
    test_the_witness_commitment_is_preserved();
    test_the_coinbase_is_deterministic();
    test_p2wpkh_address();
    test_regtest_p2wpkh();
    test_build_coinbase_structural();
    test_build_coinbase_split_fee_math();
    test_bip34_small_height_uses_opn();
    test_build_from_template();
    test_build_from_template_fee_split();
    test_count_outputs();
    test_address_network();
    test_scriptsig_length_matches_advertised_extranonce();
    test_wrong_width_extranonce_desyncs_the_parse();
    test_scriptsig_over_100_is_rejected();
    test_bip350_supported();
    test_bip350_refused_by_policy();
    test_bip350_invalid();
    test_payout_txout_weight_matches_p2tr();
    printf("test_coinbase: all tests passed\n");
    return 0;
}
