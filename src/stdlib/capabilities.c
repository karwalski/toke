/* SPDX-License-Identifier: MIT */
/*
 * capabilities.c — deny-by-default capability broker (ADR-0010, Epic 124.4a;
 * scoped grants 138.19).  See capabilities.h for the model.  This is the
 * runtime half: broker state, the class-only and scoped gates, and the
 * runtime --allow-* flag parse.  The compiler links it too, for
 * tk_cap_scope_syntax (manifest and --allow-* validation, src/config.c).
 *
 * 138.19 (D1-D3 of docs/decisions/138.19-scoped-capability-grants.md):
 *   - each class holds, per source (baked / run time), "whole class" or a
 *     list of scopes, parsed and canonicalised ONCE here, never on the hot
 *     path; a scoped check is a linear scan of a manifest-sized list, once
 *     per acquisition;
 *   - a whole-class grant answers before any canonicalisation, so `true`,
 *     a bare --allow-x and --allow-all behave exactly as before;
 *   - paths: realpath() of the longest existing prefix plus the components
 *     that do not exist yet, compared component by component (§3.1);
 *   - hosts: the name the program passed, before DNS; IP and CIDR scopes
 *     match IP literals only (§3.2);
 *   - commands: argv[0] resolved as process.c resolves it (the PATH captured
 *     at start-up) and compared by canonical path; a bare-name grant is
 *     resolved once, here, and pinned (§3.3);
 *   - D2: with a baked grant set, run-time flags narrow it (both must admit),
 *     unless the manifest said runtime = "union".
 */
#include "capabilities.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef PATH_MAX
#define PATH_MAX 4096   /* POSIX minimum is 256; 4096 is Linux's, and the larger is the safe bound */
#endif

/* Weak baked-grant defaults: absent, no grants, no scopes, legacy union.
 * The compiler (124.4b, 138.19) emits strong overrides when a program
 * declares grants.  A binary built before 138.19 has no scope table and no
 * runtime mode, so it behaves exactly as before. */
__attribute__((weak)) const uint32_t __tk_cap_baked_grants  = 0u;
__attribute__((weak)) const int      __tk_cap_baked_present = 0;
__attribute__((weak)) const int      __tk_cap_baked_enforce = 0;
__attribute__((weak)) const TkCapBakedScope *const __tk_cap_baked_scopes = NULL;
__attribute__((weak)) const int      __tk_cap_baked_nscopes = 0;
__attribute__((weak)) const int      __tk_cap_baked_runtime = TK_CAP_RUNTIME_LEGACY;

/* ── Scope and grant state ────────────────────────────────────────── */

enum { HK_NAME = 1, HK_WILD, HK_IP4, HK_IP6, HK_CIDR4, HK_CIDR6 };

typedef struct {
    int   kind;            /* TK_CAP_SK_* */
    int   dead;            /* could not be canonicalised: admits nothing */
    char *text;            /* canonical path / command; env key without '*'; net host */
    int   hk;              /* HK_*, net only */
    unsigned char addr[16];
    int   prefix;          /* CIDR length */
    long  port;            /* -1: any port */
    int   wild;            /* env: the key had a trailing '*' */
} Scope;

typedef struct {
    int    named;          /* this source mentions the class at all */
    int    whole;          /* the whole class */
    int    n, cap;
    Scope *v;
} Grant;

#define SRC_BAKED 0
#define SRC_RUN   1
static Grant g_src[2][TK_CAP_NCLASSES];

/* 124.4g: deny-by-default. The mode with no baked grants and no runtime flags is
 * ENFORCE — a program has no fs/net/env/process authority unless it is granted.
 * Only an explicit --allow-all (baked or runtime) opts back into ALLOW_ALL. */
static TkCapMode g_mode   = TK_CAP_MODE_ENFORCE;
static int       g_inited = 0;
static int       g_narrow = 0;     /* 138.19 D2: run-time flags intersect the baked set */
static char     *g_path0  = NULL;  /* PATH at start-up: process.c spawns through the same snapshot */

static const char *const k_dot[TK_CAP_NCLASSES]  =
    { "fs.read", "fs.write", "net", "env.write", "process.spawn" };
static const char *const k_key[TK_CAP_NCLASSES]  =
    { "fs_read", "fs_write", "net", "env_write", "process_spawn" };
static const char *const k_flag[TK_CAP_NCLASSES] =
    { "--allow-read", "--allow-write", "--allow-net", "--allow-env", "--allow-run" };

/* The class index of a TK_CAP_* value (a class bit, or a net direction). */
static int class_index(uint32_t cap) {
    if (cap & TK_CAP_FS_READ)       return TK_CAP_IDX_FS_READ;
    if (cap & TK_CAP_FS_WRITE)      return TK_CAP_IDX_FS_WRITE;
    if (cap & TK_CAP_NET)           return TK_CAP_IDX_NET;
    if (cap & TK_CAP_ENV_WRITE)     return TK_CAP_IDX_ENV_WRITE;
    if (cap & TK_CAP_PROCESS_SPAWN) return TK_CAP_IDX_PROCESS_SPAWN;
    return -1;
}

