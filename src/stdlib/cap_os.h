/* SPDX-License-Identifier: MIT */
/*
 * cap_os.h — OS-level enforcement of the capability grants, behind the
 * in-process broker (story 138.44; decision D5 of
 * docs/decisions/138.19-scoped-capability-grants.md; design in
 * docs/decisions/138.44-os-enforcement.md).
 *
 * The broker (capabilities.c) is a policy layer in the standard library:
 * native code -- a user `extern` C function, an FFI call, a stdlib sink a
 * defect left ungated -- does not pass through it.  When a binary is built
 * with `os_enforce = true` (tkc.toml [capabilities]) or `tkc
 * --cap-os-enforce`, tk_cap_init also asks the kernel to refuse what the
 * grants do not admit, so such a path is still denied:
 *
 *   macOS  sandbox_init(3) with an SBPL profile generated from the grants
 *          (the profile language sandbox-exec(1) takes; no entitlement);
 *   Linux  Landlock (fs read / write / execute, and TCP bind / connect by
 *          port from ABI 4) plus a seccomp filter that refuses IPv4/IPv6
 *          sockets when the net class is not granted at all;
 *   other  nothing: CAP004 says so.
 *
 * The OS layer is a SUPERSET of the broker -- it never refuses what the
 * broker admits -- because the kernel cannot express every scope: hosts
 * (only ports and, on macOS, loopback), env keys (in-process memory), and
 * the broker's "the way there" rule.  It adds a fixed read-only system
 * baseline (the C library, time zones, resolver and CA files, /dev/null)
 * that holds no user data.  The broker is unchanged and still the policy;
 * this is defence in depth.
 *
 * Everything here is static and platform-neutral except the two apply
 * functions, so test/stdlib/test_cap_os.c can include this header and test
 * the profile text and the Landlock rights on any host.  Included by
 * capabilities.c only.
 */
#ifndef TK_CAP_OS_H
#define TK_CAP_OS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Every function is static and some are used on one platform only. */
#define TK_OS_FN static __attribute__((unused))

/* ── The plan: what the OS layer admits, built from the grants ─────── */

typedef struct {
    int    on;          /* restrict this access at all (the class is not whole) */
    int    n, cap;
    char **v;           /* canonical absolute paths */
} TkOsPaths;

typedef struct {
    long port;          /* -1: any port */
    int  loopback;      /* the scope names only loopback addresses */
} TkOsPort;

typedef struct {
    int       on;
    int       n, cap;
    TkOsPort *v;
} TkOsPorts;

typedef struct {
    TkOsPaths read, write, exec;
    TkOsPorts bind, connect;
    int       net_none;   /* the net class is not granted in any form */
    char     *cwd;        /* the working directory at start-up (macOS: see tk_os_sbpl) */
} TkOsPlan;

/* The reason a plan is weaker than the grants, or could not be applied: the
 * text of one CAP004 line.  `layer` names the mechanism. */
typedef void (*TkOsWarn)(const char *layer, const char *reason);

/* A path the OS layer can carry as text: absolute, no control characters
 * (an SBPL string and a CAP004 line both hold it). */
TK_OS_FN int tk_os_path_ok(const char *p) {
    if (!p || p[0] != '/') return 0;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        if (*c < 0x20 || *c == 0x7f) return 0;
    return 1;
}

/* Add a path; 0 when it cannot be carried (the caller lifts the restriction
 * on that access, so the OS layer never refuses what the broker admits). */
TK_OS_FN int tk_os_add_path(TkOsPaths *s, const char *p) {
    if (!tk_os_path_ok(p)) return 0;
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], p)) return 1;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        char **v = (char **)realloc(s->v, (size_t)nc * sizeof *v);
        if (!v) return 0;
        s->v = v; s->cap = nc;
    }
    char *d = strdup(p);
    if (!d) return 0;
    s->v[s->n++] = d;
    return 1;
}

TK_OS_FN int tk_os_add_port(TkOsPorts *s, long port, int loopback) {
    for (int i = 0; i < s->n; i++)
        if (s->v[i].port == port && s->v[i].loopback == loopback) return 1;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        TkOsPort *v = (TkOsPort *)realloc(s->v, (size_t)nc * sizeof *v);
        if (!v) return 0;
        s->v = v; s->cap = nc;
    }
    s->v[s->n].port = port;
    s->v[s->n].loopback = loopback;
    s->n++;
    return 1;
}

TK_OS_FN void tk_os_plan_free(TkOsPlan *p) {
    TkOsPaths *ps[3] = { &p->read, &p->write, &p->exec };
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < ps[k]->n; i++) free(ps[k]->v[i]);
        free(ps[k]->v);
    }
    free(p->bind.v);
    free(p->connect.v);
    free(p->cwd);
    memset(p, 0, sizeof *p);
}

/* ── A growable text buffer ─────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; int oom; } TkOsBuf;

TK_OS_FN void tk_os_put(TkOsBuf *b, const char *s) {
    size_t l = strlen(s);
    if (b->oom) return;
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 1024;
        while (nc < b->n + l + 1) nc *= 2;
        char *q = (char *)realloc(b->p, nc);
        if (!q) { b->oom = 1; return; }
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

/* ── The read-only system baseline ──────────────────────────────────── */

