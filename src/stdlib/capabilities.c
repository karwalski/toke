/*
 * capabilities.c — deny-by-default capability broker (ADR-0010, Epic 124.4a).
 * See capabilities.h for the model. This is the runtime half: broker state,
 * the tk_cap_check gate, and the runtime --allow-* flag parse. Default mode is
 * ALLOW_ALL so behaviour is unchanged until the sinks are wired (124.4c) and
 * the default is flipped (124.4g).
 */
#include "capabilities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Weak baked-grant defaults: absent, no grants, not enforcing. The compiler
 * (124.4b) emits strong overrides when a program declares grants. */
__attribute__((weak)) const uint32_t __tk_cap_baked_grants  = 0u;
__attribute__((weak)) const int      __tk_cap_baked_present = 0;
__attribute__((weak)) const int      __tk_cap_baked_enforce = 0;

/* ── Broker state ─────────────────────────────────────────────────── */
/* 124.4g: deny-by-default. The mode with no baked grants and no runtime flags is
 * ENFORCE — a program has no fs/net/env/process authority unless it is granted.
 * Only an explicit --allow-all (baked or runtime) opts back into ALLOW_ALL. */
static TkCapMode g_mode   = TK_CAP_MODE_ENFORCE;
static uint32_t  g_grants = 0u;
static int       g_inited = 0;

/*
 * Map a single --allow-* flag (without value) to its capability bit(s).
 * Path/host scoping (e.g. --allow-read=/srv/www) is a later refinement
 * (124.4c); here the value, if any, is ignored and the whole class is granted.
 * Returns 0 for an unrecognised flag.
 */
static uint32_t flag_to_caps(const char *arg) {
    /* accept both --allow-read and --allow-fs-read spellings */
    if (!strncmp(arg, "--allow-all", 11))          return TK_CAP_ALL;
    if (!strncmp(arg, "--allow-read", 12) ||
        !strncmp(arg, "--allow-fs-read", 15))       return TK_CAP_FS_READ;
    if (!strncmp(arg, "--allow-write", 13) ||
        !strncmp(arg, "--allow-fs-write", 16))      return TK_CAP_FS_WRITE;
    if (!strncmp(arg, "--allow-net", 11))           return TK_CAP_NET;
    if (!strncmp(arg, "--allow-env", 11))           return TK_CAP_ENV_WRITE;
    if (!strncmp(arg, "--allow-run", 11))           return TK_CAP_PROCESS_SPAWN;
    return 0u;
}

void tk_cap_init(int argc, char **argv) {
    if (g_inited) return;
    g_inited = 1;

    /* Start from the compiler-baked grant set (if the program declared one).
     * Post-flip the mode is ENFORCE by default (set statically above), so a
     * baked allowlist simply enforces those grants. The only opt-out is an
     * explicit --allow-all (all bits) — baked or at run time. The legacy
     * __tk_cap_baked_enforce flag is now vestigial (enforcement is the default). */
    if (__tk_cap_baked_present) {
        g_grants = __tk_cap_baked_grants;
        if (__tk_cap_baked_grants == TK_CAP_ALL)
            g_mode = TK_CAP_MODE_ALLOW_ALL;
    }

    /* Union in runtime --allow-* flags. --allow-all drops to permissive mode
     * (the explicit escape hatch); any specific grant keeps deny-by-default for
     * every other class. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--allow-", 8) != 0) continue;
        uint32_t caps = flag_to_caps(a);
        if (caps == 0u) continue;
        g_grants |= caps;
        if (caps == TK_CAP_ALL)
            g_mode = TK_CAP_MODE_ALLOW_ALL;
    }
}

int tk_cap_is_grant_flag(const char *arg) {
    return arg && strncmp(arg, "--allow-", 8) == 0 && flag_to_caps(arg) != 0u;
}

int tk_cap_check(uint32_t cap) {
    if (g_mode == TK_CAP_MODE_ALLOW_ALL) return 1;
    return (g_grants & cap) == cap;
}

const char *tk_cap_class_name(uint32_t cap) {
    switch (cap) {
        case TK_CAP_FS_READ:       return "fs.read";
        case TK_CAP_FS_WRITE:      return "fs.write";
        case TK_CAP_NET:           return "net";
        case TK_CAP_ENV_WRITE:     return "env.write";
        case TK_CAP_PROCESS_SPAWN: return "process.spawn";
        default:                   return "capability";
    }
}

/* The --allow-* flag that grants a given class (for the diagnostic hint). */
static const char *cap_flag_hint(uint32_t cap) {
    switch (cap) {
        case TK_CAP_FS_READ:       return "--allow-read";
        case TK_CAP_FS_WRITE:      return "--allow-write";
        case TK_CAP_NET:           return "--allow-net";
        case TK_CAP_ENV_WRITE:     return "--allow-env";
        case TK_CAP_PROCESS_SPAWN: return "--allow-run";
        default:                   return "--allow-all";
    }
}

void tk_cap_deny(uint32_t cap) {
    const char *cls = tk_cap_class_name(cap);
    const char *flag = cap_flag_hint(cap);
    fprintf(stderr,
        "CAP001: capability '%s' required but not granted\n"
        "  grant it at run time with %s, or declare it in tkc.toml [capabilities]\n",
        cls, flag);
    /* 124.4d: opt-in machine-parseable line so the generate-compile-repair loop
     * can repair the missing grant mechanically (like a type error). */
    if (getenv("TK_DIAG_JSON")) {
        fprintf(stderr,
            "{\"diagnostic_id\":\"CAP001\",\"severity\":\"error\",\"stage\":\"runtime\","
            "\"capability\":\"%s\",\"fix\":\"grant %s or tkc.toml [capabilities] %s=true\"}\n",
            cls, flag, cls);
    }
    exit(1);
}

TkCapMode tk_cap_mode(void)  { return g_mode; }
uint32_t  tk_cap_grants(void) { return g_grants; }