static uint32_t class_bit(int ci) { return 1u << ci; }

/* ── Scope syntax (shared with the compiler) ──────────────────────── */

static void seterr(char *err, unsigned errsz, const char *msg) {
    if (err && errsz) snprintf(err, errsz, "%s", msg);
}

/* A port: decimal, or `*` (any).  0 is a port only for listen (ephemeral). */
static int parse_port(const char *s, int allow_zero, long *out) {
    if (!strcmp(s, "*")) { *out = -1; return 1; }
    if (!*s) return 0;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        v = v * 10 + (*p - '0');
        if (v > 65535) return 0;
    }
    if (v == 0 && !allow_zero) return 0;
    *out = v;
    return 1;
}

/* A DNS name: dot-separated labels of [A-Za-z0-9_-], 1..63 each, <= 253. */
static int valid_dns(const char *h) {
    size_t n = strlen(h), lab = 0;
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c == '.') { if (lab == 0) return 0; lab = 0; continue; }
        if (!(isalnum(c) || c == '-' || c == '_')) return 0;
        if (++lab > 63) return 0;
    }
    return lab > 0;
}

/* Lowercase `in` into out, dropping [] around an IPv6 literal and one
 * trailing dot.  Returns 0 when it does not fit. */
static int norm_host(const char *in, char *out, size_t outsz) {
    size_t n = strlen(in);
    if (n >= 2 && in[0] == '[' && in[n - 1] == ']') { in++; n -= 2; }
    if (n > 1 && in[n - 1] == '.') n--;
    if (n == 0 || n >= outsz) return 0;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)in[i]);
    out[n] = '\0';
    return 1;
}

/* Classify a host (no brackets): IP, CIDR, *.wildcard or DNS name. */
static int parse_hostpart(const char *h, int lis, Scope *s, char *err, unsigned errsz) {
    char buf[256];
    if (!norm_host(h, buf, sizeof buf)) { seterr(err, errsz, "empty or over-long host"); return 0; }
    const char *slash = strchr(buf, '/');
    if (slash) {
        char ip[256];
        size_t al = (size_t)(slash - buf);
        memcpy(ip, buf, al); ip[al] = '\0';
        char *end = NULL;
        long len = strtol(slash + 1, &end, 10);
        if (!slash[1] || *end || lis) {
            seterr(err, errsz, lis ? "a listen address is one IP literal, not a CIDR block"
                                      : "a CIDR block is ADDRESS/LENGTH");
            return 0;
        }
        if (inet_pton(AF_INET, ip, s->addr) == 1 && len >= 0 && len <= 32) s->hk = HK_CIDR4;
        else if (inet_pton(AF_INET6, ip, s->addr) == 1 && len >= 0 && len <= 128) s->hk = HK_CIDR6;
        else { seterr(err, errsz, "a CIDR block is an IPv4 or IPv6 address and a prefix length"); return 0; }
        s->prefix = (int)len;
    } else if (inet_pton(AF_INET, buf, s->addr) == 1) {
        s->hk = HK_IP4;
    } else if (inet_pton(AF_INET6, buf, s->addr) == 1) {
        s->hk = HK_IP6;
    } else if (lis) {
        seterr(err, errsz, "a listen address is an IP literal (127.0.0.1, 0.0.0.0, [::1])");
        return 0;
    } else if (buf[0] == '*' && buf[1] == '.' && valid_dns(buf + 2)) {
        s->hk = HK_WILD;
    } else if (valid_dns(buf)) {
        s->hk = HK_NAME;
    } else {
        seterr(err, errsz, "a host is a DNS name, *.domain, an IP literal or a CIDR block");
        return 0;
    }
    s->text = strdup(s->hk == HK_WILD ? buf + 1 : buf);   /* the wildcard keeps ".domain" */
    if (!s->text) { seterr(err, errsz, "out of memory"); return 0; }
    return 1;
}