/* Read: the C library and the rest of the OS image, time zones, resolver
 * configuration, CA bundles and character devices.  No home directory, no
 * temporary directory, nothing a user writes. */
static const char *const __attribute__((unused)) tk_os_darwin_read_base[] = {
    "/usr", "/System", "/bin", "/sbin", "/dev",
    "/private/etc", "/private/var/db/timezone", "/private/var/run/resolv.conf",
    "/opt/homebrew/etc/openssl@3", "/usr/local/etc/openssl@3", NULL
};
static const char *const __attribute__((unused)) tk_os_linux_read_base[] = {
    "/usr", "/lib", "/lib64", "/bin", "/sbin", "/etc", "/run/systemd/resolve",
    "/dev/null", "/dev/zero", "/dev/full", "/dev/random", "/dev/urandom", "/dev/tty",
    NULL
};
/* Write: the character devices a program writes without meaning a file. */
static const char *const __attribute__((unused)) tk_os_darwin_write_base[] = {
    "/dev/null", "/dev/zero", "/dev/tty", "/dev/stdout", "/dev/stderr", "/dev/dtracehelper",
    NULL
};
static const char *const __attribute__((unused)) tk_os_linux_write_base[] = {
    "/dev/null", "/dev/zero", "/dev/full", "/dev/tty", NULL
};
/* Execute (Linux): where the dynamic loader lives.  The kernel opens a
 * program's ELF interpreter for execution, and Landlock checks that open,
 * so a granted command would not start without it. */
static const char *const __attribute__((unused)) tk_os_linux_exec_base[] = {
    "/lib", "/lib64", "/usr/lib", "/usr/lib64", NULL
};

/* The read baseline of the platform this file is built for. */
#if defined(__APPLE__)
#define TK_OS_READ_BASE tk_os_darwin_read_base
#else
#define TK_OS_READ_BASE tk_os_linux_read_base
#endif

/* Is `path` the directory `dir` or beneath it? */
TK_OS_FN int tk_os_beneath(const char *path, const char *dir) {
    size_t l = strlen(dir);
    if (l == 1 && dir[0] == '/') return path[0] == '/';
    return !strncmp(path, dir, l) && (path[l] == '\0' || path[l] == '/');
}

/* ── Interpreters of granted commands ──────────────────────────────── */

/* The kernel execs a script's `#!` interpreter as well as the script, and
 * both platforms check that exec (Landlock: EXECUTE on the interpreter;
 * macOS: process-exec), so a granted script would not start with only its
 * own path admitted.  These helpers add, for each command in the exec
 * list, the interpreter its `#!` line names (realpath), the command
 * `/usr/bin/env NAME` resolves NAME to on PATH, and on macOS the shell
 * /bin/sh hands itself to (/private/var/select/sh).  What those then exec
 * on their own is not followed: see the design note's side effects. */

/* Add realpath(p) to s when it is an executable regular file. */
TK_OS_FN void tk_os_add_exe(TkOsPaths *s, const char *p) {
    char r[PATH_MAX];
    if (!p || access(p, X_OK) != 0 || !realpath(p, r)) return;
    (void)tk_os_add_path(s, r);
}

/* Resolve a bare command name on PATH, as env(1) does; add it to s. */
TK_OS_FN void tk_os_add_on_path(TkOsPaths *s, const char *name) {
    const char *path = getenv("PATH");
    char cand[PATH_MAX];
    size_t nl = strlen(name);
    if (!path || nl == 0 || strchr(name, '/')) return;
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        if (l > 0 && l + 1 + nl < sizeof cand) {
            memcpy(cand, path, l);
            cand[l] = '/';
            memcpy(cand + l + 1, name, nl + 1);
            if (access(cand, X_OK) == 0) { tk_os_add_exe(s, cand); return; }
        }
        if (!e) break;
        path = e + 1;
    }
}

/* Read the `#!` line of `file`: the interpreter into interp, and the first
 * argument (the word after it) into arg; 0 when the file is not a script. */
TK_OS_FN int tk_os_shebang(const char *file, char *interp, size_t in, char *arg, size_t an) {
    char line[512];
    FILE *f = fopen(file, "rb");
    if (!f) return 0;
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = '\0';
    if (n < 3 || line[0] != '#' || line[1] != '!') return 0;
    char *p = line + 2, *end;
    while (*p == ' ' || *p == '\t') p++;
    end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
    if (end == p || (size_t)(end - p) >= in) return 0;
    memcpy(interp, p, (size_t)(end - p));
    interp[end - p] = '\0';
    p = end;
    while (*p == ' ' || *p == '\t') p++;
    end = p;
    while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') end++;
    if ((size_t)(end - p) >= an) end = p;
    memcpy(arg, p, (size_t)(end - p));
    arg[end - p] = '\0';
    return 1;
}

/* Complete the exec list with the interpreters it needs (bounded: the
 * list grows by what it finds, and each new entry is looked at once). */
