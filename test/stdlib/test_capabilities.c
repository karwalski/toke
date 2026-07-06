/*
 * test_capabilities.c — unit test for the capability broker (Epic 124.4a).
 *
 * The broker's default (ALLOW_ALL, no behaviour change) is covered by the
 * differential-codegen gate. This test exercises the enforce path that no
 * toke program reaches yet: runtime --allow-* parsing, the tk_cap_check gate,
 * mode selection, and the class-name mapping.
 *
 * tk_cap_init is idempotent (one init per process), so this drives a single
 * enforce scenario. Compile:
 *   cc -std=c99 -D_GNU_SOURCE -iquote src/stdlib \
 *      test/stdlib/test_capabilities.c src/stdlib/capabilities.c -o /tmp/tc
 */
#include "capabilities.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define ASSERT(cond, msg) do { \
    if (cond) printf("pass: %s\n", (msg)); \
    else { printf("FAIL: %s\n", (msg)); failures++; } } while (0)

int main(void) {
    /* Class-name mapping (pure). */
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_FS_READ), "fs.read") == 0,       "name: fs.read");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_FS_WRITE), "fs.write") == 0,     "name: fs.write");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_NET), "net") == 0,               "name: net");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_ENV_WRITE), "env.write") == 0,   "name: env.write");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_PROCESS_SPAWN), "process.spawn") == 0, "name: process.spawn");

    /* 124.4g: deny-by-default — the broker enforces before any grant is seen. */
    ASSERT(tk_cap_mode() == TK_CAP_MODE_ENFORCE, "default mode is ENFORCE (deny-by-default)");
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0, "default: fs.write denied before any grant");
    ASSERT(tk_cap_check(TK_CAP_NET) == 0,      "default: net denied before any grant");

    /* A specific grant keeps deny-by-default and grants only the named class;
     * scoped values (=host / =path) are accepted and coarsened to the class. */
    char *argv[] = { "prog", "--allow-net=api.example.com", "--allow-read", "input.txt", NULL };
    tk_cap_init(4, argv);

    ASSERT(tk_cap_mode() == TK_CAP_MODE_ENFORCE, "mode: still ENFORCE with a specific grant");
    ASSERT(tk_cap_check(TK_CAP_NET) == 1,          "granted: net passes");
    ASSERT(tk_cap_check(TK_CAP_FS_READ) == 1,      "granted: fs.read passes");
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0,     "denied: fs.write blocked");
    ASSERT(tk_cap_check(TK_CAP_ENV_WRITE) == 0,    "denied: env.write blocked");
    ASSERT(tk_cap_check(TK_CAP_PROCESS_SPAWN) == 0,"denied: process.spawn blocked");
    ASSERT((tk_cap_grants() & TK_CAP_NET) && (tk_cap_grants() & TK_CAP_FS_READ),
           "grants: net + fs.read bits set");

    /* Idempotent: a second init does not change state. */
    char *argv2[] = { "prog", "--allow-write", NULL };
    tk_cap_init(2, argv2);
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0, "idempotent: re-init ignored");

    if (failures == 0) { printf("All capability checks passed.\n"); return 0; }
    fprintf(stderr, "%d check(s) failed.\n", failures);
    return 1;
}