/* connect:HOST[:PORT] / listen:ADDR:PORT, the prefix already removed. */
static int parse_net(const char *in, int lis, Scope *s, char *err, unsigned errsz) {
    char host[300], port[16] = "";
    const char *pstart = NULL;
    if (in[0] == '[') {                              /* [IPv6] or [IPv6/len] */
        const char *rb = strchr(in, ']');
        if (!rb || (size_t)(rb - in) >= sizeof host) { seterr(err, errsz, "unterminated [IPv6] host"); return 0; }
        memcpy(host, in + 1, (size_t)(rb - in - 1)); host[rb - in - 1] = '\0';
        if (rb[1] == ':') pstart = rb + 2;
        else if (rb[1]) { seterr(err, errsz, "expected :PORT after ]"); return 0; }
    } else {
        const char *c1 = strchr(in, ':');
        if (c1 && strchr(c1 + 1, ':')) {             /* bare IPv6, no port */
            if (strlen(in) >= sizeof host) { seterr(err, errsz, "host too long"); return 0; }
            strcpy(host, in);
        } else {
            size_t hl = c1 ? (size_t)(c1 - in) : strlen(in);
            if (hl >= sizeof host) { seterr(err, errsz, "host too long"); return 0; }
            memcpy(host, in, hl); host[hl] = '\0';
            if (c1) pstart = c1 + 1;
        }
    }
    if (pstart) {
        if (strlen(pstart) >= sizeof port || !parse_port(pstart, lis, &s->port)) {
            seterr(err, errsz, lis ? "a listen port is 0-65535 or *"
                                      : "a connect port is 1-65535 or *");
            return 0;
        }
    } else if (lis) {
        seterr(err, errsz, "a listen scope needs an explicit address and port: listen:ADDR:PORT");
        return 0;
    } else {
        s->port = -1;                                /* an omitted connect port: any */
    }
    return parse_hostpart(host, lis, s, err, errsz);
}

/* Parse `text` as a scope of class ci into s (net hosts parsed, paths and
 * commands NOT yet canonicalised; s->text then holds a copy of the text).
 * Returns the kind, or 0 with err set. */
static int parse_scope(int ci, const char *text, Scope *s, char *err, unsigned errsz) {
    memset(s, 0, sizeof *s);
    s->port = -1;
    if (!text || !*text) { seterr(err, errsz, "an empty scope"); return 0; }
    switch (ci) {
    case TK_CAP_IDX_FS_READ: case TK_CAP_IDX_FS_WRITE: case TK_CAP_IDX_PROCESS_SPAWN:
        if (text[0] == '~' && text[1] && text[1] != '/') {
            seterr(err, errsz, "~user is not expanded; write ~/ or the full path");
            return 0;
        }
        s->kind = ci == TK_CAP_IDX_PROCESS_SPAWN ? TK_CAP_SK_CMD : TK_CAP_SK_PATH;
        break;
    case TK_CAP_IDX_NET:
        if (!strncmp(text, "connect:", 8)) {
            s->kind = TK_CAP_SK_CONNECT;
            return parse_net(text + 8, 0, s, err, errsz) ? s->kind : 0;
        }
        if (!strncmp(text, "listen:", 7)) {
            s->kind = TK_CAP_SK_LISTEN;
            return parse_net(text + 7, 1, s, err, errsz) ? s->kind : 0;
        }
        seterr(err, errsz, "a net scope is connect:HOST[:PORT] or listen:ADDR:PORT");
        return 0;
    case TK_CAP_IDX_ENV_WRITE: {
        const char *star = strchr(text, '*');
        if (strchr(text, '=')) { seterr(err, errsz, "an environment key has no '='"); return 0; }
        if (star && star[1]) { seterr(err, errsz, "'*' is allowed only as the last character (a prefix)"); return 0; }
        s->kind = TK_CAP_SK_ENV;
        s->wild = star != NULL;
        break;
    }
    default:
        seterr(err, errsz, "not a capability class");
        return 0;
    }
    s->text = strdup(text);
    if (!s->text) { seterr(err, errsz, "out of memory"); return 0; }
    if (s->kind == TK_CAP_SK_ENV && s->wild) s->text[strlen(s->text) - 1] = '\0';
    return s->kind;
}

int tk_cap_scope_syntax(int cls, const char *text, char *err, unsigned errsz) {
    Scope s;
    int k = parse_scope(cls, text, &s, err, errsz);
    free(s.text);
    return k;
}

/* ── Canonicalisation (§3.1, §3.3) ────────────────────────────────── */

/* The canonical absolute form of `in` (PATH_MAX buffer): realpath() of its
 * longest existing prefix, then the components that do not exist yet, with
 * `.` dropped and `..` taken lexically.  A component that EXISTS but does not
 * resolve (a dangling symlink, a loop) fails: a symlink must never pass the
 * check by pointing at something not yet created.  `~` expands from HOME only
 * when expand_tilde (grants; a program's own "~/x" is a relative path). */