TK_OS_FN void tk_os_add_interpreters(TkOsPaths *s) {
    char interp[PATH_MAX], arg[256];
    for (int i = 0; i < s->n && i < 64; i++) {
#if defined(__APPLE__)
        if (!strcmp(s->v[i], "/bin/sh")) tk_os_add_exe(s, "/private/var/select/sh");
#endif
        if (!tk_os_shebang(s->v[i], interp, sizeof interp, arg, sizeof arg)) continue;
        if (interp[0] != '/') continue;
        tk_os_add_exe(s, interp);
        const char *base = strrchr(interp, '/') + 1;
        /* `#!/usr/bin/env NAME`: env execs NAME found on PATH (`-S` and the
         * other options are not followed). */
        if (!strcmp(base, "env") && arg[0] && arg[0] != '-') tk_os_add_on_path(s, arg);
    }
}

/* A granted command (or interpreter) whose file lies outside the read
 * scopes and the read baseline cannot load its own code under the OS layer
 * (dyld / ld.so and a script's interpreter read it), so the kernel would
 * refuse a spawn the broker admits.  Name each one: the fix is to add its
 * install prefix (for example /opt/homebrew) to fs.read. */
TK_OS_FN void tk_os_warn_exec_unreadable(const TkOsPlan *p, const char *const *base, TkOsWarn warn) {
    if (!p->read.on || !p->exec.on) return;
    for (int i = 0; i < p->exec.n; i++) {
        const char *x = p->exec.v[i];
        int ok = 0;
        for (int j = 0; base[j] && !ok; j++) ok = tk_os_beneath(x, base[j]);
        for (int j = 0; j < p->read.n && !ok; j++) ok = tk_os_beneath(x, p->read.v[j]);
        if (ok) continue;
        char why[PATH_MAX + 320];
        snprintf(why, sizeof why, "the granted command %s lies outside fs.read and the system "
                 "read baseline, so the OS layer refuses it its own files and libraries and it "
                 "will not run; add its install prefix to fs.read", x);
        warn("plan", why);
    }
}

#if defined(__linux__)
/* The process's own standard output and error.  The broker admits
 * "/dev/stdout" and "/dev/stderr" with no grant (log_glue.c: they are not
 * file-system authority), but on Linux opening them reopens, through
 * /proc/self/fd, the file the descriptor holds, and Landlock checks that
 * file at its own path.  Admit writes to exactly that file when the
 * descriptor is already open for writing: the process can already write
 * and truncate it.  A pipe or socket needs no rule (Landlock leaves them). */
TK_OS_FN void tk_os_add_own_streams(TkOsPaths *w) {
    for (int fd = 1; fd <= 2; fd++) {
        char lnk[32], path[PATH_MAX];
        struct stat st, ps;
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || (fl & O_ACCMODE) == O_RDONLY) continue;
        if (fstat(fd, &st) != 0 || !(S_ISREG(st.st_mode) || S_ISCHR(st.st_mode))) continue;
        snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(lnk, path, sizeof path - 1);
        if (n <= 0 || path[0] != '/') continue;
        path[n] = '\0';
        /* " (deleted)", or a different file now at that path: no rule */
        if (stat(path, &ps) != 0 || ps.st_ino != st.st_ino || ps.st_dev != st.st_dev) continue;
        (void)tk_os_add_path(w, path);
    }
}
#endif

/* Complete a plan before it is applied: interpreters, the process's own
 * standard streams (Linux; macOS admits /dev/fd in the profile), then the
 * warning. */
TK_OS_FN void tk_os_complete(TkOsPlan *p, TkOsWarn warn) {
    if (p->exec.on) tk_os_add_interpreters(&p->exec);
#if defined(__linux__)
    if (p->write.on) tk_os_add_own_streams(&p->write);
#endif
    tk_os_warn_exec_unreadable(p, TK_OS_READ_BASE, warn);
}

/* ── macOS: the SBPL profile ────────────────────────────────────────── */

/* An SBPL string literal (TinyScheme: `\"` and `\\` are the escapes; the
 * plan holds no control characters, tk_os_path_ok). */
TK_OS_FN void tk_os_sbpl_str(TkOsBuf *b, const char *s) {
    char one[3];
    tk_os_put(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { one[0] = '\\'; one[1] = *s; one[2] = '\0'; }
        else { one[0] = *s; one[1] = '\0'; }
        tk_os_put(b, one);
    }
    tk_os_put(b, "\"");
}

TK_OS_FN void tk_os_sbpl_paths(TkOsBuf *b, const char *op, const char *const *base,
                             const TkOsPaths *s, const char *base_filter) {
    tk_os_put(b, "(deny "); tk_os_put(b, op); tk_os_put(b, ")\n");
    tk_os_put(b, "(allow "); tk_os_put(b, op);
    for (int i = 0; base && base[i]; i++) {
        tk_os_put(b, "\n  ("); tk_os_put(b, base_filter); tk_os_put(b, " ");
        tk_os_sbpl_str(b, base[i]); tk_os_put(b, ")");
    }
    for (int i = 0; i < s->n; i++) {
        tk_os_put(b, "\n  (subpath "); tk_os_sbpl_str(b, s->v[i]); tk_os_put(b, ")");
    }
    tk_os_put(b, ")\n");
}

/* "*:443", "localhost:8080", "*:*": SBPL's ip filter takes `*` or
 * `localhost` as the host, so a host scope narrows to its port only. */
