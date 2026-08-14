#ifndef SIMPLEPOOL_VERSION_H
#define SIMPLEPOOL_VERSION_H

/* Build provenance, baked in at compile time by the Makefile.
 *
 * The commit lives inside the binary rather than being read from the checkout
 * at runtime because those two answers drift: a tree gets rebased, patched by
 * hand, or simply moves on past the last `make`, and from then on `git log`
 * in the source directory describes something that is not what is running.
 * The only durable answer is the one the running process carries itself.
 *
 * Built outside a git checkout (a release tarball), the accessors return
 * "unknown" rather than a plausible-looking lie. */

const char *version_string(void); /* "0.1.0"                                */
const char *version_commit(void); /* 40-hex, or "unknown"                   */
const char *version_branch(void); /* branch at build time, or "unknown"     */
int version_dirty(void); /* 1 if tracked files were modified at build time  */

/* "simplepool 0.1.0 (e4bc649 main)" — one line, for the startup log and the
 * dashboard's provenance report. Static storage; not thread-safe to call
 * concurrently with itself, which is fine for the two places that do. */
const char *version_line(void);

/* Multi-line --version output on stdout, in the shape the other daemons in
 * this stack use (identity first, `commit:` on its own line) so one parser
 * reads all of them. */
void version_print(void);

#endif /* SIMPLEPOOL_VERSION_H */