static int canon_path(const char *in, int expand_tilde, char *out) {
    char abs[PATH_MAX];
    if (!in || !*in) return 0;
    if (expand_tilde && in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
        const char *home = getenv("HOME");
        if (!home || !*home) return 0;
        if (snprintf(abs, sizeof abs, "%s%s", home, in + 1) >= (int)sizeof abs) return 0;
    } else if (in[0] == '/') {
        if (snprintf(abs, sizeof abs, "%s", in) >= (int)sizeof abs) return 0;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        if (snprintf(abs, sizeof abs, "%s/%s", cwd, in) >= (int)sizeof abs) return 0;
    }
    /* Components of abs (empty and "." dropped; ".." kept for realpath). */
    char *comp[PATH_MAX / 2];
    int nc = 0;
    for (char *p = abs; *p; ) {
        while (*p == '/') *p++ = '\0';
        if (!*p) break;
        comp[nc] = p;
        while (*p && *p != '/') p++;
        if (strcmp(comp[nc], ".") != 0) nc++;
    }
    /* Longest prefix that realpath resolves. */
    char head[PATH_MAX], res[PATH_MAX];
    int k = nc;
    for (; k >= 0; k--) {
        size_t hl = 0;
        head[0] = '\0';
        for (int i = 0; i < k; i++) {
            int w = snprintf(head + hl, sizeof head - hl, "/%s", comp[i]);
            if (w < 0 || (size_t)w >= sizeof head - hl) return 0;
            hl += (size_t)w;
        }
        if (k == 0) snprintf(head, sizeof head, "/");
        if (realpath(head, res)) break;
        if (k == 0) return 0;
    }
    if (k < nc) {
        /* comp[k] did not resolve: it must not exist at all. */
        char probe[PATH_MAX];
        struct stat st;
        if (snprintf(probe, sizeof probe, "%s/%s", strcmp(res, "/") ? res : "", comp[k])
                >= (int)sizeof probe) return 0;
        if (lstat(probe, &st) == 0) return 0;
    }
    size_t ol = strlen(res);
    if (ol >= PATH_MAX) return 0;
    memcpy(out, res, ol + 1);
    for (int i = k; i < nc; i++) {
        if (!strcmp(comp[i], "..")) {
            char *sl = strrchr(out, '/');
            if (sl && sl != out) *sl = '\0'; else snprintf(out, PATH_MAX, "/");
            continue;
        }
        ol = strlen(out);
        int w = snprintf(out + ol, PATH_MAX - ol, "%s%s", (ol == 1) ? "" : "/", comp[i]);
        if (w < 0 || (size_t)w >= PATH_MAX - ol) return 0;
    }
    return 1;
}

/* argv[0] as process.c's exec_no_ambient_path resolves it: a name with a `/`
 * is a path; a bare name is searched on the PATH captured at start-up, first
 * regular executable file wins.  Empty PATH entries are skipped, as that
 * function's strtok_r skips them (it never searches "." for one). */