TK_OS_FN void tk_os_sbpl_ip(TkOsBuf *b, const TkOsPort *p) {
    char t[32];
    if (p->port < 0) snprintf(t, sizeof t, "\"%s:*\"", p->loopback ? "localhost" : "*");
    else snprintf(t, sizeof t, "\"%s:%ld\"", p->loopback ? "localhost" : "*", p->port);
    tk_os_put(b, t);
}

TK_OS_FN void tk_os_sbpl_ports(TkOsBuf *b, const char *op, const char *dir, const TkOsPorts *s) {
    tk_os_put(b, "(deny "); tk_os_put(b, op); tk_os_put(b, " (");
    tk_os_put(b, dir); tk_os_put(b, " ip \"*:*\"))\n");
    for (int i = 0; i < s->n; i++) {
        tk_os_put(b, "(allow "); tk_os_put(b, op); tk_os_put(b, " (");
        tk_os_put(b, dir); tk_os_put(b, " ip ");
        tk_os_sbpl_ip(b, &s->v[i]);
        tk_os_put(b, "))\n");
    }
}

/* The whole profile.  `allow default` and then a deny-and-allow pair per
 * restricted access: everything the grants do not name stays as it was
 * (mach services, sysctl, signals), and the later rule wins in SBPL. */
TK_OS_FN void tk_os_sbpl(const TkOsPlan *p, TkOsBuf *b) {
    tk_os_put(b, "(version 1)\n(allow default)\n");
    if (p->read.on) {
        tk_os_sbpl_paths(b, "file-read-data", tk_os_darwin_read_base, &p->read, "subpath");
        /* getcwd(3) opens "." on macOS: without the working directory itself
         * (the directory, not beneath it) the broker could not resolve a
         * relative path, and would refuse what it admits. */
        if (p->cwd && tk_os_path_ok(p->cwd)) {
            tk_os_put(b, "(allow file-read-data (literal ");
            tk_os_sbpl_str(b, p->cwd);
            tk_os_put(b, "))\n");
        }
        /* The root directory itself (its names, not beneath it): dyld reads
         * it in every freshly exec'd process, so without it a granted
         * command dies with SIGABRT before main. */
        tk_os_put(b, "(allow file-read-data (literal \"/\"))\n");
    }
    if (p->write.on) {
        tk_os_sbpl_paths(b, "file-write*", tk_os_darwin_write_base, &p->write, "literal");
        /* /dev/stderr and /dev/stdout are links to /dev/fd/2 and /dev/fd/1,
         * and the profile checks the /dev/fd path.  Opening one duplicates a
         * descriptor the process already holds, so it adds no authority, and
         * the broker admits the own streams with no grant (log_glue.c). */
        tk_os_put(b, "(allow file-write* (subpath \"/dev/fd\"))\n");
    }
    if (p->exec.on) {
        tk_os_put(b, "(deny process-exec*)\n");
        for (int i = 0; i < p->exec.n; i++) {
            tk_os_put(b, "(allow process-exec* (literal ");
            tk_os_sbpl_str(b, p->exec.v[i]);
            tk_os_put(b, "))\n");
        }
    }
    if (p->bind.on)    tk_os_sbpl_ports(b, "network-bind", "local", &p->bind);
    if (p->connect.on) tk_os_sbpl_ports(b, "network-outbound", "remote", &p->connect);
}

/* ── Linux: Landlock rights (platform-neutral, so they are testable) ── */

#define TK_LL_EXECUTE     (1ull << 0)
#define TK_LL_WRITE_FILE  (1ull << 1)
#define TK_LL_READ_FILE   (1ull << 2)
#define TK_LL_READ_DIR    (1ull << 3)
#define TK_LL_REMOVE_DIR  (1ull << 4)
#define TK_LL_REMOVE_FILE (1ull << 5)
#define TK_LL_MAKE_CHAR   (1ull << 6)
#define TK_LL_MAKE_DIR    (1ull << 7)
#define TK_LL_MAKE_REG    (1ull << 8)
#define TK_LL_MAKE_SOCK   (1ull << 9)
#define TK_LL_MAKE_FIFO   (1ull << 10)
#define TK_LL_MAKE_BLOCK  (1ull << 11)
#define TK_LL_MAKE_SYM    (1ull << 12)
#define TK_LL_REFER       (1ull << 13)   /* ABI 2 */
#define TK_LL_TRUNCATE    (1ull << 14)   /* ABI 3 */
#define TK_LL_NET_BIND    (1ull << 0)    /* ABI 4 */
#define TK_LL_NET_CONNECT (1ull << 1)    /* ABI 4 */

/* The rights a rule on a regular file may carry (Landlock refuses a
 * directory right on a file). */
#define TK_LL_FILE_RIGHTS (TK_LL_EXECUTE | TK_LL_WRITE_FILE | TK_LL_READ_FILE | TK_LL_TRUNCATE)

TK_OS_FN uint64_t tk_ll_read_rights(void) { return TK_LL_READ_FILE | TK_LL_READ_DIR; }

