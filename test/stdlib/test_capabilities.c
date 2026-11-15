/*
 * test_capabilities.c — unit test for the capability broker (Epic 124.4a;
 * scoped grants, story 138.19).
 *
 * tk_cap_init is idempotent (one init per process), so the parent drives the
 * whole-class scenario of 124.4a, and every 138.19 scenario runs in a forked
 * child with its own argv: the child initialises the broker, makes one
 * request, and exits 0 when the request was admitted.  A denial exits 1 from
 * inside the broker (CAP001 / CAP002), which is the behaviour under test.
 * Nothing here opens a file or a socket: the scoped gates only compare.
 *
 * Compile:
 *   cc -std=c99 -D_GNU_SOURCE -iquote src/stdlib \
 *      test/stdlib/test_capabilities.c src/stdlib/capabilities.c -o /tmp/tc
 */
#include "capabilities.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define ASSERT(cond, msg) do { \
    if (cond) printf("pass: %s\n", (msg)); \
    else { printf("FAIL: %s\n", (msg)); failures++; } } while (0)

/* One request kind per child. */
enum { RQ_PATH_R, RQ_CONNECT, RQ_LISTEN, RQ_ENV, RQ_CLASSONLY_R, RQ_INIT_ONLY };

/* Fork; the child inits the broker from argv (NULL-terminated), makes the
 * request and exits 0 if it returned.  Returns the child's exit status. */
static int child(char **argv, int kind, const char *v, unsigned port) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        int argc = 0;
        while (argv[argc]) argc++;
        tk_cap_init(argc, argv);
        switch (kind) {
            case RQ_PATH_R:      tk_cap_require_path(TK_CAP_FS_READ, v); break;
            case RQ_CONNECT:     tk_cap_require_net(TK_CAP_NET_CONNECT, v, port); break;
            case RQ_LISTEN:      tk_cap_require_net(TK_CAP_NET_LISTEN, v, port); break;
            case RQ_ENV:         tk_cap_require_env(v); break;
            case RQ_CLASSONLY_R: TK_REQUIRE(TK_CAP_FS_READ); break;
            default: break;
        }
        _exit(0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

#define ADMIT(argv, kind, v, port, msg) ASSERT(child((argv), (kind), (v), (port)) == 0, (msg))
#define DENY(argv, kind, v, port, msg)  ASSERT(child((argv), (kind), (v), (port)) == 1, (msg))

static void scoped(void) {
    /* D1 net: a wildcard covers one or more labels, never the apex, and is
     * matched on the name the program passed (case and a final dot aside). */
    char *wild[] = { "t", "--allow-net=connect:*.example.com:443", NULL };
    ADMIT(wild, RQ_CONNECT, "a.example.com", 443, "net: *.example.com admits a.example.com:443");
    ADMIT(wild, RQ_CONNECT, "b.a.example.com", 443, "net: *.example.com admits b.a.example.com:443");
    ADMIT(wild, RQ_CONNECT, "A.Example.COM.", 443, "net: host compared case-insensitively, final dot dropped");
    DENY(wild, RQ_CONNECT, "example.com", 443, "net: *.example.com denies the apex example.com");
    DENY(wild, RQ_CONNECT, "evilexample.com", 443, "net: *.example.com denies evilexample.com");
    DENY(wild, RQ_CONNECT, "a.example.com", 80, "net: the scope's port is enforced (80 denied)");
    DENY(wild, RQ_LISTEN, "127.0.0.1", 443, "net: a connect-only grant denies a listen");

    /* Documentation ranges only (RFC 5737): nothing here is dialled. */
    char *cidr[] = { "t", "--allow-net=connect:198.51.100.0/24", NULL };
    ADMIT(cidr, RQ_CONNECT, "198.51.100.7", 9, "net: an IP literal inside a CIDR scope (any port)");
    DENY(cidr, RQ_CONNECT, "203.0.113.1", 9, "net: an IP literal outside the CIDR scope");
    DENY(cidr, RQ_CONNECT, "ten.example.com", 9, "net: a CIDR scope never matches a name (no DNS)");

    char *v6[] = { "t", "--allow-net=connect:[::1]:443", NULL };
    ADMIT(v6, RQ_CONNECT, "::1", 443, "net: [::1]:443 admits ::1");
    ADMIT(v6, RQ_CONNECT, "[::1]", 443, "net: [::1]:443 admits [::1] (brackets dropped)");
    DENY(v6, RQ_CONNECT, "::2", 443, "net: [::1]:443 denies ::2");

    char *lis[] = { "t", "--allow-net=listen:127.0.0.1:8080", NULL };
    ADMIT(lis, RQ_LISTEN, "127.0.0.1", 8080, "net: listen:127.0.0.1:8080 admits that bind");
    DENY(lis, RQ_LISTEN, "0.0.0.0", 8080, "net: listen:127.0.0.1:8080 denies a bind to 0.0.0.0");
    DENY(lis, RQ_LISTEN, "127.0.0.1", 8081, "net: listen:127.0.0.1:8080 denies another port");
    DENY(lis, RQ_CONNECT, "127.0.0.1", 8080, "net: a listen-only grant denies a connect");

    /* env.write: a key, and one trailing '*' as a prefix. */
    char *env[] = { "t", "--allow-env=TZ,TOKE_*", NULL };
    ADMIT(env, RQ_ENV, "TZ", 0, "env: TZ admitted");
    ADMIT(env, RQ_ENV, "TOKE_HOME", 0, "env: TOKE_* admits TOKE_HOME");
    DENY(env, RQ_ENV, "TOKEN", 0, "env: TOKE_* denies TOKEN");
    DENY(env, RQ_ENV, "PATH", 0, "env: PATH denied");

    /* §3.4: a class-only gate under a scoped grant fails closed. */
    char *sc[] = { "t", "--allow-read=/", NULL };
    DENY(sc, RQ_CLASSONLY_R, NULL, 0, "fail closed: class-only TK_REQUIRE denied under fs_read = [\"/\"]");
    ADMIT(sc, RQ_PATH_R, "/etc", 0, "scoped: fs_read = [\"/\"] admits /etc through the scoped gate");
    char *wh[] = { "t", "--allow-read", NULL };
    ADMIT(wh, RQ_CLASSONLY_R, NULL, 0, "whole class: class-only TK_REQUIRE admitted");
    ADMIT(wh, RQ_PATH_R, "/nonexistent/x", 0, "whole class: scoped gate admits without canonicalising");

    /* A malformed run-time scope stops the program at start-up. */
    char *bad[] = { "t", "--allow-net=api.example.com", NULL };
    DENY(bad, RQ_INIT_ONLY, NULL, 0, "init: --allow-net=api.example.com (no connect:/listen:) refused");
    char *badall[] = { "t", "--allow-all=1", NULL };
    DENY(badall, RQ_INIT_ONLY, NULL, 0, "init: --allow-all=1 refused");
}

static void syntax(void) {
    char err[160];
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "connect:api.example.com:443", err, sizeof err) == TK_CAP_SK_CONNECT,
           "syntax: connect:HOST:PORT");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "connect:api.example.com", err, sizeof err) == TK_CAP_SK_CONNECT,
           "syntax: connect:HOST (any port)");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "listen:127.0.0.1:*", err, sizeof err) == TK_CAP_SK_LISTEN,
           "syntax: listen:ADDR:*");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "listen:127.0.0.1", err, sizeof err) == 0,
           "syntax: listen without a port is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "listen:localhost:80", err, sizeof err) == 0,
           "syntax: listen needs an IP literal");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "udp:192.0.2.4:5", err, sizeof err) == 0,
           "syntax: an unknown net prefix is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "192.0.2.1:9", err, sizeof err) == 0,
           "syntax: a net scope without a direction is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "connect:*:443", err, sizeof err) == 0,
           "syntax: a bare * host is malformed (that is the class)");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_NET, "connect:h:0", err, sizeof err) == 0,
           "syntax: connect port 0 is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_FS_READ, "", err, sizeof err) == 0,
           "syntax: an empty scope is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_FS_READ, "~bob/x", err, sizeof err) == 0,
           "syntax: ~user is malformed");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_FS_READ, "~/.config/loke", err, sizeof err) == TK_CAP_SK_PATH,
           "syntax: ~/ path");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_ENV_WRITE, "A*B", err, sizeof err) == 0,
           "syntax: env '*' only last");
    ASSERT(tk_cap_scope_syntax(TK_CAP_IDX_PROCESS_SPAWN, "git", err, sizeof err) == TK_CAP_SK_CMD,
           "syntax: a bare command name");
}

