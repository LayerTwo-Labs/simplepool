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
    # _POSIX_C_SOURCE (below) is what makes clock_gettime and friends visible
    # on glibc, but on Darwin the same macro works in reverse: asking for a
    # strict POSIX namespace *hides* everything BSD, and INADDR_LOOPBACK and
    # MSG_DONTWAIT are BSD, not POSIX. Without this the test suites do not
    # compile on macOS at all. _DARWIN_C_SOURCE puts them back; it is a no-op
    # anywhere else because this block is Darwin-only.
    PLATFORM_CFLAGS  := -D_DARWIN_C_SOURCE \
                        -I$(BREW_PREFIX)/include \
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

# --- Build provenance --------------------------------------------------------
# Baked into the binary so `simplepool --version` states which commit is
# actually running. Reading the checkout at runtime instead would answer a
# different question: a tree gets patched or moves on past the last `make`,
# and from then on its HEAD is not what the running process was built from.
# Empty outside a git checkout (release tarball) — reported as "unknown".
VERSION    := 0.3.0
GIT_COMMIT := $(shell git rev-parse HEAD 2>/dev/null)
GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
GIT_DIRTY  := $(shell git status --porcelain --untracked-files=no 2>/dev/null | head -1)
VERSION_H  := $(BUILD_DIR)/version_gen.h

# Sources compiled in this wave. More modules land in later waves.
SRCS := src/main.c src/log.c src/config.c src/coinbase.c src/pplns.c \
        src/share.c src/sha256.c src/stratum.c src/store.c \
        src/bitcoind.c src/broadcast.c src/thunder.c src/version.c \
        src/reconcile.c src/cjson/cJSON.c
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test asan coverage format install help FORCE

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# version.o is the one object whose correctness depends on git state rather
# than on any file it includes, so no ordinary prerequisite can invalidate it:
# commit a change and it would keep reporting the previous hash.
#
# The git state therefore goes through a generated header, rewritten only when
# its content actually differs. Recompiling version.o unconditionally would
# work too, but it would relink the binary on every single `make` — so `make`
# would never be a no-op, every invocation would produce a new mtime, and any
# deploy that restarts on "the binary changed" would restart forever. Writing
# to a temp file and moving it only on a real difference keeps `make` idempotent
# while still catching a new commit.
$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@printf '#define SIMPLEPOOL_VERSION "%s"\n#define SIMPLEPOOL_GIT_COMMIT "%s"\n#define SIMPLEPOOL_GIT_BRANCH "%s"\n#define SIMPLEPOOL_GIT_DIRTY %s\n' \
		'$(VERSION)' '$(GIT_COMMIT)' '$(GIT_BRANCH)' '$(if $(GIT_DIRTY),1,0)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD_DIR)/src/version.o: src/version.c $(VERSION_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include $(VERSION_H) -MMD -MP -c $< -o $@

FORCE:

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
include tests/test_config.mk
include tests/test_reconcile.mk
include tests/test_pplns.mk
include tests/test_store_walk.mk

test: build/test_share build/test_bitcoind build/test_stratum build/test_store build/test_store_walk build/test_coinbase build/test_broadcast build/test_thunder build/test_config build/test_reconcile build/test_pplns
	./build/test_share
	./build/test_bitcoind
	./build/test_stratum
	./build/test_store
	./build/test_store_walk
	./build/test_coinbase
	./build/test_broadcast
	./build/test_thunder
	./build/test_config
	./build/test_reconcile
	./build/test_pplns

# Run the suites under AddressSanitizer + UndefinedBehaviorSanitizer.
#
# Worth its own target: the jobs the stratum server hands to submit handlers
# are shared across threads and freed by the tip watcher, and a lifetime bug
# there is invisible to a normal test run — it corrupts a field rather than
# crashing. One reached production as blocks_found rows carrying a freed job's
# height and reward. Plain `make test` will not catch the next one; this will.
#
# Not the default build: ASan costs roughly 2x runtime and a lot of memory.
ASAN_CFLAGS := -std=c11 -g -O1 -fsanitize=address,undefined \
               -fno-omit-frame-pointer -D_POSIX_C_SOURCE=200809L \
               -Iinclude -Isrc -Isrc/cjson $(PLATFORM_CFLAGS)
ASAN_DIR := build/asan

