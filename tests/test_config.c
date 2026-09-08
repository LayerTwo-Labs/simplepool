/* Config parsing. Focused on the comment/quoting rules, because getting them
 * wrong is silent: the pool boots, then fails to authenticate to bitcoind with
 * nothing in the log that points at the config file. */

#include "../src/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static const char *VALID_ADDR = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";

/* Write `body` to a temp file and load it. Returns the load's return code. */
static int load_text(const char *body, proxy_config_t *cfg,
                     char *err, size_t errlen) {
    char path[] = "/tmp/simplepool_test_conf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL: mkstemp\n"); failures++; return -99; }
    FILE *f = fdopen(fd, "w");
    fputs(body, f);
    fclose(f);
    int rc = proxy_config_load(path, cfg, err, errlen);
    unlink(path);
    return rc;
}

/* A '#' inside a value is data, not a comment introducer.
 *
 * The old parser cut the line at the FIRST '#' anywhere, before the key/value
 * split and before unquoting — so this password silently became "p", and
 * quoting did not help either. */
static void test_hash_inside_value_is_kept(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "bitcoind_pass = p#ssw0rd\n"
             "bitcoind_user = a#b#c\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(cfg.bitcoind_pass, "p#ssw0rd") == 0);
    CHECK(strcmp(cfg.bitcoind_user, "a#b#c") == 0);
}

/* Quoted values keep everything inside the quotes, including whitespace-led
 * '#' that would otherwise start a comment. */
static void test_quoted_value_keeps_hash(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "coinbase_tag = \"tag #7 rules\"\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(cfg.coinbase_tag, "tag #7 rules") == 0);
}

/* Inline comments still work — proxy.conf.example documents them, so this is
 * the behaviour the fix had to preserve. */
static void test_inline_comment_still_strips(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "# a whole-line comment\n"
             "operator_address = %s\n"
             "listen_port = 3333   # which port to bind\n"
             "coinbase_tag = plain # trailing\n",
             VALID_ADDR);
    int rc = load_text(body, &cfg, err, sizeof err);
    CHECK(rc == 0);
    CHECK(cfg.listen_port == 3333);
    CHECK(strcmp(cfg.coinbase_tag, "plain") == 0);
}

/* An operator_address that is not a decodable Bitcoin address must not reach
 * the mining path — every coinbase render would fail at the first share. */
static void test_rejects_bad_operator_address(void) {
    proxy_config_t cfg; char err[256] = {0};
    int rc = load_text("operator_address = not-an-address\n",
                       &cfg, err, sizeof err);
    /* config_load itself accepts the string; main.c is where it is decoded.
     * What must hold here is that the value arrives INTACT, so that check can
     * do its job rather than validating a truncated string. */
    CHECK(rc == 0);
    CHECK(strcmp(cfg.operator_address, "not-an-address") == 0);
}


/* ---- pool_mode validation ------------------------------------------------
 *
 * These are the messages an operator actually meets, and until now the only
 * thing that checked them was a person running the binary by hand. Every
 * string asserted here is also quoted in INSTALL.md's troubleshooting section,
 * so a reworded error that leaves the docs behind fails here first. */

/* Bare `pplns` is refused ahead of the generic catch-all, because it is the
 * likely typo: the operator knows which accounting they want and has not
 * noticed that the rail is part of the name. */
static void test_pplns_without_a_rail_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "pool_mode = pplns\n"
             "pool_btc_address = %s\n", VALID_ADDR, VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "does not say which rail pays") != NULL);
    CHECK(strstr(err, "pplns-thunder") != NULL);
    CHECK(strstr(err, "pplns-btc") != NULL);
}

static void test_unknown_mode_names_the_real_ones(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplnsx\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "'solo'") != NULL);
    CHECK(strstr(err, "'pps-classic'") != NULL);
    CHECK(strstr(err, "'pplns-thunder'") != NULL);
    CHECK(strstr(err, "'pplns-btc'") != NULL);
}

/* Both pplns rails pool the reward, so both need somewhere to pay it. */
static void test_pplns_requires_a_pool_address(void) {
    for (const char *mode = "pplns-thunder";; mode = "pplns-btc") {
        proxy_config_t cfg; char err[256] = {0};
        char body[512];
        snprintf(body, sizeof body,
                 "operator_address = %s\npool_mode = %s\n", VALID_ADDR, mode);
        CHECK(load_text(body, &cfg, err, sizeof err) != 0);
        CHECK(strstr(err, "pool_btc_address") != NULL);
        CHECK(strstr(err, mode) != NULL);
        if (strcmp(mode, "pplns-btc") == 0) break;
    }
}