TK_OS_FN uint64_t tk_ll_write_rights(int abi) {
    uint64_t r = TK_LL_WRITE_FILE | TK_LL_REMOVE_DIR | TK_LL_REMOVE_FILE | TK_LL_MAKE_CHAR |
                 TK_LL_MAKE_DIR | TK_LL_MAKE_REG | TK_LL_MAKE_SOCK | TK_LL_MAKE_FIFO |
                 TK_LL_MAKE_BLOCK | TK_LL_MAKE_SYM;
    if (abi >= 2) r |= TK_LL_REFER;
    if (abi >= 3) r |= TK_LL_TRUNCATE;
    return r;
}

/* The fs rights the ruleset handles: only the accesses the plan restricts. */
TK_OS_FN uint64_t tk_ll_handled_fs(const TkOsPlan *p, int abi) {
    uint64_t h = 0;
    if (p->read.on)  h |= tk_ll_read_rights();
    if (p->write.on) h |= tk_ll_write_rights(abi);
    if (p->exec.on)  h |= TK_LL_EXECUTE;
    return h;
}

/* A connect or bind scope with any port cannot be a Landlock port rule:
 * that direction is left to the broker. */
TK_OS_FN int tk_os_ports_any(const TkOsPorts *s) {
    for (int i = 0; i < s->n; i++)
        if (s->v[i].port < 0) return 1;
    return 0;
}

TK_OS_FN uint64_t tk_ll_handled_net(const TkOsPlan *p, int abi) {
    uint64_t h = 0;
    if (abi < 4) return 0;
    if (p->bind.on && !tk_os_ports_any(&p->bind))       h |= TK_LL_NET_BIND;
    if (p->connect.on && !tk_os_ports_any(&p->connect)) h |= TK_LL_NET_CONNECT;
    return h;
}

/* The Linux rules as text, one per line (TK_CAP_OS_SHOW and the tests). */
TK_OS_FN void tk_ll_describe(const TkOsPlan *p, int abi, TkOsBuf *b) {
    char t[96];
    snprintf(t, sizeof t, "landlock abi %d handled_fs 0x%llx handled_net 0x%llx\n", abi,
             (unsigned long long)tk_ll_handled_fs(p, abi),
             (unsigned long long)tk_ll_handled_net(p, abi));
    tk_os_put(b, t);
    struct { int on; const char *what; const char *const *base; const TkOsPaths *s; } k[3] = {
        { p->read.on,  "read",    tk_os_linux_read_base,  &p->read  },
        { p->write.on, "write",   tk_os_linux_write_base, &p->write },
        { p->exec.on,  "execute", tk_os_linux_exec_base,  &p->exec  },
    };
    for (int j = 0; j < 3; j++) {
        if (!k[j].on) continue;
        for (int i = 0; k[j].base[i]; i++) {
            tk_os_put(b, k[j].what); tk_os_put(b, " base "); tk_os_put(b, k[j].base[i]); tk_os_put(b, "\n");
        }
        for (int i = 0; i < k[j].s->n; i++) {
            tk_os_put(b, k[j].what); tk_os_put(b, " scope "); tk_os_put(b, k[j].s->v[i]); tk_os_put(b, "\n");
        }
    }
    const TkOsPorts *ns[2] = { &p->bind, &p->connect };
    const char *nw[2] = { "bind", "connect" };
    uint64_t nb[2] = { TK_LL_NET_BIND, TK_LL_NET_CONNECT };
    for (int j = 0; j < 2; j++) {
        if (!(tk_ll_handled_net(p, abi) & nb[j])) continue;
        for (int i = 0; i < ns[j]->n; i++) {
            snprintf(t, sizeof t, "%s port %ld\n", nw[j], ns[j]->v[i].port);
            tk_os_put(b, t);
        }
    }
    if (p->net_none) tk_os_put(b, "seccomp socket AF_INET AF_INET6 io_uring_setup x32 compat-socketcall: EACCES; other arch: kill\n");
}

/* ── Linux: the seccomp socket filter (platform-neutral, so testable) ─ */

/* One classic-BPF instruction: the layout of struct sock_filter. */
typedef struct { uint16_t code; uint8_t jt, jf; uint32_t k; } TkBpf;
#define TK_BPF_LD_W_ABS 0x20u          /* BPF_LD | BPF_W | BPF_ABS */
#define TK_BPF_JEQ_K    0x15u          /* BPF_JMP | BPF_JEQ | BPF_K */
#define TK_BPF_JGE_K    0x35u          /* BPF_JMP | BPF_JGE | BPF_K */
#define TK_BPF_RET_K    0x06u          /* BPF_RET | BPF_K */
#define TK_SC_RET_KILL_PROCESS 0x80000000u
#define TK_SC_RET_ERRNO        0x00050000u
#define TK_SC_RET_ALLOW        0x7fff0000u
#define TK_SC_NONE             0xFFFFFFFFu   /* a number no system call has */
/* struct seccomp_data: int nr; __u32 arch; __u64 instruction_pointer; __u64 args[6]. */
#define TK_SC_OFF_NR   0u
#define TK_SC_OFF_ARCH 4u
#define TK_SC_OFF_ARG0 16u
#define TK_SC_FILTER_LEN 19
#define TK_OS_AF_INET6 10u   /* Linux's AF_INET6 (the filter is Linux's; macOS's is 30) */

