/*
 * capabilities.h — deny-by-default capability broker (ADR-0010, Epic 124.4).
 *
 * toke is AOT-compiled to native code with no language-level sandbox: any
 * program that reaches a std fs/net/env/process sink runs with the full
 * ambient authority of the invoking user (audit finding AMB-01). ADR-0010
 * (ratified Option C) closes this with a deny-by-default capability model:
 * a program has no fs/net/env-write/process-spawn authority unless granted.
 *
 * Grants thread compiler->runtime by the owner-confirmed mechanism (a):
 *   - the compiler bakes a required grant set into the binary (weak globals
 *     __tk_cap_baked_* below, overridden by strong defs the compiler emits),
 *   - the program also accepts runtime --allow-* flags (Deno-style),
 *   - a broker (tk_cap_check) gates every sink; effective grants are the
 *     union of the baked set and the runtime flags.
 *
 * This file is 124.4a: the broker + grant model + runtime flag parse. The
 * default mode is ALLOW_ALL, so nothing is enforced yet and behaviour is
 * unchanged — the sink wiring (124.4c) and the flip to deny-by-default
 * (124.4g) come later. The compile-time baking + tkc.toml [capabilities]
 * parse is 124.4b.
 */
#ifndef TK_CAPABILITIES_H
#define TK_CAPABILITIES_H

#include <stdint.h>

/* Capability classes — one bit each. The five ADR-0010 classes. */
#define TK_CAP_FS_READ        (1u << 0)
#define TK_CAP_FS_WRITE       (1u << 1)
#define TK_CAP_NET            (1u << 2)
#define TK_CAP_ENV_WRITE      (1u << 3)
#define TK_CAP_PROCESS_SPAWN  (1u << 4)
#define TK_CAP_ALL            0xFFFFFFFFu

/* Broker enforcement mode. */
typedef enum {
    TK_CAP_MODE_ALLOW_ALL = 0,  /* default today: ambient, nothing gated */
    TK_CAP_MODE_ENFORCE   = 1   /* deny-by-default: only granted classes pass */
} TkCapMode;

/*
 * tk_cap_init — initialise the broker from the compiled program's argv.
 * Reads the compiler-baked grant set (weak globals below) and unions in any
 * runtime --allow-* flags. Idempotent; called once from tk_runtime_init.
 */
void tk_cap_init(int argc, char **argv);

/*
 * tk_cap_check — is capability `cap` (one of the TK_CAP_* bits) granted?
 * Returns 1 if allowed, 0 if denied. Always 1 in ALLOW_ALL mode.
 * Sinks (124.4c) call this and, on 0, call tk_cap_deny.
 */
int tk_cap_check(uint32_t cap);

/* Human-readable class name for a single TK_CAP_* bit (for diagnostics). */
const char *tk_cap_class_name(uint32_t cap);

/*
 * TK_REQUIRE — gate a capability sink. Placed at the top of a stdlib _w wrapper
 * (a resource-acquisition point: open file / listen / connect / spawn / setenv).
 * A no-op in ALLOW_ALL mode; under enforcement a missing grant prints CAP001 and
 * exits. Gating is at acquisition, not per read/write on an already-open handle
 * (Deno semantics — the check happens once, when authority is taken).
 */
#define TK_REQUIRE(cap) do { \
    if (!tk_cap_check(cap)) tk_cap_deny(cap); \
} while (0)

/*
 * tk_cap_deny — a denied sink: print a structured capability diagnostic that
 * names the missing grant and the flag that would grant it, then exit(1).
 * (124.4d refines the diagnostic; sinks are wired in 124.4c.)
 */
void tk_cap_deny(uint32_t cap);

/* Current mode / effective grants — exposed for testing and diagnostics. */
TkCapMode tk_cap_mode(void);
uint32_t  tk_cap_grants(void);

/*
 * Compile-time baked grant set. capabilities.c provides weak defaults
 * (absent / no grants / not enforcing); when the compiler bakes grants
 * (124.4b) it emits strong definitions that override these at link time.
 */
extern const uint32_t __tk_cap_baked_grants;
extern const int      __tk_cap_baked_present;
extern const int      __tk_cap_baked_enforce;

#endif /* TK_CAPABILITIES_H */
