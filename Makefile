# simplepool Makefile
# Pure C11. Deps: sqlite3, libcurl, pthread. cJSON is vendored under src/cjson/.

CC      ?= cc
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

UNAME_S := $(shell uname -s)

# --- Platform-specific include / lib paths -----------------------------------
# macOS: prefer Homebrew's prefix; fall back to /opt/homebrew (Apple Silicon)
# and /usr/local (Intel). Linux: rely on system paths.
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifeq ($(BREW_PREFIX),)
        ifneq ($(wildcard /opt/homebrew),)
            BREW_PREFIX := /opt/homebrew
        else
            BREW_PREFIX := /usr/local
        endif
    endif
    PLATFORM_CFLAGS  := -I$(BREW_PREFIX)/include \
                        -I$(BREW_PREFIX)/opt/sqlite/include \
                        -I$(BREW_PREFIX)/opt/curl/include \
                        -I$(BREW_PREFIX)/opt/hiredis/include
    PLATFORM_LDFLAGS := -L$(BREW_PREFIX)/lib \
                        -L$(BREW_PREFIX)/opt/sqlite/lib \
                        -L$(BREW_PREFIX)/opt/curl/lib \
                        -L$(BREW_PREFIX)/opt/hiredis/lib
else
    PLATFORM_CFLAGS  :=
    PLATFORM_LDFLAGS :=
endif

# --- Flags -------------------------------------------------------------------
WARNFLAGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wstrict-prototypes
HARDEN    := -fstack-protector-strong -D_FORTIFY_SOURCE=2
# Strict -std=c11 hides POSIX functions (clock_gettime, localtime_r, ...) behind
# feature-test macros; request POSIX.1-2008 so they're declared on glibc.
POSIX     := -D_POSIX_C_SOURCE=200809L

CFLAGS  ?= -std=c11 $(WARNFLAGS) -O2 -g $(HARDEN) $(POSIX) \
           -Iinclude -Isrc -Isrc/cjson $(PLATFORM_CFLAGS)
LDFLAGS ?= $(PLATFORM_LDFLAGS)
LDLIBS  ?= -lsqlite3 -lcurl -lhiredis -lpthread

BUILD_DIR := build
BIN       := $(BUILD_DIR)/simplepool

# Sources compiled in this wave. More modules land in later waves.
SRCS := src/main.c src/log.c src/config.c src/coinbase.c \
        src/share.c src/sha256.c src/stratum.c src/store.c \
        src/bitcoind.c src/broadcast.c src/thunder.c src/cjson/cJSON.c
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test format install help

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

include tests/test_share.mk
include tests/test_bitcoind.mk
include tests/test_stratum.mk
include tests/test_store.mk
include tests/test_coinbase.mk
include tests/test_broadcast.mk
include tests/test_thunder.mk

test: build/test_share build/test_bitcoind build/test_stratum build/test_store build/test_coinbase build/test_broadcast build/test_thunder
	./build/test_share
	./build/test_bitcoind
	./build/test_stratum
	./build/test_store
	./build/test_coinbase
	./build/test_broadcast
	./build/test_thunder

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		find src include tests -type f \( -name '*.c' -o -name '*.h' \) \
			-not -path 'src/cjson/*' \
			-print0 | xargs -0 clang-format -i ; \
		echo "formatted"; \
	else \
		echo "clang-format not installed; skipping"; \
	fi

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/simplepool

help:
	@echo "Targets: all clean test format install"
	@echo "  PREFIX=$(PREFIX)  CC=$(CC)  UNAME_S=$(UNAME_S)"
