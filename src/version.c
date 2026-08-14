#include "version.h"

#include <stdio.h>

/* Defaults for a build that isn't going through the Makefile (an IDE, a
 * hand-run cc). Never silently substitute a guess for the commit. */
#ifndef SIMPLEPOOL_VERSION
#define SIMPLEPOOL_VERSION "0.0.0-dev"
#endif
#ifndef SIMPLEPOOL_GIT_COMMIT
#define SIMPLEPOOL_GIT_COMMIT ""
#endif
#ifndef SIMPLEPOOL_GIT_BRANCH
#define SIMPLEPOOL_GIT_BRANCH ""
#endif
#ifndef SIMPLEPOOL_GIT_DIRTY
#define SIMPLEPOOL_GIT_DIRTY 0
#endif

static const char *or_unknown(const char *s) {
    return (s && s[0]) ? s : "unknown";
}

const char *version_string(void) { return SIMPLEPOOL_VERSION; }
const char *version_commit(void) { return or_unknown(SIMPLEPOOL_GIT_COMMIT); }
const char *version_branch(void) { return or_unknown(SIMPLEPOOL_GIT_BRANCH); }
int version_dirty(void) { return SIMPLEPOOL_GIT_DIRTY; }

const char *version_line(void) {
    static char buf[192];
    const char *commit = SIMPLEPOOL_GIT_COMMIT;
    if (!commit[0]) {
        snprintf(buf, sizeof buf, "simplepool %s (commit unknown)",
                 SIMPLEPOOL_VERSION);
    } else {
        /* Short hash only: this goes in every startup log line and the full
         * 40 is available from --version when someone actually needs it. */
        snprintf(buf, sizeof buf, "simplepool %s (%.7s %s%s)",
                 SIMPLEPOOL_VERSION, commit, or_unknown(SIMPLEPOOL_GIT_BRANCH),
                 SIMPLEPOOL_GIT_DIRTY ? ", dirty" : "");
    }
    return buf;
}

void version_print(void) {
    printf("simplepool %s\n", SIMPLEPOOL_VERSION);
    printf(" commit: %s\n", version_commit());
    printf(" branch: %s\n", version_branch());
    /* Stated on its own line, and only when true, because "built from a tree
     * with uncommitted changes" means the commit above does not describe this
     * binary — an auditor comparing hashes has to be told that. */
    if (SIMPLEPOOL_GIT_DIRTY) printf(" tree:   dirty (uncommitted changes at build time)\n");
}
