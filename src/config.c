#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

void proxy_config_defaults(proxy_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->listen_addr, sizeof cfg->listen_addr, "%s", "0.0.0.0");
    cfg->listen_port  = 3334;
    cfg->max_conns    = 500;
    cfg->initial_diff = 1.0;

    snprintf(cfg->bitcoind_url,  sizeof cfg->bitcoind_url,  "%s", "http://127.0.0.1:18443");
    snprintf(cfg->bitcoind_user, sizeof cfg->bitcoind_user, "%s", "drivepool");
    snprintf(cfg->bitcoind_pass, sizeof cfg->bitcoind_pass, "%s", "drivepool");
    cfg->bitcoind_poll_interval_ms = 30000;

    cfg->operator_address[0] = '\0';
    cfg->fee_bps = 100;  /* 1% */
    snprintf(cfg->coinbase_tag, sizeof cfg->coinbase_tag, "%s", "/simplepool/");

    snprintf(cfg->db_path, sizeof cfg->db_path, "%s", "./data/shares.db");
    cfg->commit_window_ms  = 100;
    cfg->commit_max_shares = 100;

    cfg->log_level = 1; /* info */

    cfg->vardiff_enabled    = 1;
    cfg->vardiff_target_spm = 12.0;   /* ~1 share every 5s per connection */
    cfg->vardiff_min        = 1.0;
    cfg->vardiff_max        = 1e12;
    cfg->vardiff_window_sec = 30;
}

static char *strtrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = '\0'; e--; }
    return s;
}

static void unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int parse_log_level(const char *v) {
    if (strcasecmp(v, "debug") == 0) return 0;
    if (strcasecmp(v, "info")  == 0) return 1;
    if (strcasecmp(v, "warn")  == 0 || strcasecmp(v, "warning") == 0) return 2;
    if (strcasecmp(v, "error") == 0) return 3;
    /* Numeric. */
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end && *end == '\0' && n >= 0 && n <= 3) return (int)n;
    return -1;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src);
}

int proxy_config_load(const char *path, proxy_config_t *cfg,
                      char *errbuf, size_t errlen) {
    proxy_config_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(errbuf, errlen, "cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[2048];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        /* Strip comment. */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char *trimmed = strtrim(line);
        if (*trimmed == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) {
            LOG_WARN("config: line %d: no '=' separator, skipping", lineno);
            continue;
        }
        *eq = '\0';
        char *k = strtrim(trimmed);
        char *v = strtrim(eq + 1);
        unquote(v);

        if      (strcmp(k, "listen_addr")               == 0) copy_str(cfg->listen_addr, sizeof cfg->listen_addr, v);
        else if (strcmp(k, "listen_port")               == 0) cfg->listen_port = atoi(v);
        else if (strcmp(k, "max_conns")                 == 0) cfg->max_conns = atoi(v);
        else if (strcmp(k, "initial_diff")              == 0) cfg->initial_diff = atof(v);
        else if (strcmp(k, "bitcoind_url")              == 0) copy_str(cfg->bitcoind_url, sizeof cfg->bitcoind_url, v);
        else if (strcmp(k, "bitcoind_user")             == 0) copy_str(cfg->bitcoind_user, sizeof cfg->bitcoind_user, v);
        else if (strcmp(k, "bitcoind_pass")             == 0) copy_str(cfg->bitcoind_pass, sizeof cfg->bitcoind_pass, v);
        else if (strcmp(k, "bitcoind_poll_interval_ms") == 0) cfg->bitcoind_poll_interval_ms = atoi(v);
        else if (strcmp(k, "operator_address")          == 0) copy_str(cfg->operator_address, sizeof cfg->operator_address, v);
        else if (strcmp(k, "fee_bps")                   == 0) cfg->fee_bps = atoi(v);
        else if (strcmp(k, "payout_address")            == 0) {
            set_err(errbuf, errlen,
                    "config: 'payout_address' is no longer supported; "
                    "rename to 'operator_address' (recipient of the "
                    "fee_bps cut; miners are paid directly via the "
                    "stratum username address)");
            fclose(f);
            return -3;
        }
        else if (strcmp(k, "coinbase_tag")              == 0) copy_str(cfg->coinbase_tag, sizeof cfg->coinbase_tag, v);
        else if (strcmp(k, "vardiff_enabled")           == 0) cfg->vardiff_enabled = atoi(v);
        else if (strcmp(k, "vardiff_target_spm")        == 0) cfg->vardiff_target_spm = atof(v);
        else if (strcmp(k, "vardiff_min")               == 0) cfg->vardiff_min = atof(v);
        else if (strcmp(k, "vardiff_max")               == 0) cfg->vardiff_max = atof(v);
        else if (strcmp(k, "vardiff_window_sec")        == 0) cfg->vardiff_window_sec = atoi(v);
        else if (strcmp(k, "db_path")                   == 0) copy_str(cfg->db_path, sizeof cfg->db_path, v);
        else if (strcmp(k, "commit_window_ms")          == 0) cfg->commit_window_ms = atoi(v);
        else if (strcmp(k, "commit_max_shares")         == 0) cfg->commit_max_shares = atoi(v);
        else if (strcmp(k, "log_level")                 == 0) {
            int lv = parse_log_level(v);
            if (lv < 0) {
                LOG_WARN("config: line %d: unknown log_level '%s'", lineno, v);
            } else {
                cfg->log_level = lv;
            }
        }
        else {
            LOG_WARN("config: line %d: unknown key '%s'", lineno, k);
        }
    }

    fclose(f);

    if (cfg->operator_address[0] == '\0') {
        set_err(errbuf, errlen, "config: 'operator_address' is required");
        return -2;
    }
    if (cfg->fee_bps < 0 || cfg->fee_bps > 1000) {
        set_err(errbuf, errlen,
                "config: 'fee_bps' must be in [0, 1000] (0%% to 10%%), got %d",
                cfg->fee_bps);
        return -4;
    }
    return 0;
}