/* The two flags pool_mode used to conflate. pplns-btc is the combination no
 * single "is this PPS" flag could express: the coinbase pays the pool, and the
 * username is a Bitcoin address. */
static void test_each_mode_sets_its_two_independent_flags(void) {
    static const struct { const char *mode; int pays_pool, thunder, accrues; } CASES[] = {
        { "solo",          0, 0, 0 },
        { "pps-classic",   1, 1, 1 },
        { "pplns-thunder", 1, 1, 0 },
        { "pplns-btc",     1, 0, 0 },
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
        proxy_config_t cfg; char err[256] = {0};
        char body[512];
        snprintf(body, sizeof body,
                 "operator_address = %s\npool_mode = %s\npool_btc_address = %s\n",
                 VALID_ADDR, CASES[i].mode, VALID_ADDR);
        CHECK(load_text(body, &cfg, err, sizeof err) == 0);
        CHECK(strcmp(cfg.pool_mode, CASES[i].mode) == 0);
    }
}

/* A window of zero or less would divide by nothing; the proxy refuses rather
 * than distributing a block across a window that does not exist. */
static void test_a_non_positive_pplns_window_is_refused(void) {
    const char *bad[] = { "0", "-1", "-0.5" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        proxy_config_t cfg; char err[256] = {0};
        char body[512];
        snprintf(body, sizeof body,
                 "operator_address = %s\npool_mode = pplns-btc\n"
                 "pool_btc_address = %s\npplns_window_diff_multiple = %s\n",
                 VALID_ADDR, VALID_ADDR, bad[i]);
        CHECK(load_text(body, &cfg, err, sizeof err) != 0);
        CHECK(strstr(err, "pplns_window_diff_multiple") != NULL);
        CHECK(strstr(err, "> 0") != NULL);
    }
}

/* Below 1.0 a block pays out across less work than it took to find, which
 * rewards hopping. That is a choice an operator is allowed to make badly, so
 * it warns and loads rather than refusing. */
static void test_a_small_pplns_window_warns_but_loads(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-btc\n"
             "pool_btc_address = %s\npplns_window_diff_multiple = 0.5\n",
             VALID_ADDR, VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.pplns_window_diff_multiple == 0.5);
}

static void test_the_window_defaults_to_two(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-btc\npool_btc_address = %s\n",
             VALID_ADDR, VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.pplns_window_diff_multiple == 2.0);
}

/* pplns-coinbase pays the window out of the block's own coinbase, so the pool
 * never receives the reward. */
static void test_pplns_coinbase_is_accepted_without_a_pool_wallet(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(strcmp(cfg.pool_mode, "pplns-coinbase") == 0);
    CHECK(cfg.pplns_window_diff_multiple == 2.0);
}

/* A configured pool wallet is refused rather than ignored. The whole claim of
 * this mode is that the pool never holds the reward, and a pool_btc_address is
 * the shape of a pool that does — most likely a mode switched in place without
 * the rest of the config following. Running a custodial-looking pool that
 * quietly is not one is worse than refusing to start. */
static void test_pplns_coinbase_refuses_a_pool_wallet(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "pool_btc_address = %s\n", VALID_ADDR, VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "must not be set") != NULL);
    CHECK(strstr(err, "pays miners directly from the coinbase") != NULL);
}

/* It is a pplns mode, so the window knob applies to it too. */
static void test_pplns_coinbase_validates_the_window(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "pplns_window_diff_multiple = 0\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "pplns_window_diff_multiple") != NULL);
}

/* The byte budget is what limits how many miners a block can pay, so a value
 * too small to hold even one payout is a pool that cannot run at all. */
static void test_a_tiny_coinbase_budget_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "coinbase_max_bytes = 150\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "coinbase_max_bytes") != NULL);
}

static void test_the_coinbase_budget_defaults_and_parses(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.coinbase_max_bytes == 1000);

    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "coinbase_max_bytes = 820\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.coinbase_max_bytes == 820);
}

/* The payout floor decides who this pool refuses to serve, so it has to parse
 * exactly and default to something an operator can defend. It is the harshest
 * knob in the file: above it a miner is paid out of the block, below it a
 * miner mines here and earns nothing. */
static void test_the_payout_floor_defaults_and_parses(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.pplns_payout_floor_sats == 546);   /* the dust limit */

    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "pplns_payout_floor_sats = 25000\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.pplns_payout_floor_sats == 25000);

    /* Zero is legitimate: it means "pay anything the dust limit allows", and
     * coinbase.c clamps it up. Only a negative is a typo. */
    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "pplns_payout_floor_sats = 0\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.pplns_payout_floor_sats == 0);

    snprintf(body, sizeof body,
             "operator_address = %s\npool_mode = pplns-coinbase\n"
             "pplns_payout_floor_sats = -1\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "pplns_payout_floor_sats") != NULL);
}