static int resolve_cmd(const char *argv0, int expand_tilde, char *out) {
    if (!argv0 || !*argv0) return 0;
    if (strchr(argv0, '/') || (expand_tilde && argv0[0] == '~'))
        return canon_path(argv0, expand_tilde, out);
    const char *path = g_path0 ? g_path0 : "/usr/bin:/bin";
    const char *p = path;
    for (;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        char full[PATH_MAX];
        struct stat st;
        if (dl && snprintf(full, sizeof full, "%.*s/%s", (int)dl, p, argv0) < (int)sizeof full &&
            access(full, X_OK) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode))
            return canon_path(full, 0, out);
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* ── Grant tables ─────────────────────────────────────────────────── */

static int grant_push(Grant *g, const Scope *s) {
    if (g->n == g->cap) {
        int nc = g->cap ? g->cap * 2 : 4;
        Scope *v = (Scope *)realloc(g->v, (size_t)nc * sizeof *v);
        if (!v) return 0;
        g->v = v; g->cap = nc;
    }
    g->v[g->n++] = *s;
    return 1;
}

/* Add `text` as a scope of class ci to source src.  A scope that does not
 * parse or canonicalise stays in the list, dead: the class is still granted
 * for scopes (CAP002, not CAP001) and it admits nothing. */
static int add_scope(int src, int ci, const char *text, char *err, unsigned errsz) {
    Grant *g = &g_src[src][ci];
    Scope s;
    g->named = 1;
    if (!parse_scope(ci, text, &s, err, errsz)) {
        memset(&s, 0, sizeof s);
        s.dead = 1;
        grant_push(g, &s);
        return 0;
    }
    if (s.kind == TK_CAP_SK_PATH || s.kind == TK_CAP_SK_CMD) {
        char buf[PATH_MAX];
        int ok = s.kind == TK_CAP_SK_PATH ? canon_path(s.text, 1, buf)
                                           : resolve_cmd(s.text, 1, buf);
        free(s.text);
        s.text = ok ? strdup(buf) : NULL;
        if (!s.text) s.dead = 1;
    }
    grant_push(g, &s);
    return 1;
}

/* ── Requests and matching ────────────────────────────────────────── */

typedef struct {
    int  kind;                 /* TK_CAP_SK_* */
    int  ok;                   /* canonicalised / parsed */
    const char *raw;
    char canon[PATH_MAX];      /* path / command; env key; net host */
    int  fam;                  /* 4 / 6 when the host is an IP literal */
    unsigned char addr[16];
    long port;
} Req;

static int prefix_eq(const unsigned char *a, const unsigned char *b, int bits) {
    int full = bits / 8, rem = bits % 8;
    if (memcmp(a, b, (size_t)full) != 0) return 0;
    if (!rem) return 1;
    unsigned char m = (unsigned char)(0xFFu << (8 - rem));
    return (a[full] & m) == (b[full] & m);
}

static int path_under(const char *req, const char *grant) {
    size_t gl = strlen(grant);
    if (gl == 1 && grant[0] == '/') return 1;
    return strncmp(req, grant, gl) == 0 && (req[gl] == '\0' || req[gl] == '/');
}

static int scope_match(const Scope *s, const Req *q) {
    if (s->dead || s->kind != q->kind || !q->ok) return 0;
    switch (s->kind) {
    case TK_CAP_SK_PATH: return path_under(q->canon, s->text);
    case TK_CAP_SK_CMD:  return strcmp(q->canon, s->text) == 0;
    case TK_CAP_SK_ENV:
        return s->wild ? strncmp(q->raw, s->text, strlen(s->text)) == 0
                       : strcmp(q->raw, s->text) == 0;
    case TK_CAP_SK_CONNECT: case TK_CAP_SK_LISTEN:
        if (s->port != -1 && s->port != q->port) return 0;
        switch (s->hk) {
        case HK_NAME:  return !q->fam && strcmp(q->canon, s->text) == 0;
        case HK_WILD: {
            size_t ql = strlen(q->canon), sl = strlen(s->text);   /* s->text is ".domain" */
            return !q->fam && ql > sl && strcmp(q->canon + ql - sl, s->text) == 0;
        }
        case HK_IP4:   return q->fam == 4 && memcmp(q->addr, s->addr, 4) == 0;
        case HK_IP6:   return q->fam == 6 && memcmp(q->addr, s->addr, 16) == 0;
        case HK_CIDR4: return q->fam == 4 && prefix_eq(q->addr, s->addr, s->prefix);
        case HK_CIDR6: return q->fam == 6 && prefix_eq(q->addr, s->addr, s->prefix);
        default:       return 0;
        }
    default: return 0;
    }
}

static int admits(const Grant *g, const Req *q) {
    if (g->whole) return 1;
    for (int i = 0; i < g->n; i++)
        if (scope_match(&g->v[i], q)) return 1;
    return 0;
}

/* The whole class, effectively (baked and run time combined). */
static int eff_whole(int ci) {
    if (g_mode == TK_CAP_MODE_ALLOW_ALL) return 1;
    const Grant *b = &g_src[SRC_BAKED][ci], *r = &g_src[SRC_RUN][ci];
    if (g_narrow) return b->whole && (!r->named || r->whole);
    return b->whole || r->whole;
}

/* The class granted in any form (whole or for scopes). */
static int eff_granted(int ci) {
    if (g_mode == TK_CAP_MODE_ALLOW_ALL) return 1;
    const Grant *b = &g_src[SRC_BAKED][ci], *r = &g_src[SRC_RUN][ci];
    int gb = b->whole || b->n > 0, gr = r->whole || r->n > 0;
    if (g_narrow) return gb && (!r->named || gr);
    return gb || gr;
}

/* ── Diagnostics ──────────────────────────────────────────────────── */

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", (unsigned)*p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

/* The --allow-* flag that grants a given class (for the diagnostic hint). */
static const char *cap_flag_hint(uint32_t cap) {
    int ci = class_index(cap);
    return ci >= 0 ? k_flag[ci] : "--allow-all";
}

/* CAP001 as before 138.19, byte for byte, when run-time flags can add the
 * class.  On a binary whose baked set they may only narrow (D2), the run-time
 * half of the advice is wrong, so the hint and fix name the manifest alone --
 * and the fix is left out when a run-time flag scopes the class, since it
 * would still narrow the new grant. */
static void deny_class(uint32_t cap) {
    int ci = class_index(cap);
    const char *cls = tk_cap_class_name(cap);
    const char *flag = cap_flag_hint(cap);
    if (!g_narrow || ci < 0) {
        fprintf(stderr,
            "CAP001: capability '%s' required but not granted\n"
            "  grant it at run time with %s, or declare it in tkc.toml [capabilities]\n",
            cls, flag);
        if (getenv("TK_DIAG_JSON"))
            fprintf(stderr,
                "{\"diagnostic_id\":\"CAP001\",\"severity\":\"error\",\"stage\":\"runtime\","
                "\"capability\":\"%s\",\"fix\":\"grant %s or tkc.toml [capabilities] %s=true\"}\n",
                cls, flag, cls);
        exit(1);
    }
    fprintf(stderr,
        "CAP001: capability '%s' required but not granted\n"
        "  this binary's grants are baked, and run-time flags can narrow them, not add to them:\n"
        "  declare it in tkc.toml [capabilities] (%s = true, or a scope list) and rebuild\n",
        cls, k_key[ci]);
    if (getenv("TK_DIAG_JSON")) {
        fprintf(stderr, "{\"diagnostic_id\":\"CAP001\",\"severity\":\"error\",\"stage\":\"runtime\","
                        "\"capability\":\"%s\"", cls);
        if (!g_src[SRC_RUN][ci].named || g_src[SRC_RUN][ci].whole)
            fprintf(stderr, ",\"fix\":\"tkc.toml [capabilities] %s = true\"", k_key[ci]);
        fputs("}\n", stderr);
    }
    exit(1);
}

/* CAP002 (D3): the class is granted, but not for this scope.  `scope` is
 * the canonical scope that would admit the call (or the raw value, when it
 * could not be canonicalised: canon_ok = 0).  The fix names exactly that
 * scope in the one place that would admit it:
 *   - a run-time flag, when run-time flags union (no baked set, or
 *     runtime = "union") or when only the run-time flags exclude it;
 *   - the manifest, when only the baked grant excludes it;
 * and is absent when both exclude it, or no single scope can be named. */
static void deny_scope(int ci, const char *scope, int canon_ok, int b, int r) {
    const Grant *rg = &g_src[SRC_RUN][ci];
    int where = 0;   /* 1 run-time flag, 2 manifest */
    if (canon_ok && tk_cap_scope_syntax(ci, scope, NULL, 0)) {
        if (!g_narrow || (b && !r)) where = 1;
        else if (!b && (!rg->named || r)) where = 2;
    }
    if (where == 1 && strchr(scope, ',')) where = 0;           /* the flag splits on ',' */
    if (where == 2) for (const char *p = scope; *p; p++)        /* a TOML basic string */
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) { where = 0; break; }

    fprintf(stderr, "CAP002: capability '%s' is granted, but not for '%s'\n", k_dot[ci], scope);
    if (where == 1)
        fprintf(stderr, "  grant this scope at run time with %s=%s\n", k_flag[ci], scope);
    else if (where == 2)
        fprintf(stderr, "  add it to tkc.toml [capabilities] and rebuild: %s = [\"%s\"]\n"
                        "  (run-time flags can narrow this binary's baked grants, not widen them)\n",
                k_key[ci], scope);
    else if (!canon_ok)
        fprintf(stderr, "  it does not resolve to a canonical %s (a dangling symlink, a loop, "
                        "a name not on PATH, or too long), so no scope admits it\n",
                ci == TK_CAP_IDX_PROCESS_SPAWN ? "command" : "scope");
    else
        fprintf(stderr, "  both the baked grant (tkc.toml [capabilities] %s) and the run-time "
                        "%s flags exclude it\n", k_key[ci], k_flag[ci]);
    if (getenv("TK_DIAG_JSON")) {
        fprintf(stderr, "{\"diagnostic_id\":\"CAP002\",\"severity\":\"error\",\"stage\":\"runtime\","
                        "\"capability\":\"%s\",\"scope\":", k_dot[ci]);
        json_str(stderr, scope);
        if (where) {
            char fix[PATH_MAX + 64];
            if (where == 1) snprintf(fix, sizeof fix, "%s=%s", k_flag[ci], scope);
            else            snprintf(fix, sizeof fix, "%s = [\"%s\"]", k_key[ci], scope);
            fputs(",\"fix\":", stderr);
            json_str(stderr, fix);
        }
        fputs("}\n", stderr);
    }
    exit(1);
}