asan:
	@mkdir -p $(ASAN_DIR)
	$(CC) $(ASAN_CFLAGS) -o $(ASAN_DIR)/test_stratum tests/test_stratum.c \
		src/stratum.c src/coinbase.c src/share.c src/sha256.c src/thunder.c \
		src/log.c src/cjson/cJSON.c -lpthread
	$(CC) $(ASAN_CFLAGS) -o $(ASAN_DIR)/test_store tests/test_store.c \
		src/store.c src/log.c $(PLATFORM_LDFLAGS) -lsqlite3 -lpthread
	$(CC) $(ASAN_CFLAGS) -Wno-unused-function -o $(ASAN_DIR)/test_store_walk \
		tests/test_store_walk.c src/log.c $(PLATFORM_LDFLAGS) -lsqlite3 -lpthread
	$(CC) $(ASAN_CFLAGS) -o $(ASAN_DIR)/test_coinbase tests/test_coinbase.c \
		src/coinbase.c src/sha256.c
	$(CC) $(ASAN_CFLAGS) -o $(ASAN_DIR)/test_share tests/test_share.c \
		src/share.c src/sha256.c
	$(CC) $(ASAN_CFLAGS) -o $(ASAN_DIR)/test_pplns tests/test_pplns.c \
		src/pplns.c src/coinbase.c src/sha256.c
	./$(ASAN_DIR)/test_stratum
	./$(ASAN_DIR)/test_store
	./$(ASAN_DIR)/test_store_walk
	./$(ASAN_DIR)/test_coinbase
	./$(ASAN_DIR)/test_share
	./$(ASAN_DIR)/test_pplns

# Line and function coverage of the C suites, via LLVM source-based coverage.
#
# What it measures is the UNIT suites only. The three regtest e2e scripts drive
# the real binary and cover a great deal that never shows up here -- the tip
# watcher, the reconcile pass, the distributor, every RPC path -- so a low
# number for a file like main.c means "not covered by `make test`", not
# "untested". Reading it the other way round is how a coverage number starts
# doing harm.
#
# Vendored cJSON is excluded: it is upstream code, and including it would move
# the headline number without saying anything about this project's tests.
COV_DIR    := build/cov
COV_CFLAGS := -std=c11 -g -O0 -fprofile-instr-generate -fcoverage-mapping \
              -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc -Isrc/cjson $(PLATFORM_CFLAGS)
# Vendored cJSON, the test files themselves, and every system / Homebrew header
# the suites pull in. Without the last of these the totals are dominated by
# hiredis and curl inlines this project never calls.
COV_IGNORE := --ignore-filename-regex='(tests/|src/cjson/|^/usr/|/opt/|/Applications/|/Library/)'

coverage:
	@command -v xcrun >/dev/null 2>&1 || { echo "coverage needs llvm-profdata/llvm-cov"; exit 1; }
	@mkdir -p $(COV_DIR)
	@rm -f $(COV_DIR)/*.profraw $(COV_DIR)/*.profdata
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_stratum tests/test_stratum.c \
		src/stratum.c src/coinbase.c src/share.c src/sha256.c src/thunder.c \
		src/log.c src/cjson/cJSON.c -lpthread
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_store tests/test_store.c \
		src/store.c src/log.c $(PLATFORM_LDFLAGS) -lsqlite3 -lpthread
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_coinbase tests/test_coinbase.c \
		src/coinbase.c src/sha256.c
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_share tests/test_share.c \
		src/share.c src/sha256.c
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_bitcoind tests/test_bitcoind.c \
		src/bitcoind.c src/log.c src/cjson/cJSON.c $(PLATFORM_LDFLAGS) -lcurl -lpthread
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_broadcast tests/test_broadcast.c \
		src/broadcast.c src/log.c $(PLATFORM_LDFLAGS) -lhiredis -lpthread
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_thunder tests/test_thunder.c src/thunder.c
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_config tests/test_config.c \
		src/config.c src/log.c src/coinbase.c src/sha256.c
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_pplns tests/test_pplns.c \
		src/pplns.c src/coinbase.c src/sha256.c
	$(CC) $(COV_CFLAGS) -o $(COV_DIR)/test_reconcile tests/test_reconcile.c \
		src/reconcile.c src/store.c src/log.c $(PLATFORM_LDFLAGS) -lsqlite3 -lpthread
	@set -e; for t in stratum store coinbase share bitcoind broadcast thunder config reconcile; do \
		LLVM_PROFILE_FILE=$(COV_DIR)/$$t.profraw ./$(COV_DIR)/test_$$t >/dev/null 2>&1 \
			|| { echo "coverage: test_$$t FAILED"; exit 1; }; \
	done
	@xcrun llvm-profdata merge -sparse $(COV_DIR)/*.profraw -o $(COV_DIR)/all.profdata
	@echo
	@xcrun llvm-cov report $(COV_DIR)/test_stratum \
		$(addprefix -object ,$(COV_DIR)/test_store $(COV_DIR)/test_coinbase \
		$(COV_DIR)/test_share $(COV_DIR)/test_bitcoind $(COV_DIR)/test_broadcast \
		$(COV_DIR)/test_thunder $(COV_DIR)/test_config $(COV_DIR)/test_reconcile \
		$(COV_DIR)/test_pplns) \
		-instr-profile=$(COV_DIR)/all.profdata $(COV_IGNORE)

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
	@echo "Targets: all clean test asan coverage format install"
	@echo "  PREFIX=$(PREFIX)  CC=$(CC)  UNAME_S=$(UNAME_S)"