/* ---- listener lines ------------------------------------------------------
 *
 * A `listener` line is how rented hashrate is served its own difficulty. A
 * marketplace measures what the port advertises and cancels an order that
 * comes in under it, so a line that parses wrongly is a delisting, not a
 * cosmetic problem. None of this was covered. */

static void test_a_listener_line_becomes_a_port_policy(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listener = port=3335 min_diff=65536 label=rental-a\n",
             VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.listener_count == 1);
    CHECK(cfg.listeners[0].port == 3335);
    CHECK(cfg.listeners[0].min_diff == 65536.0);
    CHECK(strcmp(cfg.listeners[0].label, "rental-a") == 0);
}

static void test_several_listeners_keep_their_own_policies(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\n"
             "listener = port=3335 min_diff=1000 label=a\n"
             "listener = port=3336 min_diff=2000 label=b\n",
             VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.listener_count == 2);
    CHECK(cfg.listeners[0].port == 3335 && cfg.listeners[0].min_diff == 1000.0);
    CHECK(cfg.listeners[1].port == 3336 && cfg.listeners[1].min_diff == 2000.0);
    CHECK(strcmp(cfg.listeners[1].label, "b") == 0);
}

static void test_a_listener_field_that_is_not_key_value_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\nlistener = port=3335 nonsense\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
    CHECK(strstr(err, "nonsense") != NULL);
}

/* A listener with no port is not a listener. */
static void test_a_listener_without_a_port_is_refused(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\nlistener = min_diff=1000\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) != 0);
}

/* ---- log level ----------------------------------------------------------- */

static void test_log_level_accepts_names_and_numbers(void) {
    static const struct { const char *v; int want; } CASES[] = {
        { "debug", 0 }, { "info", 1 }, { "warn", 2 }, { "error", 3 },
        { "DEBUG", 0 }, { "0", 0 }, { "3", 3 },
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; ++i) {
        proxy_config_t cfg; char err[256] = {0};
        char body[512];
        snprintf(body, sizeof body,
                 "operator_address = %s\nlog_level = %s\n", VALID_ADDR, CASES[i].v);
        CHECK(load_text(body, &cfg, err, sizeof err) == 0);
        CHECK(cfg.log_level == CASES[i].want);
    }
}

/* An unparseable log level warns and keeps the default rather than refusing.
 * Deliberate, and worth pinning so nobody "fixes" it into a hard error: a
 * typo in a cosmetic setting should not stop a pool accepting work, and the
 * warning is on the same line as everything else the operator is reading. */
static void test_a_nonsense_log_level_warns_and_keeps_the_default(void) {
    proxy_config_t cfg; char err[256] = {0};
    char body[512];
    snprintf(body, sizeof body,
             "operator_address = %s\nlog_level = chatty\n", VALID_ADDR);
    CHECK(load_text(body, &cfg, err, sizeof err) == 0);
    CHECK(cfg.log_level == 1);   /* info, the default */
}

int main(void) {
    printf("running test_config...\n");
    test_hash_inside_value_is_kept();
    test_quoted_value_keeps_hash();
    test_inline_comment_still_strips();
    test_rejects_bad_operator_address();
    test_the_coinbase_budget_defaults_and_parses();
    test_the_payout_floor_defaults_and_parses();
    test_a_tiny_coinbase_budget_is_refused();
    test_pplns_coinbase_validates_the_window();
    test_pplns_coinbase_refuses_a_pool_wallet();
    test_pplns_coinbase_is_accepted_without_a_pool_wallet();
    test_a_nonsense_log_level_warns_and_keeps_the_default();
    test_log_level_accepts_names_and_numbers();
    test_a_listener_without_a_port_is_refused();
    test_a_listener_field_that_is_not_key_value_is_refused();
    test_several_listeners_keep_their_own_policies();
    test_a_listener_line_becomes_a_port_policy();
    test_the_window_defaults_to_two();
    test_a_small_pplns_window_warns_but_loads();
    test_a_non_positive_pplns_window_is_refused();
    test_each_mode_sets_its_two_independent_flags();
    test_pplns_requires_a_pool_address();
    test_unknown_mode_names_the_real_ones();
    test_pplns_without_a_rail_is_refused();
    if (failures) { printf("test_config: %d failed\n", failures); return 1; }
    printf("test_config: all tests passed\n");
    return 0;
}