/* 138.19 §3.4: a class-only gate reached while the class is granted for
 * scopes only.  It fails closed; no single scope can be named. */
static void deny_classonly(int ci) {
    fprintf(stderr,
        "CAP002: capability '%s' is granted, but only for listed scopes, and this std entry "
        "checks the whole class\n"
        "  (an entry the scoped gates do not cover yet fails closed rather than ignore the scopes)\n",
        k_dot[ci]);
    if (getenv("TK_DIAG_JSON"))
        fprintf(stderr, "{\"diagnostic_id\":\"CAP002\",\"severity\":\"error\",\"stage\":\"runtime\","
                        "\"capability\":\"%s\",\"scope\":\"\"}\n", k_dot[ci]);
    exit(1);
}

void tk_cap_deny(uint32_t cap) {
    int ci = class_index(cap);
    if (ci >= 0 && eff_granted(ci)) deny_classonly(ci);
    deny_class(cap);
}

/* Evaluate request q of class ci; returns only when it is admitted. */
static void require(int ci, const Req *q, const char *scope_text) {
    int b = admits(&g_src[SRC_BAKED][ci], q), r = admits(&g_src[SRC_RUN][ci], q);
    int ok = g_narrow ? (b && (!g_src[SRC_RUN][ci].named || r)) : (b || r);
    if (ok) return;
    deny_scope(ci, scope_text, q->ok, b, r);
}

/* ── Scoped gates ─────────────────────────────────────────────────── */

