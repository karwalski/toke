/* SPDX-License-Identifier: MIT */
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
 *   - a broker gates every sink at the point where authority is acquired.
 *
 * 138.19 (owner answer 2026-11-15, D1-D3 of
 * docs/decisions/138.19-scoped-capability-grants.md): a grant is either a
 * WHOLE class or a list of SCOPES for that class -- fs paths (component-wise,
 * after realpath canonicalisation), direction-prefixed net scopes
 * (connect:HOST[:PORT], listen:ADDR:PORT), process.spawn commands (resolved
 * on PATH and pinned at start-up) and env.write keys (one trailing `*`).
 * Every sink passes the value it is about to use to a scoped gate
 * (TK_REQUIRE_PATH / _NET / _CMD / _ENV).  A sink still on the class-only
 * TK_REQUIRE passes only when the WHOLE class is granted, so a scoped grant
 * fails closed there.  When a grant set is baked, run-time --allow-* flags
 * NARROW it (D2) unless the manifest says runtime = "union".  A class that
 * is granted but not for this scope is CAP002 (D3); a class not granted at
 * all is CAP001, as before.
 *
 * Which entries reach a sink, and whether each passes a SCOPED gate of the
 * right class, is checked by scripts/check_capability_sinks.py (conform case
 * C470, stories 121.22 and 138.19).
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

/* 138.19 D1: the two directions of the net class.  They are sub-bits OF the
 * net class (each carries TK_CAP_NET), not new classes: `net = true` grants
 * both, and a scope says which one it admits (connect:... / listen:...). */
#define TK_CAP_NET_CONNECT    (TK_CAP_NET | (1u << 5))
#define TK_CAP_NET_LISTEN     (TK_CAP_NET | (1u << 6))

/* Broker enforcement mode. */
typedef enum {
    TK_CAP_MODE_ALLOW_ALL = 0,  /* only after an explicit --allow-all */
    TK_CAP_MODE_ENFORCE   = 1   /* deny-by-default: only granted classes pass */
} TkCapMode;

/* 138.19: class indices, the `cls` of a baked scope (and of tkc.toml keys). */
enum {
    TK_CAP_IDX_FS_READ = 0, TK_CAP_IDX_FS_WRITE = 1, TK_CAP_IDX_NET = 2,
    TK_CAP_IDX_ENV_WRITE = 3, TK_CAP_IDX_PROCESS_SPAWN = 4, TK_CAP_NCLASSES = 5
};

/* 138.19: scope kinds, the `kind` of a baked scope. */
enum {
    TK_CAP_SK_PATH = 1, TK_CAP_SK_CONNECT = 2, TK_CAP_SK_LISTEN = 3,
    TK_CAP_SK_CMD = 4, TK_CAP_SK_ENV = 5
};

/* 138.19 D1: one baked scope, as the compiler emits it ({i32, i32, i8*}).
 * The text is the scope as written (./content, connect:api.example.com:443,
 * git, TOKE_*): paths and commands are canonicalised at start-up, against
 * the working directory, HOME and PATH of the process. */
typedef struct {
    int32_t     cls;    /* TK_CAP_IDX_* */
    int32_t     kind;   /* TK_CAP_SK_* */
    const char *text;
} TkCapBakedScope;

/* 138.19 D2: __tk_cap_baked_runtime values. */
#define TK_CAP_RUNTIME_LEGACY 0  /* no grant set baked (or a pre-138.19 binary): runtime flags union */
#define TK_CAP_RUNTIME_NARROW 1  /* a grant set is baked: runtime flags may only narrow it */
#define TK_CAP_RUNTIME_UNION  2  /* baked, and the manifest says runtime = "union" */

/*
 * tk_cap_init — initialise the broker from the compiled program's argv.
 * Reads the compiler-baked grant set (weak globals below) and the runtime
 * --allow-* flags: unioned with the baked set when none is baked (or the
 * manifest says runtime = "union"), intersected with it otherwise.  Exits 1
 * with CAP003 (one line, and a JSON line under TK_DIAG_JSON) on a malformed
 * scope or on a refused --allow-all.
 * Idempotent; called once from tk_runtime_init.
 */
void tk_cap_init(int argc, char **argv);

/*
 * tk_cap_check — is the WHOLE capability class `cap` (a TK_CAP_* bit)
 * granted?  Returns 1 if allowed, 0 if denied.  Always 1 in ALLOW_ALL mode.
 * A class granted only for listed scopes is not the whole class: 0.
 */
int tk_cap_check(uint32_t cap);

/* Human-readable class name for a single TK_CAP_* bit (for diagnostics). */
const char *tk_cap_class_name(uint32_t cap);

/*
 * tk_cap_is_grant_flag — is `arg` a consumed `--allow-*` capability flag?
 * Used by tk_runtime_init to strip these tokens from the program's argv so the
 * program never sees the capability flags (Deno-style). Returns 0 for argv[0]
 * callers to handle, and for any non-grant token.
 */
int tk_cap_is_grant_flag(const char *arg);

/*
 * TK_REQUIRE — the class-only gate.  It passes only when the WHOLE class is
 * granted (138.19 §3.4): under a scoped grant it denies, never grants silent
 * access.  Every sink that holds a path, host, command or key uses a scoped
 * gate below instead; C470 keeps it that way.
 */
#define TK_REQUIRE(cap) do { \
    if (!tk_cap_check(cap)) tk_cap_deny(cap); \
} while (0)