int main(void) {
    /* Class-name mapping (pure). */
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_FS_READ), "fs.read") == 0,       "name: fs.read");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_FS_WRITE), "fs.write") == 0,     "name: fs.write");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_NET), "net") == 0,               "name: net");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_ENV_WRITE), "env.write") == 0,   "name: env.write");
    ASSERT(strcmp(tk_cap_class_name(TK_CAP_PROCESS_SPAWN), "process.spawn") == 0, "name: process.spawn");

    syntax();
    scoped();

    /* 124.4g: deny-by-default — the broker enforces before any grant is seen. */
    ASSERT(tk_cap_mode() == TK_CAP_MODE_ENFORCE, "default mode is ENFORCE (deny-by-default)");
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0, "default: fs.write denied before any grant");
    ASSERT(tk_cap_check(TK_CAP_NET) == 0,      "default: net denied before any grant");

    /* A whole-class grant keeps deny-by-default and grants only the named
     * class; a scoped one (138.19) grants the class for its scope only, so the
     * class-only check says no. */
    char *argv[] = { "prog", "--allow-net=connect:api.example.com", "--allow-read", "input.txt", NULL };
    tk_cap_init(4, argv);

    ASSERT(tk_cap_mode() == TK_CAP_MODE_ENFORCE, "mode: still ENFORCE with a specific grant");
    ASSERT(tk_cap_check(TK_CAP_FS_READ) == 1,      "granted: fs.read (whole class) passes");
    ASSERT(tk_cap_check(TK_CAP_NET) == 0,          "scoped: net is not the whole class");
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0,     "denied: fs.write blocked");
    ASSERT(tk_cap_check(TK_CAP_ENV_WRITE) == 0,    "denied: env.write blocked");
    ASSERT(tk_cap_check(TK_CAP_PROCESS_SPAWN) == 0,"denied: process.spawn blocked");
    ASSERT((tk_cap_grants() & TK_CAP_NET) && (tk_cap_grants() & TK_CAP_FS_READ),
           "grants: net (scoped) + fs.read bits set");
    ASSERT(tk_cap_is_grant_flag("--allow-read=/srv") && !tk_cap_is_grant_flag("--allow-readers"),
           "flags: --allow-read=VALUE is a grant flag, --allow-readers is not");

    /* Idempotent: a second init does not change state. */
    char *argv2[] = { "prog", "--allow-write", NULL };
    tk_cap_init(2, argv2);
    ASSERT(tk_cap_check(TK_CAP_FS_WRITE) == 0, "idempotent: re-init ignored");

    if (failures == 0) { printf("All capability checks passed.\n"); return 0; }
    fprintf(stderr, "%d check(s) failed.\n", failures);
    return 1;
}