void tk_cap_require_path(uint32_t cap, const char *path) {
    int ci = class_index(cap);
    if (ci < 0 || eff_whole(ci)) return;
    if (!eff_granted(ci)) deny_class(class_bit(ci));
    Req q;
    memset(&q, 0, sizeof q);
    q.kind = TK_CAP_SK_PATH;
    q.raw  = path ? path : "";
    q.ok   = path && canon_path(path, 0, q.canon);
    require(ci, &q, q.ok ? q.canon : q.raw);
}

void tk_cap_require_net(uint32_t dir, const char *host, uint64_t port) {
    const int ci = TK_CAP_IDX_NET;
    if (eff_whole(ci)) return;
    if (!eff_granted(ci)) deny_class(TK_CAP_NET);
    int is_listen = (dir & TK_CAP_NET_LISTEN & ~TK_CAP_NET) != 0;
    Req q;
    memset(&q, 0, sizeof q);
    q.kind = is_listen ? TK_CAP_SK_LISTEN : TK_CAP_SK_CONNECT;
    q.raw  = host ? host : "";
    q.port = (long)port;
    q.ok   = host && port <= 65535 && norm_host(host, q.canon, sizeof q.canon);
    if (q.ok) {
        if (inet_pton(AF_INET, q.canon, q.addr) == 1) q.fam = 4;
        else if (inet_pton(AF_INET6, q.canon, q.addr) == 1) q.fam = 6;
        else if (is_listen) q.ok = 0;          /* a bind address is an IP literal */
    }
    char scope[PATH_MAX];
    if (q.ok)
        snprintf(scope, sizeof scope, q.fam == 6 ? "%s:[%s]:%ld" : "%s:%s:%ld",
                 is_listen ? "listen" : "connect", q.canon, q.port);
    else
        snprintf(scope, sizeof scope, "%s:%s:%llu", is_listen ? "listen" : "connect",
                 q.raw, (unsigned long long)port);
    require(ci, &q, scope);
}

void tk_cap_require_cmd(const char *argv0) {
    const int ci = TK_CAP_IDX_PROCESS_SPAWN;
    if (eff_whole(ci)) return;
    if (!eff_granted(ci)) deny_class(TK_CAP_PROCESS_SPAWN);
    Req q;
    memset(&q, 0, sizeof q);
    q.kind = TK_CAP_SK_CMD;
    q.raw  = argv0 ? argv0 : "";
    q.ok   = argv0 && resolve_cmd(argv0, 0, q.canon);
    require(ci, &q, q.ok ? q.canon : q.raw);
}

void tk_cap_require_env(const char *key) {
    const int ci = TK_CAP_IDX_ENV_WRITE;
    if (eff_whole(ci)) return;
    if (!eff_granted(ci)) deny_class(TK_CAP_ENV_WRITE);
    Req q;
    memset(&q, 0, sizeof q);
    q.kind = TK_CAP_SK_ENV;
    q.raw  = key ? key : "";
    q.ok   = key && *key;
    require(ci, &q, q.raw);
}

const char *tk_cap_ip4str(uint32_t s_addr_be, char buf[TK_CAP_ADDRSTRLEN]) {
    struct in_addr a;
    a.s_addr = s_addr_be;
    if (!inet_ntop(AF_INET, &a, buf, TK_CAP_ADDRSTRLEN)) return NULL;
    return buf;
}

const char *tk_cap_sockname(int fd, char buf[TK_CAP_ADDRSTRLEN], uint64_t *port) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    *port = 0;
    memset(&ss, 0, sizeof ss);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return NULL;
    if (ss.ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)&ss;
        *port = ntohs(a->sin_port);
        return inet_ntop(AF_INET, &a->sin_addr, buf, TK_CAP_ADDRSTRLEN);
    }
    if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&ss;
        *port = ntohs(a->sin6_port);
        return inet_ntop(AF_INET6, &a->sin6_addr, buf, TK_CAP_ADDRSTRLEN);
    }
    return NULL;
}

/* ── Start-up ─────────────────────────────────────────────────────── */

/*
 * Map one --allow-* flag to its class: TK_CAP_IDX_*, TK_CAP_NCLASSES for
 * --allow-all, -1 for anything else.  The name must end at '\0' or '=' (an
 * earlier prefix match took --allow-readers for --allow-read); *val is the
 * text after '=' or NULL.  Both --allow-read and --allow-fs-read spellings.
 */
static int flag_class(const char *arg, const char **val) {
    static const struct { const char *name; int ci; } names[] = {
        {"--allow-all", TK_CAP_NCLASSES},
        {"--allow-read", TK_CAP_IDX_FS_READ},   {"--allow-fs-read", TK_CAP_IDX_FS_READ},
        {"--allow-write", TK_CAP_IDX_FS_WRITE}, {"--allow-fs-write", TK_CAP_IDX_FS_WRITE},
        {"--allow-net", TK_CAP_IDX_NET},        {"--allow-env", TK_CAP_IDX_ENV_WRITE},
        {"--allow-run", TK_CAP_IDX_PROCESS_SPAWN},
    };
    *val = NULL;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n = strlen(names[i].name);
        if (strncmp(arg, names[i].name, n) != 0) continue;
        if (arg[n] == '\0') return names[i].ci;
        if (arg[n] == '=') { *val = arg + n + 1; return names[i].ci; }
    }
    return -1;
}