/* What the filter needs to know about one architecture. */
typedef struct {
    uint32_t arch;            /* AUDIT_ARCH_* of native system calls */
    uint32_t compat_arch;     /* AUDIT_ARCH_* of the 32-bit compat entry */
    uint32_t nr_socket, nr_io_uring, x32_bit;   /* native; x32_bit 0: none */
    uint32_t c_socket, c_socketcall, c_io_uring; /* compat; TK_SC_NONE: none */
    uint32_t arg0;            /* offset of args[0]'s low word */
} TkScArch;

/* x86_64: the i386 entry (int 0x80) has its own numbers, and socketcall,
 * whose domain sits in user memory the filter cannot read. */
static const TkScArch __attribute__((unused)) tk_sc_x86_64 = {
    0xC000003Eu, 0x40000003u, 41u, 425u, 0x40000000u, 359u, 102u, 425u, TK_SC_OFF_ARG0
};
/* aarch64: the AArch32 compat entry (EABI: no socketcall). */
static const TkScArch __attribute__((unused)) tk_sc_aarch64 = {
    0xC00000B7u, 0x40000028u, 198u, 425u, 0u, 281u, TK_SC_NONE, 425u, TK_SC_OFF_ARG0
};

/* The net class is not granted at all: refuse IPv4 / IPv6 sockets of every
 * kind (Landlock covers TCP only), io_uring (which opens sockets without
 * the socket call), x32 calls, and on the compat entry also socketcall
 * (every socket operation; its domain cannot be read).  A system call of
 * any other architecture kills the process: it cannot be checked. */
TK_OS_FN int tk_sc_no_net_filter(const TkScArch *a, TkBpf f[TK_SC_FILTER_LEN]) {
    const uint32_t deny = TK_SC_RET_ERRNO | (uint32_t)(EACCES & 0xFFFF);
#define TK_I(i, c, t, e, kk) (f[i].code = (uint16_t)(c), f[i].jt = (t), f[i].jf = (e), f[i].k = (kk))
    TK_I(0,  TK_BPF_LD_W_ABS, 0, 0, TK_SC_OFF_ARCH);
    TK_I(1,  TK_BPF_JEQ_K, 0, 5, a->arch);                       /* -> 2, else compat 7 */
    TK_I(2,  TK_BPF_LD_W_ABS, 0, 0, TK_SC_OFF_NR);
    if (a->x32_bit) TK_I(3, TK_BPF_JGE_K, 13, 0, a->x32_bit);   /* -> deny 17 */
    else            TK_I(3, TK_BPF_JEQ_K, 13, 0, TK_SC_NONE);
    TK_I(4,  TK_BPF_JEQ_K, 12, 0, a->nr_io_uring);               /* -> deny 17 */
    TK_I(5,  TK_BPF_JEQ_K, 7, 0, a->nr_socket);                  /* -> domain 13 */
    TK_I(6,  TK_BPF_RET_K, 0, 0, TK_SC_RET_ALLOW);
    TK_I(7,  TK_BPF_JEQ_K, 0, 10, a->compat_arch);               /* -> 8, else kill 18 */
    TK_I(8,  TK_BPF_LD_W_ABS, 0, 0, TK_SC_OFF_NR);
    TK_I(9,  TK_BPF_JEQ_K, 7, 0, a->c_socketcall);               /* -> deny 17 */
    TK_I(10, TK_BPF_JEQ_K, 6, 0, a->c_io_uring);                 /* -> deny 17 */
    TK_I(11, TK_BPF_JEQ_K, 1, 0, a->c_socket);                   /* -> domain 13 */
    TK_I(12, TK_BPF_RET_K, 0, 0, TK_SC_RET_ALLOW);
    TK_I(13, TK_BPF_LD_W_ABS, 0, 0, a->arg0);                    /* the domain */
    TK_I(14, TK_BPF_JEQ_K, 2, 0, 2u);                            /* AF_INET -> deny 17 */
    TK_I(15, TK_BPF_JEQ_K, 1, 0, TK_OS_AF_INET6);                /* AF_INET6 -> deny 17 */
    TK_I(16, TK_BPF_RET_K, 0, 0, TK_SC_RET_ALLOW);
    TK_I(17, TK_BPF_RET_K, 0, 0, deny);
    TK_I(18, TK_BPF_RET_K, 0, 0, TK_SC_RET_KILL_PROCESS);
#undef TK_I
    return TK_SC_FILTER_LEN;
}

/* ── Apply ──────────────────────────────────────────────────────────── */

#if defined(__APPLE__)

/* libsystem_sandbox: exported, documented in sandbox_init(3), deprecated in
 * <sandbox.h> but working, and needing no entitlement.  Declared here, not
 * through <sandbox.h>, so the deprecation is not a -Werror failure.  A
 * profile string is passed with flags 0. */
int  sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
void sandbox_free_error(char *errorbuf);

TK_OS_FN void tk_os_apply(const TkOsPlan *p, TkOsWarn warn) {
    TkOsBuf b = {0};
    tk_os_sbpl(p, &b);
    if (b.oom || !b.p) { free(b.p); warn("sandbox", "out of memory building the profile"); return; }
    if (getenv("TK_CAP_OS_SHOW")) fputs(b.p, stderr);
    char *err = NULL;
    if (sandbox_init(b.p, 0, &err) != 0) {
        char why[512];
        snprintf(why, sizeof why, "sandbox_init refused the profile: %s", err ? err : "unknown error");
        if (err) sandbox_free_error(err);
        warn("sandbox", why);
    }
    free(b.p);
}

