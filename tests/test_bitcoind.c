#include "../src/bitcoind.h"
#include "../src/cjson/cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static const char *SAMPLE_GBT =
"{"
"  \"version\": 536870912,"
"  \"previousblockhash\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\","
"  \"transactions\": ["
"    {"
"      \"data\": \"0100000001abcd\","
"      \"txid\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"      \"hash\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"      \"fee\": 12345,"
"      \"sigops\": 4,"
"      \"weight\": 800"
"    },"
"    {"
"      \"data\": \"0200000002deadbeef\","
"      \"txid\": \"1111111111111111111111111111111111111111111111111111111111111111\","
"      \"fee\": 600,"
"      \"weight\": 400"
"    }"
"  ],"
"  \"coinbaseaux\": {},"
"  \"coinbasevalue\": 5000012945,"
"  \"target\": \"0000000000000000000a000000000000000000000000000000000000000000ff\","
"  \"mintime\": 1700000000,"
"  \"mutable\": [\"time\",\"transactions\",\"prevblock\"],"
"  \"noncerange\": \"00000000ffffffff\","
"  \"sigoplimit\": 80000,"
"  \"sizelimit\": 4000000,"
"  \"weightlimit\": 4000000,"
"  \"curtime\": 1700001234,"
"  \"bits\": \"170abc12\","
"  \"height\": 800123,"
"  \"default_witness_commitment\": \"6a24aa21a9ed0000000000000000000000000000000000000000000000000000000000000000\","
"  \"longpollid\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\""
"}";

static void test_parse_ok(void) {
    cJSON *root = cJSON_Parse(SAMPLE_GBT);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->height == 800123);
        CHECK(strcmp(t->prev_hash_hex, "0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567") == 0);
        CHECK(t->coinbase_value_sats == 5000012945LL);
        CHECK(strcmp(t->target_hex, "0000000000000000000a000000000000000000000000000000000000000000ff") == 0);
        CHECK(t->bits == 0x170abc12u);
        CHECK(t->curtime == 1700001234u);
        CHECK(t->version == 536870912);
        CHECK(t->min_time == 1700000000LL);
        CHECK(t->default_witness_commitment != NULL);
        CHECK(t->default_witness_commitment != NULL &&
              strncmp(t->default_witness_commitment, "6a24aa21a9ed", 12) == 0);
        /* BIP22 long-poll token is optional but must round-trip when sent. */
        CHECK(t->longpollid != NULL);
        CHECK(t->longpollid != NULL &&
              strcmp(t->longpollid,
                     "0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567") == 0);
        CHECK(t->tx_count == 2);
        CHECK(t->txs != NULL);
        if (t->tx_count == 2 && t->txs) {
            CHECK(strcmp(t->txs[0].data_hex, "0100000001abcd") == 0);
            CHECK(strcmp(t->txs[0].txid_hex, "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899") == 0);
            CHECK(t->txs[0].fee == 12345);
            CHECK(t->txs[0].weight == 800);
            CHECK(strcmp(t->txs[1].data_hex, "0200000002deadbeef") == 0);
            CHECK(t->txs[1].fee == 600);
            CHECK(t->txs[1].weight == 400);
        }
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

/* Backends like the CUSF enforcer return a server-built coinbase under
 * "coinbasetxn" (no "coinbasevalue"). The coinbase's total output value is
 * reported as a negative "fee"; the parser stores the raw tx hex and recovers
 * the value by negating that fee. */