/* A run-time flag the broker cannot honour: one line, exit 1, before main. */
static void refuse(const char *flag, const char *why) {
    fprintf(stderr, "capabilities: %s refused: %s\n", flag, why);
    exit(1);
}

/* One --allow-x=a,b,c: every scope must parse. */
static void add_flag_scopes(int ci, const char *flag, const char *val) {
    char buf[PATH_MAX], err[160];
    const char *p = val;
    for (;;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof buf) refuse(flag, "a scope is too long");
        memcpy(buf, p, n); buf[n] = '\0';
        if (!add_scope(SRC_RUN, ci, buf, err, sizeof err)) {
            char why[PATH_MAX + 200];
            snprintf(why, sizeof why, "'%s' is not a %s scope: %s", buf, k_dot[ci], err);
            refuse(flag, why);
        }
        if (!e) break;
        p = e + 1;
    }
}

void tk_cap_init(int argc, char **argv) {
    if (g_inited) return;
    g_inited = 1;
    {
        const char *p = getenv("PATH");
        g_path0 = p ? strdup(p) : NULL;
    }

    /* The compiler-baked grant set (if the program declared one).  Post-flip
     * the mode is ENFORCE by default, so a baked allowlist simply enforces
     * those grants; __tk_cap_baked_enforce is vestigial. */
    int baked_all = 0;
    if (__tk_cap_baked_present) {
        uint32_t b = __tk_cap_baked_grants;
        baked_all = (b == TK_CAP_ALL);
        for (int ci = 0; ci < TK_CAP_NCLASSES; ci++)
            if (b & class_bit(ci)) { g_src[SRC_BAKED][ci].whole = 1; g_src[SRC_BAKED][ci].named = 1; }
        for (int i = 0; __tk_cap_baked_scopes && i < __tk_cap_baked_nscopes; i++) {
            const TkCapBakedScope *s = &__tk_cap_baked_scopes[i];
            if (s->cls >= 0 && s->cls < TK_CAP_NCLASSES)
                (void)add_scope(SRC_BAKED, s->cls, s->text, NULL, 0);
        }
        g_narrow = (__tk_cap_baked_runtime == TK_CAP_RUNTIME_NARROW);
    }

    /* Run-time --allow-* flags: unioned with the baked set, or (D2) narrowing
     * it.  --allow-all drops to permissive mode only where it widens nothing
     * it may not: never on a binary whose narrower set is baked. */
    int run_all = 0, run_any = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val;
        if (strncmp(a, "--allow-", 8) != 0) continue;
        int ci = flag_class(a, &val);
        if (ci < 0) continue;
        if (ci == TK_CAP_NCLASSES) {
            if (val) refuse(a, "--allow-all takes no value");
            run_all = 1;
            continue;
        }
        run_any = 1;
        g_src[SRC_RUN][ci].named = 1;
        if (!val) g_src[SRC_RUN][ci].whole = 1;
        else add_flag_scopes(ci, a, val);
    }
    if (run_all && g_narrow && !baked_all)
        refuse("--allow-all", "this binary's grants are baked (tkc.toml [capabilities] or "
               "tkc --allow-*), and run-time flags may only narrow them; rebuild with "
               "runtime = \"union\" to let a launcher widen them");
    if (g_narrow) {
        if (baked_all && !run_any) g_mode = TK_CAP_MODE_ALLOW_ALL;
    } else if (baked_all || run_all) {
        g_mode = TK_CAP_MODE_ALLOW_ALL;
    }
}

int tk_cap_is_grant_flag(const char *arg) {
    const char *val;
    return arg && strncmp(arg, "--allow-", 8) == 0 && flag_class(arg, &val) >= 0;
}

int tk_cap_check(uint32_t cap) {
    if (g_mode == TK_CAP_MODE_ALLOW_ALL) return 1;
    for (int ci = 0; ci < TK_CAP_NCLASSES; ci++)
        if ((cap & class_bit(ci)) && !eff_whole(ci)) return 0;
    return 1;
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

TkCapMode tk_cap_mode(void)  { return g_mode; }

uint32_t tk_cap_grants(void) {
    if (g_mode == TK_CAP_MODE_ALLOW_ALL) return TK_CAP_ALL;
    uint32_t g = 0;
    for (int ci = 0; ci < TK_CAP_NCLASSES; ci++)
        if (eff_granted(ci)) g |= class_bit(ci);
    return g;
}