#elif defined(__linux__)

#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
/* The Landlock syscalls have one number on every architecture. */
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif

/* The uapi structs, copied so the build does not need a new enough
 * <linux/landlock.h> (net rules are ABI 4, Linux 6.7). */
struct tk_ll_ruleset_attr { uint64_t handled_access_fs; uint64_t handled_access_net; };
struct tk_ll_path_beneath { uint64_t allowed_access; int32_t parent_fd; } __attribute__((packed));
struct tk_ll_net_port     { uint64_t allowed_access; uint64_t port; };
#define TK_LL_RULE_PATH_BENEATH 1
#define TK_LL_RULE_NET_PORT     2
#define TK_LL_CREATE_RULESET_VERSION 1u

/* Add one path rule: 1 added (or nothing to add), 0 the path does not
 * exist (nothing to admit), -1 landlock_add_rule refused it. */
TK_OS_FN int tk_ll_add_path(int rs, const char *path, uint64_t rights) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) rights &= TK_LL_FILE_RIGHTS;
    struct tk_ll_path_beneath pb;
    pb.allowed_access = rights;
    pb.parent_fd = fd;
    int ok = rights == 0 ||
             syscall(SYS_landlock_add_rule, rs, TK_LL_RULE_PATH_BENEATH, &pb, 0) == 0;
    close(fd);
    return ok ? 1 : -1;
}

/* A write scope that does not exist yet (a cache directory the program
 * creates): Landlock needs an existing directory, so the rule goes on the
 * nearest existing ancestor, which the CAP004 line names.  -1 as above. */
TK_OS_FN int tk_ll_add_write_scope(int rs, const char *path, uint64_t rights, TkOsWarn warn) {
    int r = tk_ll_add_path(rs, path, rights);
    if (r != 0) return r;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    for (;;) {
        char *slash = strrchr(dir, '/');
        if (!slash) return 1;
        if (slash == dir) slash[1] = '\0'; else *slash = '\0';
        r = tk_ll_add_path(rs, dir, rights);
        if (r < 0) return r;
        if (r > 0) {
            char why[4096 + 200];
            snprintf(why, sizeof why, "the fs.write scope %s does not exist yet; Landlock "
                     "admits writes beneath %s, its nearest existing directory", path, dir);
            warn("landlock", why);
            return 1;
        }
        if (!strcmp(dir, "/")) return 1;
    }
}

/* A rule the kernel refused would leave the ruleset narrower than the
 * grant: apply none of it (the broker still enforces every grant). */
TK_OS_FN void tk_ll_refused(int rs, const char *what, TkOsWarn warn) {
    char why[4096 + 200];
    snprintf(why, sizeof why, "landlock_add_rule refused the rule for %s: %s; no Landlock "
             "rules were applied, so file-system, execute and TCP scopes are enforced by the "
             "broker only", what, strerror(errno));
    warn("landlock", why);
    close(rs);
}

/* The path rules: 0, or -1 after tk_ll_refused (rs closed). */
TK_OS_FN int tk_ll_add_paths(int rs, const TkOsPlan *p, int abi, TkOsWarn warn) {
    struct { int on; const char *const *base; const TkOsPaths *s; uint64_t rights; int write; } k[3] = {
        { p->read.on,  tk_os_linux_read_base,  &p->read,  tk_ll_read_rights(),   0 },
        { p->write.on, tk_os_linux_write_base, &p->write, tk_ll_write_rights(abi), 1 },
        { p->exec.on,  tk_os_linux_exec_base,  &p->exec,  TK_LL_EXECUTE,        0 },
    };
    for (int j = 0; j < 3; j++) {
        if (!k[j].on) continue;
        for (int i = 0; k[j].base[i]; i++)
            if (tk_ll_add_path(rs, k[j].base[i], k[j].rights) < 0) {
                tk_ll_refused(rs, k[j].base[i], warn);
                return -1;
            }
        for (int i = 0; i < k[j].s->n; i++) {
            const char *v = k[j].s->v[i];
            int r = k[j].write ? tk_ll_add_write_scope(rs, v, k[j].rights, warn)
                               : tk_ll_add_path(rs, v, k[j].rights);
            if (r < 0) { tk_ll_refused(rs, v, warn); return -1; }
        }
    }
    return 0;
}