/*
 * 138.19 scoped gates.  Each takes the value the sink is about to use and
 * passes, or prints CAP001 (class not granted) / CAP002 (class granted, not
 * for this scope) and exits 1 before the syscall.  A no-op in ALLOW_ALL mode.
 *   TK_REQUIRE_PATH(TK_CAP_FS_READ | TK_CAP_FS_WRITE, path)
 *   TK_REQUIRE_NET(TK_CAP_NET_CONNECT, host, port)   host as the program passed it
 *   TK_REQUIRE_NET(TK_CAP_NET_LISTEN, addr, port)    the IP literal being bound
 *   TK_REQUIRE_CMD(argv0)                             before PATH resolution
 *   TK_REQUIRE_ENV(key)
 */
#define TK_REQUIRE_PATH(cap, path)      tk_cap_require_path((cap), (path))
#define TK_REQUIRE_NET(dir, host, port) tk_cap_require_net((dir), (host), (uint64_t)(port))
/* A connect request for a host name with no port (net.resolve, 138.25 x 138.19):
 * admitted by any connect scope whose host matches, whatever its port. */
#define TK_CAP_ANY_PORT UINT64_MAX
#define TK_REQUIRE_CMD(argv0)           tk_cap_require_cmd(argv0)
#define TK_REQUIRE_ENV(key)             tk_cap_require_env(key)
#define TK_REQUIRE_PATH_DIR(cap, path, dir) tk_cap_require_path_dir((cap), (path), (dir))

void tk_cap_require_path(uint32_t cap, const char *path);
void tk_cap_require_net(uint32_t dir, const char *host, uint64_t port);
void tk_cap_require_cmd(const char *argv0);
void tk_cap_require_env(const char *key);

/*
 * TK_REQUIRE_PATH_DIR / tk_cap_require_path_dir — TK_REQUIRE_PATH for a sink that also creates and
 * removes files beside `path` in `dir` (log.openaccess / log.openerror: a
 * lock sidecar and rotated files; 138.19).  The way to `path` is checked,
 * then `dir`, then `path`.  The directory's CAP002 carries a fix only when
 * `path` resolves under it, so the fix is the one edit that admits the call.
 */
void tk_cap_require_path_dir(uint32_t cap, const char *path, const char *dir);

/*
 * tk_cap_path_admitted — would TK_REQUIRE_PATH(cap, path) pass?  1 or 0,
 * never a diagnostic and never an exit.  For a sink that must leave out
 * what a scope does not admit rather than stop: file.listglob drops a match
 * found through a directory outside the scope (138.19 round 2).
 */
int tk_cap_path_admitted(uint32_t cap, const char *path);

/*
 * tk_cap_require_whole — `value` is something no scope can admit (an SQLite
 * URI filename, an ATTACH whose file name the statement computes): pass when
 * every class in `cap` is granted whole; otherwise CAP001 for a class not
 * granted at all, else CAP002 naming `value`, with `why` as its hint and no
 * fix, and exit 1.
 */
void tk_cap_require_whole(uint32_t cap, const char *value, const char *why);

/* Format an IPv4 address held in network byte order (a sockaddr_in's
 * sin_addr.s_addr) into buf, for TK_REQUIRE_NET(TK_CAP_NET_LISTEN, ...). */
#define TK_CAP_ADDRSTRLEN 64
const char *tk_cap_ip4str(uint32_t s_addr_be, char buf[TK_CAP_ADDRSTRLEN]);

/* The local address and port a socket is bound to (getsockname), formatted
 * for a listen check; *port is set to 0 and NULL returned when unknown. */
const char *tk_cap_sockname(int fd, char buf[TK_CAP_ADDRSTRLEN], uint64_t *port);

/*
 * tk_cap_deny — a denied class-only sink: print CAP001 (or, when the class is
 * granted for scopes only, CAP002 with no scope) and exit(1).
 */
void tk_cap_deny(uint32_t cap);

/*
 * tk_cap_scope_syntax — 138.19: is `text` a well-formed scope for class
 * `cls` (TK_CAP_IDX_*)?  Pure syntax, no file-system access: the compiler
 * validates manifest and --allow-* scopes with it, the runtime parses with
 * it.  Returns the scope kind (TK_CAP_SK_*), or 0 with a reason in err.
 */
int tk_cap_scope_syntax(int cls, const char *text, char *err, unsigned errsz);

/* Current mode / effective grants — exposed for testing and diagnostics.
 * tk_cap_grants() has a bit set for every class granted in any form. */
TkCapMode tk_cap_mode(void);
uint32_t  tk_cap_grants(void);

/*
 * Compile-time baked grant set. capabilities.c provides weak defaults
 * (absent / no grants / no scopes / legacy union); when the compiler bakes
 * grants (124.4b, 138.19) it emits strong definitions that override these at
 * link time.  __tk_cap_baked_grants carries the WHOLE classes; a class granted
 * for scopes only has its scopes in __tk_cap_baked_scopes and no bit.
 */
extern const uint32_t __tk_cap_baked_grants;
extern const int      __tk_cap_baked_present;
extern const int      __tk_cap_baked_enforce;
extern const TkCapBakedScope *const __tk_cap_baked_scopes;
extern const int      __tk_cap_baked_nscopes;
extern const int      __tk_cap_baked_runtime;

/*
 * 138.44 (D5): OS-level enforcement behind the broker, opt-in.  1 when the
 * binary was built with `os_enforce = true` in tkc.toml [capabilities] or
 * `tkc --cap-os-enforce`: tk_cap_init then also asks the kernel to refuse
 * what the grants do not admit (macOS sandbox profile, Linux Landlock and
 * seccomp; src/stdlib/cap_os.h), so native code and a sink the broker
 * misses are denied too.  The weak default is 0: no change.  Where the
 * kernel cannot enforce a grant, CAP004 (a warning) says so at start-up.
 */
extern const int      __tk_cap_baked_os;

#endif /* TK_CAPABILITIES_H */
