/* Unit tests for thunder_address_decode. */

#include "../src/thunder.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* Helper: encode 20 bytes into the base58 textual form used by Thunder.
 * We only need this for round-trip tests; keep it self-contained. */
static const char b58_alpha[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static size_t b58_encode_20(const uint8_t in[20], char *out, size_t cap) {
    /* Big-int divide-by-58. */
    uint8_t buf[20];
    memcpy(buf, in, 20);
    char tmp[64];
    size_t tl = 0;
    int empty;
    do {
        unsigned rem = 0;
        empty = 1;
        for (int i = 0; i < 20; i++) {
            unsigned v = (rem << 8) | buf[i];
            buf[i] = (uint8_t)(v / 58);
            rem = v % 58;
            if (buf[i]) empty = 0;
        }
        tmp[tl++] = b58_alpha[rem];
    } while (!empty);
    /* Leading zero bytes -> leading '1's. */
    for (int i = 0; i < 20 && in[i] == 0; i++) tmp[tl++] = '1';
    if (tl + 1 > cap) return 0;
    for (size_t i = 0; i < tl; i++) out[i] = tmp[tl - 1 - i];
    out[tl] = '\0';
    return tl;
}

static void test_roundtrip_bare(void) {
    /* 20-byte all-zero address. */
    uint8_t in[20] = {0};
    in[19] = 0xab;
    char addr[64];
    size_t n = b58_encode_20(in, addr, sizeof addr);
    CHECK(n > 0);
    uint8_t got[20];
    char err[128];
    int rc = thunder_address_decode(addr, got, err, sizeof err);
    if (rc != 0) fprintf(stderr, "decode err: %s\n", err);
    CHECK(rc == 0);
    CHECK(memcmp(got, in, 20) == 0);
}

static void test_roundtrip_random_pattern(void) {
    uint8_t in[20];
    for (int i = 0; i < 20; i++) in[i] = (uint8_t)(0x10 * i + 7);
    char addr[64];
    CHECK(b58_encode_20(in, addr, sizeof addr) > 0);
    uint8_t got[20];
    char err[128];
    CHECK(thunder_address_decode(addr, got, err, sizeof err) == 0);
    CHECK(memcmp(got, in, 20) == 0);
}

static void test_deposit_format(void) {
    /* s9_<base58>_<checksum hex> — the wrapper format. */
    uint8_t in[20];
    for (int i = 0; i < 20; i++) in[i] = (uint8_t)(i * 11 + 1);
    char b58[64];
    CHECK(b58_encode_20(in, b58, sizeof b58) > 0);

    char wrapped[128];
    snprintf(wrapped, sizeof wrapped, "s9_%s_a1b2c3", b58);

    uint8_t got[20];
    char err[128];
    int rc = thunder_address_decode(wrapped, got, err, sizeof err);
    if (rc != 0) fprintf(stderr, "deposit decode err: %s\n", err);
    CHECK(rc == 0);
    CHECK(memcmp(got, in, 20) == 0);
}

static void test_rejects_garbage(void) {
    uint8_t got[20];
    /* Non-base58 chars. */
    CHECK(thunder_address_decode("not-a-real-address!!!", got, NULL, 0) < 0);
    /* Too short. */
    CHECK(thunder_address_decode("abc", got, NULL, 0) < 0);
    /* Empty. */
    CHECK(thunder_address_decode("", got, NULL, 0) < 0);
}

int main(void) {
    test_roundtrip_bare();
    test_roundtrip_random_pattern();
    test_deposit_format();
    test_rejects_garbage();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_thunder: ok\n");
    return 0;
}