TK_OS_FN void tk_ll_apply(const TkOsPlan *p, TkOsWarn warn) {
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, TK_LL_CREATE_RULESET_VERSION);
    if (abi < 0) {
        warn("landlock", errno == EOPNOTSUPP ? "Landlock is disabled on this kernel (boot "
             "parameter lsm=); file-system, execute and TCP scopes are enforced by the broker only"
             : "this kernel has no Landlock (Linux 5.13 or later); file-system, execute and TCP "
               "scopes are enforced by the broker only");
        return;
    }
    if (abi < 4 && ((p->bind.on && !tk_os_ports_any(&p->bind)) ||
                    (p->connect.on && !tk_os_ports_any(&p->connect))))
        warn("landlock", "this kernel's Landlock has no TCP rules (ABI 4, Linux 6.7); "
             "connect and listen scopes are enforced by the broker only");
    if (getenv("TK_CAP_OS_SHOW")) {
        TkOsBuf b = {0};
        tk_ll_describe(p, (int)abi, &b);
        if (b.p) fputs(b.p, stderr);
        free(b.p);
    }
    struct tk_ll_ruleset_attr ra;
    ra.handled_access_fs = tk_ll_handled_fs(p, (int)abi);
    ra.handled_access_net = tk_ll_handled_net(p, (int)abi);
    if (!ra.handled_access_fs && !ra.handled_access_net) return;
    int rs = (int)syscall(SYS_landlock_create_ruleset, &ra,
                          abi >= 4 ? sizeof ra : sizeof ra.handled_access_fs, 0);
    if (rs < 0) {
        char why[160];
        snprintf(why, sizeof why, "landlock_create_ruleset failed: %s", strerror(errno));
        warn("landlock", why);
        return;
    }
    if (tk_ll_add_paths(rs, p, (int)abi, warn) < 0) return;
    const TkOsPorts *ns[2] = { &p->bind, &p->connect };
    uint64_t nb[2] = { TK_LL_NET_BIND, TK_LL_NET_CONNECT };
    for (int j = 0; j < 2; j++) {
        if (!(ra.handled_access_net & nb[j])) continue;
        for (int i = 0; i < ns[j]->n; i++) {
            struct tk_ll_net_port np;
            np.allowed_access = nb[j];
            np.port = (uint64_t)ns[j]->v[i].port;
            if (syscall(SYS_landlock_add_rule, rs, TK_LL_RULE_NET_PORT, &np, 0) != 0) {
                char what[64];
                snprintf(what, sizeof what, "%s port %ld", j ? "connect" : "bind", ns[j]->v[i].port);
                tk_ll_refused(rs, what, warn);
                return;
            }
        }
    }
    if (syscall(SYS_landlock_restrict_self, rs, 0) != 0) {
        char why[160];
        snprintf(why, sizeof why, "landlock_restrict_self failed: %s", strerror(errno));
        warn("landlock", why);
    }
    close(rs);
}

/* The seccomp filter's architecture (tk_sc_no_net_filter). */
#if defined(__x86_64__)
#define TK_SC_ARCH (&tk_sc_x86_64)
#define TK_SC_NR_SOCKET 41
#elif defined(__aarch64__)
#define TK_SC_ARCH (&tk_sc_aarch64)
#define TK_SC_NR_SOCKET 198
#endif

TK_OS_FN void tk_sc_apply_no_net(TkOsWarn warn) {
#ifdef TK_SC_ARCH
    /* The neutral filter hard-codes these; a libc that disagrees fails the build. */
    typedef char tk_sc_check_layout[(offsetof(struct seccomp_data, nr) == TK_SC_OFF_NR &&
        offsetof(struct seccomp_data, arch) == TK_SC_OFF_ARCH &&
        offsetof(struct seccomp_data, args) == TK_SC_OFF_ARG0 &&
        sizeof(struct sock_filter) == sizeof(TkBpf) && AF_INET == 2 &&
        AF_INET6 == TK_OS_AF_INET6 && __NR_socket == TK_SC_NR_SOCKET) ? 1 : -1]
        __attribute__((unused));
    TkScArch a = *TK_SC_ARCH;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    a.arg0 += 4;
#endif
    TkBpf t[TK_SC_FILTER_LEN];
    struct sock_filter f[TK_SC_FILTER_LEN];
    int n = tk_sc_no_net_filter(&a, t);
    for (int i = 0; i < n; i++) {
        f[i].code = t[i].code; f[i].jt = t[i].jt; f[i].jf = t[i].jf; f[i].k = t[i].k;
    }
    struct sock_fprog prog;
    prog.len = (unsigned short)n;
    prog.filter = f;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0) {
        char why[160];
        snprintf(why, sizeof why, "the seccomp socket filter was refused: %s; IPv4/IPv6 "
                 "sockets are refused by the broker only", strerror(errno));
        warn("seccomp", why);
    }
#else
    warn("seccomp", "no seccomp socket filter for this architecture; IPv4/IPv6 sockets "
         "are refused by the broker only");
#endif
}

TK_OS_FN void tk_os_apply(const TkOsPlan *p, TkOsWarn warn) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        char why[160];
        snprintf(why, sizeof why, "prctl(PR_SET_NO_NEW_PRIVS) failed: %s; no OS layer applied",
                 strerror(errno));
        warn("landlock", why);
        return;
    }
    tk_ll_apply(p, warn);
    if (p->net_none) tk_sc_apply_no_net(warn);
}

#else

TK_OS_FN void tk_os_apply(const TkOsPlan *p, TkOsWarn warn) {
    (void)p;
    warn("none", "this platform has no OS-level enforcement in toke (macOS sandbox and Linux "
         "Landlock/seccomp only); every grant is enforced by the broker only");
}

#endif /* platform */

#endif /* TK_CAP_OS_H */