static const char *SAMPLE_GBT_COINBASETXN =
"{"
"  \"version\": 536870912,"
"  \"previousblockhash\": \"0000000000000000000a1b2c3d4e5f6789abcdef0123456789abcdef01234567\","
"  \"transactions\": [],"
"  \"coinbaseaux\": {},"
"  \"coinbasetxn\": {"
"    \"data\": \"02000000010000000000000000000000000000000000000000000000000000000000000000ffffffff04025c0bffffffff0100f2052a01000000160014000000000000000000000000000000000000000000000000\","
"    \"txid\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"    \"hash\": \"aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899\","
"    \"depends\": [],"
"    \"fee\": -5000000000,"
"    \"sigops\": 0,"
"    \"weight\": 400"
"  },"
"  \"target\": \"0000000000000000000a000000000000000000000000000000000000000000ff\","
"  \"mintime\": 1700000000,"
"  \"noncerange\": \"00000000ffffffff\","
"  \"curtime\": 1700001234,"
"  \"bits\": \"170abc12\","
"  \"height\": 800123,"
"  \"default_witness_commitment\": \"6a24aa21a9ed0000000000000000000000000000000000000000000000000000000000000000\""
"}";

static void test_parse_coinbasetxn(void) {
    cJSON *root = cJSON_Parse(SAMPLE_GBT_COINBASETXN);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->height == 800123);
        /* value derived by negating the coinbasetxn "fee" */
        CHECK(t->coinbase_value_sats == 5000000000LL);
        CHECK(t->coinbasetxn_hex != NULL);
        CHECK(t->coinbasetxn_hex != NULL &&
              strncmp(t->coinbasetxn_hex, "02000000", 8) == 0);
        CHECK(t->bits == 0x170abc12u);
        /* No longpollid in this fixture: the server doesn't long poll. */
        CHECK(t->longpollid == NULL);
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

static void test_parse_missing_prevhash(void) {
    const char *bad =
        "{ \"version\":1, \"coinbasevalue\":100, \"target\":\"00\","
        "  \"bits\":\"1d00ffff\", \"curtime\":1, \"height\":1, \"transactions\":[] }";
    cJSON *root = cJSON_Parse(bad);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc < 0);
    CHECK(t == NULL);
    CHECK(strlen(err) > 0);
    cJSON_Delete(root);
}

static void test_parse_empty_txs(void) {
    const char *src =
        "{ \"version\":1,"
        "  \"previousblockhash\":\"00000000000000000000000000000000aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "  \"coinbasevalue\":50,"
        "  \"target\":\"ffff000000000000000000000000000000000000000000000000000000000000\","
        "  \"bits\":\"1d00ffff\","
        "  \"curtime\":42,"
        "  \"height\":1,"
        "  \"mintime\":40,"
        "  \"transactions\":[] }";
    cJSON *root = cJSON_Parse(src);
    assert(root);
    bitcoind_template_t *t = NULL;
    char err[256] = {0};
    int rc = bitcoind_parse_template(root, &t, err, sizeof(err));
    CHECK(rc == 0);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->tx_count == 0);
        CHECK(t->txs == NULL);
        CHECK(t->bits == 0x1d00ffffu);
        CHECK(t->default_witness_commitment == NULL);
    }
    bitcoind_template_free(t);
    cJSON_Delete(root);
}

static void test_double_free_safe(void) {
    /* template_free(NULL) must be safe */
    bitcoind_template_free(NULL);
    CHECK(1);
}

static void test_client_init_validates(void) {
    bitcoind_cfg_t cfg = {0};
    bitcoind_client_t c;
    int rc = bitcoind_client_init(&c, &cfg);
    CHECK(rc < 0); /* empty url */

    snprintf(cfg.url, sizeof(cfg.url), "http://127.0.0.1:18443/");
    snprintf(cfg.user, sizeof(cfg.user), "u");
    snprintf(cfg.pass, sizeof(cfg.pass), "p");
    rc = bitcoind_client_init(&c, &cfg);
    CHECK(rc == 0);
    CHECK(c._curl != NULL);
    CHECK(c._lock != NULL);
    bitcoind_client_free(&c);
    CHECK(c._curl == NULL);
    CHECK(c._lock == NULL);
}

int main(void) {
    test_parse_ok();
    test_parse_coinbasetxn();
    test_parse_missing_prevhash();
    test_parse_empty_txs();
    test_double_free_safe();
    test_client_init_validates();

    printf("test_bitcoind: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("all assertions passed\n");
        return 0;
    }
    return 1;
}
