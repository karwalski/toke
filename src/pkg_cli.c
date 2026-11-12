/*
 * pkg_cli.c — `tkc pkg` (story 76.1.3b) and the build's pkg.* import path
 * (story 76.1.3c), built on the 76.1.3a resolver and lock (pkg.c) and the
 * git fetch (pkg_git.c, 76.1.3d).  Design: ADR-0004.
 *
 *   tkc pkg init [<name>]                      write pkg.toml
 *   tkc pkg add <name> <constraint> [--git U]  add or update a dependency
 *   tkc pkg remove <name>                      drop a dependency
 *   tkc pkg resolve                            MVS -> pkg.lock, no fetch
 *   tkc pkg fetch                              fetch, pin, verify, pkg.lock
 *   tkc pkg list                               direct and transitive deps
 *
 * Every command also takes --allow-file-source (ADR-0004's explicit
 * local-path override: file:/// sources are accepted for this run) and
 * --diag-text.  Every error is a structured diagnostic with a pkg.h code and
 * no `fix`.
 *
 * Package cache: $HOME/.toke/pkg/<name>/<version>/ holds the working tree of
 * the commit the lock pins (no .git); <version>.commit beside it records that
 * commit.  Neither is trusted: the tree is re-hashed against the lock on
 * every fetch and every build that uses it.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "pkg.h"
#include "pkg_cli.h"
#include "pkg_git.h"
#include "tkc_limits.h"

#define PKG_MANIFEST  "pkg.toml"
#define PKG_LOCKFILE  "pkg.lock"
#define PKG_PATH      4096
#define PKG_MAX_REQS  (PKG_MAX_DEPS * 8)
#define PKG_MSG       1024

/* ── Diagnostics ────────────────────────────────────────────────────────── */

/* "line N: ..." from a pkg.c error message, or 0. */
static int
err_line(const char *msg)
{
    int n = 0;
    if (msg && sscanf(msg, "line %d", &n) == 1 && n > 0) return n;
    return 0;
}

static void
pdiag(DiagSeverity sev, int code, const char *file, int line,
      const char *fmt, ...)
{
    char msg[PKG_MSG * 2];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_set_source_file(file ? file : "");
    diag_emit(sev, code, 0, line, line ? 1 : 0, msg, (const char *)NULL);
}

/* ── Small helpers ──────────────────────────────────────────────────────── */

static bool
file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool
dir_exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void
vfmt(PkgVersion v, char buf[PKG_MAX_VERSION])
{
    pkg_version_fmt(v, buf, PKG_MAX_VERSION);
}

/* Constraint text of a dependency, as pkg.toml spells it. */
static void
dep_constraint(const PkgDep *d, char *buf, size_t n)
{
    char a[PKG_MAX_VERSION], b[PKG_MAX_VERSION];
    vfmt(d->minimum, a);
    if (d->exact) { snprintf(buf, n, "%s", a); return; }
    if (d->has_below) {
        vfmt(d->below, b);
        snprintf(buf, n, ">=%s <%s", a, b);
        return;
    }
    snprintf(buf, n, ">=%s", a);
}

static bool
dep_admits(const PkgDep *d, PkgVersion v)
{
    if (d->exact) return pkg_version_cmp(v, d->minimum) == 0;
    if (pkg_version_cmp(v, d->minimum) < 0) return false;
    if (d->has_below && pkg_version_cmp(v, d->below) >= 0) return false;
    return true;
}

/* Read a whole small text file.  NULL if it cannot be read. */
static char *
read_text(const char *path, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= max) { free(buf); fclose(f); return NULL; }
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb; cap *= 2;
        }
        size_t want = cap - 1 - n;
        size_t r = fread(buf + n, 1, want, f);
        n += r;
        if (r < want) break;            /* end of file or an error */
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

/* Write a file via a temporary sibling and rename, so a reader never sees
 * half of it.  Returns 0, or -1. */
static int
write_atomic(const char *path, const char *text)
{
    char tmp[PKG_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp-%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int bad = fwrite(text, 1, n, f) != n;
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Lock write via a temporary sibling and rename. */
static int
lock_write_atomic(const char *path, const PkgLock *lock)
{
    char tmp[PKG_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp-%ld", path, (long)getpid());
    if (pkg_lock_write(tmp, lock) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* ── Package cache ──────────────────────────────────────────────────────── */

static int
cache_root(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') {
        pdiag(DIAG_ERROR, E9032, "", 0,
              "the package cache is $HOME/.toke/pkg, and HOME is %s",
              home ? "not an absolute path" : "not set");
        return -1;
    }
    if ((size_t)snprintf(buf, n, "%s/.toke/pkg", home) >= n) {
        pdiag(DIAG_ERROR, E9032, "", 0, "HOME is too long for the package cache path");
        return -1;
    }
    return 0;
}

static void
cache_dir(const char *root, const char *name, PkgVersion v, char *buf, size_t n)
{
    char vb[PKG_MAX_VERSION];
    vfmt(v, vb);
    snprintf(buf, n, "%s/%s/%s", root, name, vb);
}

/* The commit recorded beside a cached tree.  Returns 0, or -1. */
static int
read_sidecar(const char *dir, char commit[PKG_MAX_COMMIT])
{
    char p[PKG_PATH];
    snprintf(p, sizeof p, "%s.commit", dir);
    char *t = read_text(p, 256);
    if (!t) return -1;
    size_t l = strlen(t);
    while (l > 0 && (t[l - 1] == '\n' || t[l - 1] == '\r')) t[--l] = '\0';
    int ok = pkg_commit_valid(t);
    if (ok) snprintf(commit, PKG_MAX_COMMIT, "%s", t);
    free(t);
    return ok ? 0 : -1;
}

/* ── Command-line options ───────────────────────────────────────────────── */

typedef struct {
    const char *pos[4];
    int         npos;
    const char *git;
} PkgArgs;

/* Parse the flags every command shares, plus --git for add.  Returns 0,
 * or -1 after emitting E9031. */
static int
parse_args(int argc, char **argv, int allow_git, PkgArgs *a)
{
    memset(a, 0, sizeof *a);
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--allow-file-source")) { pkg_set_allow_file_sources(true); continue; }
        if (!strcmp(s, "--diag-text")) { diag_set_format(DIAG_FMT_TEXT); continue; }
        if (!strcmp(s, "--diag-json")) { diag_set_format(DIAG_FMT_JSON); continue; }
        if (allow_git && !strcmp(s, "--git")) {
            if (++i >= argc) {
                pdiag(DIAG_ERROR, E9031, "", 0, "tkc pkg %s: --git requires a URL", argv[0]);
                return -1;
            }
            a->git = argv[i];
            continue;
        }
        if (s[0] == '-' && s[1] != '\0') {
            pdiag(DIAG_ERROR, E9031, "", 0, "tkc pkg %s: unknown flag '%s'", argv[0], s);
            return -1;
        }
        if (a->npos >= 4) {
            pdiag(DIAG_ERROR, E9031, "", 0, "tkc pkg %s: too many arguments", argv[0]);
            return -1;
        }
        a->pos[a->npos++] = s;
    }
    return 0;
}

static int
want_args(const char *cmd, const PkgArgs *a, int lo, int hi, const char *usage)
{
    if (a->npos < lo || a->npos > hi) {
        pdiag(DIAG_ERROR, E9031, "", 0, "usage: tkc pkg %s %s", cmd, usage);
        return -1;
    }
    return 0;
}

/* ── Manifest ───────────────────────────────────────────────────────────── */

static int
load_manifest(const char *path, PkgManifest *m)
{
    char err[PKG_MSG];
    if (!file_exists(path)) {
        pdiag(DIAG_ERROR, E9032, path, 0,
              "no %s in this directory; create one with `tkc pkg init`", path);
        return -1;
    }
    if (pkg_manifest_parse(path, m, err, (int)sizeof err) != 0) {
        pdiag(DIAG_ERROR, E9012, path, err_line(err), "malformed %s: %s", path, err);
        return -1;
    }
    return 0;
}

/* Line-oriented manifest editing.  The manifest is a closed TOML subset
 * (pkg.c), so a line is a table header, a key = "value" pair, a comment or
 * blank; editing by line keeps the user's comments and order. */
typedef struct {
    char **v;
    int    n, cap;
} Lines;

static void
lines_free(Lines *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = l->cap = 0;
}

static int
lines_insert(Lines *l, int at, const char *s)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        char **nv = realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        l->v = nv; l->cap = nc;
    }
    char *d = strdup(s);
    if (!d) return -1;
    memmove(l->v + at + 1, l->v + at, (size_t)(l->n - at) * sizeof *l->v);
    l->v[at] = d;
    l->n++;
    return 0;
}

static void
lines_delete(Lines *l, int at)
{
    free(l->v[at]);
    memmove(l->v + at, l->v + at + 1, (size_t)(l->n - at - 1) * sizeof *l->v);
    l->n--;
}

static int
lines_split(const char *text, Lines *l)
{
    memset(l, 0, sizeof *l);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *s = malloc(len + 1);
        if (!s) { lines_free(l); return -1; }
        memcpy(s, p, len); s[len] = '\0';
        if (len > 0 && s[len - 1] == '\r') s[len - 1] = '\0';
        if (lines_insert(l, l->n, s) != 0) { free(s); lines_free(l); return -1; }
        free(s);
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

static char *
lines_join(const Lines *l)
{
    size_t n = 1;
    for (int i = 0; i < l->n; i++) n += strlen(l->v[i]) + 1;
    char *out = malloc(n), *p = out;
    if (!out) return NULL;
    for (int i = 0; i < l->n; i++) {
        size_t k = strlen(l->v[i]);
        memcpy(p, l->v[i], k); p += k; *p++ = '\n';
    }
    *p = '\0';
    return out;
}

/* Table header name of a line ("dependencies.web" for "[dependencies.web]"),
 * or NULL when the line is not a header. */
static const char *
header_of(const char *line, char *buf, size_t n)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '[') return NULL;
    const char *e = strchr(line, ']');
    if (!e) return NULL;
    size_t k = (size_t)(e - line - 1);
    if (k >= n) k = n - 1;
    memcpy(buf, line + 1, k); buf[k] = '\0';
    /* trim */
    while (k > 0 && (buf[k - 1] == ' ' || buf[k - 1] == '\t')) buf[--k] = '\0';
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    if (s != buf) memmove(buf, s, strlen(s) + 1);
    return buf;
}

/* Key of a key = "value" line, or NULL. */
static const char *
key_of(const char *line, char *buf, size_t n)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '[') return NULL;
    size_t k = strcspn(line, " \t=");
    if (k == 0 || k >= n) return NULL;
    memcpy(buf, line, k); buf[k] = '\0';
    return buf;
}

/* [first, end) line range of a table's body; first = -1 if absent. */
static void
table_range(const Lines *l, const char *table, int *hdr, int *end)
{
    char hb[256];
    *hdr = -1; *end = l->n;
    for (int i = 0; i < l->n; i++) {
        const char *h = header_of(l->v[i], hb, sizeof hb);
        if (!h) continue;
        if (*hdr >= 0) { *end = i; return; }
        if (!strcmp(h, table)) *hdr = i;
    }
}

static int
find_key(const Lines *l, int from, int to, const char *key)
{
    char kb[256];
    for (int i = from; i < to; i++) {
        const char *k = key_of(l->v[i], kb, sizeof kb);
        if (k && !strcmp(k, key)) return i;
    }
    return -1;
}

static int
set_line(Lines *l, int at, const char *s)
{
    char *d = strdup(s);
    if (!d) return -1;
    free(l->v[at]);
    l->v[at] = d;
    return 0;
}

/* Insert after the last non-blank line of [from, to). */
static int
append_in(Lines *l, int from, int to, const char *s)
{
    int at = to;
    while (at > from && l->v[at - 1][0] == '\0') at--;
    return lines_insert(l, at, s);
}

/* Add or update dependency `name`.  `git` NULL keeps the form it has. */
static int
manifest_edit_add(Lines *l, const char *name, const char *constraint,
                  const char *git)
{
    char tbl[PKG_MAX_NAME + 16], line[PKG_MAX_LINE];
    snprintf(tbl, sizeof tbl, "dependencies.%s", name);
    int h, e;
    table_range(l, tbl, &h, &e);
    if (h >= 0) {                                   /* extended form */
        snprintf(line, sizeof line, "version = \"%s\"", constraint);
        int k = find_key(l, h + 1, e, "version");
        if (k >= 0 ? set_line(l, k, line) : append_in(l, h + 1, e, line)) return -1;
        if (git) {
            table_range(l, tbl, &h, &e);
            snprintf(line, sizeof line, "git = \"%s\"", git);
            k = find_key(l, h + 1, e, "git");
            if (k >= 0 ? set_line(l, k, line) : append_in(l, h + 1, e, line)) return -1;
        }
        return 0;
    }
    table_range(l, "dependencies", &h, &e);
    int k = h >= 0 ? find_key(l, h + 1, e, name) : -1;
    if (!git) {
        snprintf(line, sizeof line, "%s = \"%s\"", name, constraint);
        if (k >= 0) return set_line(l, k, line);
        if (h >= 0) return append_in(l, h + 1, e, line);
        if (l->n > 0 && l->v[l->n - 1][0] && lines_insert(l, l->n, "")) return -1;
        if (lines_insert(l, l->n, "[dependencies]")) return -1;
        return lines_insert(l, l->n, line);
    }
    /* With a source: the extended form, replacing a simple-form line. */
    if (k >= 0) lines_delete(l, k);
    while (l->n > 0 && l->v[l->n - 1][0] == '\0') lines_delete(l, l->n - 1);
    if (l->n > 0 && lines_insert(l, l->n, "")) return -1;
    snprintf(line, sizeof line, "[%s]", tbl);
    if (lines_insert(l, l->n, line)) return -1;
    snprintf(line, sizeof line, "version = \"%s\"", constraint);
    if (lines_insert(l, l->n, line)) return -1;
    snprintf(line, sizeof line, "git = \"%s\"", git);
    return lines_insert(l, l->n, line);
}

/* Remove dependency `name` in either form.  Returns 1 if removed, 0 if it
 * was not there. */
static int
manifest_edit_remove(Lines *l, const char *name)
{
    char tbl[PKG_MAX_NAME + 16];
    snprintf(tbl, sizeof tbl, "dependencies.%s", name);
    int h, e, removed = 0;
    table_range(l, tbl, &h, &e);
    if (h >= 0) {
        for (int i = e - 1; i >= h; i--) lines_delete(l, i);
        while (h > 0 && h <= l->n && l->v[h - 1][0] == '\0' &&
               (h == l->n || l->v[h][0] == '\0' || l->v[h][0] == '['))
            { lines_delete(l, h - 1); h--; }
        removed = 1;
    }
    table_range(l, "dependencies", &h, &e);
    int k = h >= 0 ? find_key(l, h + 1, e, name) : -1;
    if (k >= 0) { lines_delete(l, k); removed = 1; }
    return removed;
}

/* ── Resolution (MVS over the transitive graph) ─────────────────────────── */

typedef struct {
    PkgDep dep;
    char   from[PKG_MAX_NAME];   /* the package that requires it; "" = root */
} Req;

typedef struct {
    char       name[PKG_MAX_NAME];
    PkgVersion version;
} Visit;

typedef struct {
    const PkgManifest *root;
    Req       *reqs;
    int        nreq;
    Visit      visit[PKG_MAX_REQS];
    int        nvisit;
    PkgLock    pins;        /* previous lock + pins recorded this run */
    char       cache[PKG_PATH];
    int        have_cache;
    int        do_fetch;
    int        unfetched;   /* resolve: packages whose manifests are not cached */
} Graph;

static int
add_req(Graph *g, const PkgDep *d, const char *from)
{
    for (int i = 0; i < g->nreq; i++)
        if (!strcmp(g->reqs[i].from, from) && !strcmp(g->reqs[i].dep.name, d->name))
            return 0;                                       /* already there */
    if (g->nreq >= PKG_MAX_REQS) {
        pdiag(DIAG_ERROR, E9025, PKG_MANIFEST, 0,
              "the dependency graph has more than %d requirements", PKG_MAX_REQS);
        return -1;
    }
    g->reqs[g->nreq].dep = *d;
    snprintf(g->reqs[g->nreq].from, PKG_MAX_NAME, "%s", from);
    g->nreq++;
    return 1;
}

static const char *
who(const Graph *g, const Req *r)
{
    return r->from[0] ? r->from : g->root->name;
}

static PkgResolved *
pin_find(PkgLock *pins, const char *name)
{
    for (int i = 0; i < pins->count; i++)
        if (!strcmp(pins->entries[i].name, name)) return &pins->entries[i];
    return NULL;
}

/* E9034 / E9033 checks before MVS. */
static int
graph_precheck(Graph *g)
{
    for (int i = 0; i < g->nreq; i++) {
        const Req *a = &g->reqs[i];
        if (!strcmp(a->dep.name, g->root->name)) {
            pdiag(DIAG_ERROR, E9033, PKG_MANIFEST, 0,
                  "circular package dependency: '%s' requires '%s', the package being built",
                  who(g, a), a->dep.name);
            return -1;
        }
        if (!a->dep.git[0]) continue;
        for (int j = 0; j < i; j++) {
            const Req *b = &g->reqs[j];
            if (strcmp(a->dep.name, b->dep.name) || !b->dep.git[0]) continue;
            if (strcmp(a->dep.git, b->dep.git)) {
                pdiag(DIAG_ERROR, E9034, PKG_MANIFEST, 0,
                      "'%s' has two sources: '%s' requires it from %s, '%s' from %s",
                      a->dep.name, who(g, b), b->dep.git, who(g, a), a->dep.git);
                return -1;
            }
        }
    }
    return 0;
}

/* Explain a pkg_resolve failure as E9025, naming the package. */
static void
explain_unsat(Graph *g)
{
    for (int i = 0; i < g->nreq; i++) {
        const char *name = g->reqs[i].dep.name;
        PkgVersion sel = g->reqs[i].dep.minimum;
        const Req *by = &g->reqs[i];
        for (int j = 0; j < g->nreq; j++)
            if (!strcmp(g->reqs[j].dep.name, name) &&
                pkg_version_cmp(g->reqs[j].dep.minimum, sel) > 0) {
                sel = g->reqs[j].dep.minimum; by = &g->reqs[j];
            }
        for (int j = 0; j < g->nreq; j++) {
            if (strcmp(g->reqs[j].dep.name, name) || dep_admits(&g->reqs[j].dep, sel))
                continue;
            char v[PKG_MAX_VERSION], c1[80], c2[80];
            vfmt(sel, v);
            dep_constraint(&by->dep, c1, sizeof c1);
            dep_constraint(&g->reqs[j].dep, c2, sizeof c2);
            pdiag(DIAG_ERROR, E9025, PKG_MANIFEST, 0,
                  "no version of '%s' satisfies every constraint: minimal version "
                  "selection picks %s ('%s' requires \"%s\"), which '%s' does not admit (\"%s\")",
                  name, v, who(g, by), c1, who(g, &g->reqs[j]), c2);
            return;
        }
    }
    pdiag(DIAG_ERROR, E9025, PKG_MANIFEST, 0,
          "the dependency graph cannot be resolved (more than %d packages?)", PKG_MAX_DEPS);
}

static int
mvs(Graph *g, PkgLock *sel)
{
    PkgDep *deps = malloc((size_t)(g->nreq ? g->nreq : 1) * sizeof *deps);
    if (!deps) return -1;
    for (int i = 0; i < g->nreq; i++) deps[i] = g->reqs[i].dep;
    int rc = pkg_resolve(deps, g->nreq, sel);
    free(deps);
    if (rc != 0) { explain_unsat(g); return -1; }
    /* A package no manifest gives a source keeps the one its lock recorded:
     * a source is resolved once (ADR-0004 section 3). */
    for (int i = 0; i < sel->count; i++) {
        PkgResolved *e = &sel->entries[i];
        if (e->source[0]) continue;
        PkgResolved *p = pin_find(&g->pins, e->name);
        if (p && p->source[0]) snprintf(e->source, sizeof e->source, "%s", p->source);
    }
    return 0;
}

static bool
visited(const Graph *g, const PkgResolved *e)
{
    for (int i = 0; i < g->nvisit; i++)
        if (!strcmp(g->visit[i].name, e->name) &&
            pkg_version_cmp(g->visit[i].version, e->version) == 0)
            return true;
    return false;
}

/* The pin recorded for exactly this name, version and source, or NULL. */
static PkgResolved *
pin_for(Graph *g, const PkgResolved *e)
{
    PkgResolved *p = pin_find(&g->pins, e->name);
    if (!p || pkg_version_cmp(p->version, e->version) != 0) return NULL;
    if (p->source[0] && e->source[0] && strcmp(p->source, e->source)) return NULL;
    return p;
}

static void
pin_record(Graph *g, const PkgResolved *e)
{
    PkgResolved *p = pin_find(&g->pins, e->name);
    if (!p) {
        if (g->pins.count >= PKG_MAX_DEPS) return;
        p = &g->pins.entries[g->pins.count++];
    }
    *p = *e;
}

/* Read and check a package's own manifest: it must declare the name and
 * version the lock gives it (ADR-0004, step 3 of fetch). */
static int
package_manifest(const char *dir, const PkgResolved *e, PkgManifest *m,
                 const char *what)
{
    char path[PKG_PATH], err[PKG_MSG], v[PKG_MAX_VERSION], mv[PKG_MAX_VERSION];
    snprintf(path, sizeof path, "%s/%s", dir, PKG_MANIFEST);
    vfmt(e->version, v);
    if (!file_exists(path)) {
        pdiag(DIAG_ERROR, E9030, path, 0,
              "%s '%s' %s has no pkg.toml", what, e->name, v);
        return -1;
    }
    if (pkg_manifest_parse(path, m, err, (int)sizeof err) != 0) {
        pdiag(DIAG_ERROR, E9012, path, err_line(err),
              "malformed pkg.toml in package '%s' %s: %s", e->name, v, err);
        return -1;
    }
    vfmt(m->version, mv);
    if (strcmp(m->name, e->name) || pkg_version_cmp(m->version, e->version)) {
        pdiag(DIAG_ERROR, E9030, path, 0,
              "%s '%s' %s declares itself as '%s' %s", what, e->name, v, m->name, mv);
        return -1;
    }
    return 0;
}

/*
 * Get package e into the cache (fetch mode) and return its manifest.
 * Returns 0 with *m filled, 1 when the manifest is not available (resolve
 * mode, not cached), -1 after a diagnostic.
 */
static int
obtain(Graph *g, PkgResolved *e, PkgManifest *m)
{
    char dir[PKG_PATH], v[PKG_MAX_VERSION], err[PKG_MSG];
    vfmt(e->version, v);
    if (!g->have_cache) return 1;
    cache_dir(g->cache, e->name, e->version, dir, sizeof dir);

    PkgResolved *pin = pin_for(g, e);
    bool has_c = pin && pin->commit[0], has_h = pin && pin->hash[0];
    if (has_c != has_h) {
        pdiag(DIAG_ERROR, E9029, PKG_LOCKFILE, 0,
              "pkg.lock pins '%s' %s only partly (a %s but no %s); delete its "
              "entry to fetch it afresh", e->name, v,
              has_c ? "commit" : "hash", has_c ? "hash" : "commit");
        return -1;
    }
    bool pinned = has_c && has_h;
    PkgResolved want = *e;
    if (pinned) {
        snprintf(want.commit, sizeof want.commit, "%s", pin->commit);
        snprintf(want.hash, sizeof want.hash, "%s", pin->hash);
        if (!want.source[0]) snprintf(want.source, sizeof want.source, "%s", pin->source);
    }

    bool cached = dir_exists(dir);
    if (cached && pinned) {
        char got[PKG_MAX_COMMIT];
        if (read_sidecar(dir, got) != 0) {
            pdiag(DIAG_ERROR, E9030, dir, 0,
                  "cached package '%s' %s has no recorded commit; delete %s and fetch again",
                  e->name, v, dir);
            return -1;
        }
        if (pkg_verify_fetched(&want, got, dir, err, (int)sizeof err) != 0) {
            pdiag(DIAG_ERROR, E9030, dir, 0,
                  "cached package '%s' %s does not match pkg.lock: %s", e->name, v, err);
            return -1;
        }
        if (g->do_fetch) printf("fetching %s %s ... ok (cached)\n", e->name, v);
        *e = want;
        return package_manifest(dir, e, m, "cached package");
    }
    if (!g->do_fetch) {
        if (!cached) { g->unfetched++; return 1; }
        /* resolve: an unpinned cached tree may supply a manifest for
         * resolution; nothing is pinned from it. */
        return package_manifest(dir, e, m, "cached package");
    }

    /* Fetch mode: from the source, into a temporary sibling. */
    if (!want.source[0]) {
        pdiag(DIAG_ERROR, E9011, PKG_MANIFEST, 0,
              "package '%s' has no git source and there is no package index; "
              "declare one: tkc pkg add %s <constraint> --git <url>", e->name, e->name);
        return -1;
    }
    if (!pkg_source_valid(want.source)) {
        pdiag(DIAG_ERROR, E9029, PKG_LOCKFILE, 0,
              "source of '%s' is not an allowed transport: %s", e->name, want.source);
        return -1;
    }
    char parent[PKG_PATH], tmp[PKG_PATH], tag[PKG_MAX_VERSION + 2], commit[PKG_MAX_COMMIT];
    snprintf(parent, sizeof parent, "%s/%s", g->cache, e->name);
    snprintf(tmp, sizeof tmp, "%s/.tmp-%s-%ld", parent, v, (long)getpid());
    snprintf(tag, sizeof tag, "v%s", v);
    if (pkg_mkdirs(parent) != 0) {
        pdiag(DIAG_ERROR, E9032, parent, 0, "cannot create the package cache directory %s: %s",
              parent, strerror(errno));
        return -1;
    }
    pkg_rmtree(tmp);
    int gr = pkg_git_fetch_tag(want.source, tag, tmp, commit, err, (int)sizeof err);
    if (gr == PKG_GIT_NOTAG) {
        pdiag(DIAG_ERROR, E9014, PKG_MANIFEST, 0,
              "no git tag %s for package '%s' in %s", tag, e->name, want.source);
        return -1;
    }
    if (gr != PKG_GIT_OK) {
        pdiag(DIAG_ERROR, E9013, PKG_MANIFEST, 0,
              "fetching package '%s' %s from %s failed: %s", e->name, v, want.source, err);
        return -1;
    }

    int rc = -1;
    if (pinned) {
        /* Every later fetch must reproduce the first: same commit (a moved
         * tag fails here), same content, hashed by the verifier itself. */
        if (pkg_verify_fetched(&want, commit, tmp, err, (int)sizeof err) != 0) {
            pdiag(DIAG_ERROR, E9030, PKG_LOCKFILE, 0,
                  "package '%s' %s from %s does not match pkg.lock: %s",
                  e->name, v, want.source, err);
            goto out;
        }
    } else {
        /* Trust on first use (ADR-0004 section 1): record all three pins. */
        snprintf(want.commit, sizeof want.commit, "%s", commit);
        if (pkg_tree_hash(tmp, want.hash, err, (int)sizeof err) != 0) {
            pdiag(DIAG_ERROR, E9030, tmp, 0,
                  "package '%s' %s cannot be hashed: %s", e->name, v, err);
            goto out;
        }
    }
    if (package_manifest(tmp, &want, m, "package") != 0) goto out;

    if (cached) {
        /* An unpinned tree already in the cache must be the same bytes. */
        char have[PKG_MAX_HASH];
        if (pkg_tree_hash(dir, have, err, (int)sizeof err) != 0 ||
            strcmp(have, want.hash) != 0) {
            pdiag(DIAG_ERROR, E9030, dir, 0,
                  "cached package '%s' %s differs from its source; delete %s and fetch again",
                  e->name, v, dir);
            goto out;
        }
        pkg_rmtree(tmp);
    } else if (rename(tmp, dir) != 0) {
        pdiag(DIAG_ERROR, E9032, dir, 0, "cannot install package '%s' %s into %s: %s",
              e->name, v, dir, strerror(errno));
        goto out;
    }
    {
        char side[PKG_PATH], text[PKG_MAX_COMMIT + 2];
        snprintf(side, sizeof side, "%s.commit", dir);
        snprintf(text, sizeof text, "%s\n", want.commit);
        if (write_atomic(side, text) != 0) {
            pdiag(DIAG_ERROR, E9032, side, 0, "cannot write %s", side);
            goto out;
        }
    }
    printf("fetching %s %s ... ok\n", e->name, v);
    *e = want;
    pin_record(g, &want);
    rc = 0;
out:
    if (rc != 0) pkg_rmtree(tmp);
    return rc;
}

/* DFS for a cycle among the selected packages (E9033): white/grey/black
 * colouring over sel, edges from the requirements. */
static int
sel_index(const PkgLock *sel, const char *name)
{
    for (int i = 0; i < sel->count; i++)
        if (!strcmp(sel->entries[i].name, name)) return i;
    return -1;
}

static int
cycle_dfs(const Graph *g, const PkgLock *sel, int at, char *colour,
          int *stack, int depth)
{
    colour[at] = 1;
    stack[depth] = at;
    for (int i = 0; i < g->nreq; i++) {
        if (strcmp(g->reqs[i].from, sel->entries[at].name)) continue;
        int to = sel_index(sel, g->reqs[i].dep.name);
        if (to < 0) continue;
        if (colour[to] == 1) {
            char path[PKG_MSG] = "";
            int k = depth;
            while (k > 0 && stack[k] != to) k--;
            for (; k <= depth; k++) {
                strncat(path, sel->entries[stack[k]].name, sizeof path - strlen(path) - 1);
                strncat(path, " -> ", sizeof path - strlen(path) - 1);
            }
            strncat(path, sel->entries[to].name, sizeof path - strlen(path) - 1);
            pdiag(DIAG_ERROR, E9033, PKG_MANIFEST, 0,
                  "circular package dependency: %s", path);
            return -1;
        }
        if (colour[to] == 0 && cycle_dfs(g, sel, to, colour, stack, depth + 1) != 0)
            return -1;
    }
    colour[at] = 2;
    return 0;
}

/*
 * Resolve the manifest's graph.  Minimal version selection over the root's
 * requirements and those of every package version reached; in fetch mode
 * each selected version is fetched (or verified in the cache) so its own
 * requirements are known, until nothing new is required.  Pins from `old`
 * carry over for every package whose version and source are unchanged.
 */
static int
resolve_graph(const PkgManifest *root, const PkgLock *old, int do_fetch,
              PkgLock *out, int *unfetched)
{
    Graph *g = calloc(1, sizeof *g);
    if (!g) return -1;
    g->reqs = calloc(PKG_MAX_REQS, sizeof *g->reqs);
    if (!g->reqs) { free(g); return -1; }
    g->root = root;
    g->do_fetch = do_fetch;
    if (old) g->pins = *old;
    int rc = -1;

    if (do_fetch) {
        if (cache_root(g->cache, sizeof g->cache) != 0) goto done;
        g->have_cache = 1;
    } else {
        const char *home = getenv("HOME");
        if (home && home[0] == '/' &&
            (size_t)snprintf(g->cache, sizeof g->cache, "%s/.toke/pkg", home) < sizeof g->cache)
            g->have_cache = 1;
    }

    for (int i = 0; i < root->dep_count; i++)
        if (add_req(g, &root->deps[i], "") < 0) goto done;

    PkgLock sel;
    for (int iter = 0; ; iter++) {
        if (iter > PKG_MAX_REQS) {
            pdiag(DIAG_ERROR, E9025, PKG_MANIFEST, 0, "resolution did not converge");
            goto done;
        }
        if (graph_precheck(g) != 0) goto done;
        if (mvs(g, &sel) != 0) goto done;
        int grew = 0;
        for (int i = 0; i < sel.count; i++) {
            PkgResolved *e = &sel.entries[i];
            if (visited(g, e)) continue;
            if (g->nvisit >= PKG_MAX_REQS) {
                pdiag(DIAG_ERROR, E9025, PKG_MANIFEST, 0, "too many package versions");
                goto done;
            }
            snprintf(g->visit[g->nvisit].name, PKG_MAX_NAME, "%s", e->name);
            g->visit[g->nvisit].version = e->version;
            g->nvisit++;
            PkgManifest *m = calloc(1, sizeof *m);
            if (!m) goto done;
            int r = obtain(g, e, m);
            if (r < 0) { free(m); goto done; }
            if (r == 0)
                for (int k = 0; k < m->dep_count; k++) {
                    int a = add_req(g, &m->deps[k], e->name);
                    if (a < 0) { free(m); goto done; }
                    if (a > 0) grew = 1;
                }
            free(m);
            /* New requirements: re-check and re-select before fetching
             * anything else, so a conflict they bring (two sources, an
             * unsatisfiable constraint, a higher minimum) is found before a
             * package is fetched from the wrong place or at the wrong
             * version. */
            if (grew) break;
        }
        if (!grew) break;
    }

    /* Cycles among packages (the root case was refused in precheck). */
    {
        char colour[PKG_MAX_DEPS] = { 0 };
        int stack[PKG_MAX_DEPS];
        for (int i = 0; i < sel.count; i++)
            if (colour[i] == 0 && cycle_dfs(g, &sel, i, colour, stack, 0) != 0) goto done;
    }

    /* Carry pins: the commit and hash stay with an unchanged version and
     * source (a changed one is fetched afresh). */
    for (int i = 0; i < sel.count; i++) {
        PkgResolved *e = &sel.entries[i];
        PkgResolved *p = pin_for(g, e);
        if (!p) continue;
        if (!e->source[0]) snprintf(e->source, sizeof e->source, "%s", p->source);
        snprintf(e->commit, sizeof e->commit, "%s", p->commit);
        snprintf(e->hash, sizeof e->hash, "%s", p->hash);
    }
    *out = sel;
    if (unfetched) *unfetched = g->unfetched;
    rc = 0;
done:
    free(g->reqs);
    free(g);
    return rc;
}

/* Read pkg.lock if present.  Returns 1 if read, 0 if absent, -1 (E9028). */
static int
load_lock_opt(PkgLock *lock)
{
    if (!file_exists(PKG_LOCKFILE)) { memset(lock, 0, sizeof *lock); return 0; }
    if (pkg_lock_read(PKG_LOCKFILE, lock) != 0) {
        pdiag(DIAG_ERROR, E9028, PKG_LOCKFILE, 0,
              "pkg.lock is malformed%s; delete it and run `tkc pkg resolve`",
              pkg_allow_file_sources() ? "" :
              " (a file:// source needs --allow-file-source)");
        return -1;
    }
    return 1;
}

static void
print_lock(const char *head, const PkgLock *lock)
{
    printf("%s %d package%s%s\n", head, lock->count, lock->count == 1 ? "" : "s",
           lock->count ? ":" : "");
    for (int i = 0; i < lock->count; i++) {
        char v[PKG_MAX_VERSION];
        vfmt(lock->entries[i].version, v);
        printf("  %-14s %s\n", lock->entries[i].name, v);
    }
}

/* Resolve (fetch or not) and write pkg.lock; optionally rewrite pkg.toml
 * first with new_manifest_text (add/remove), only if resolution succeeds. */
static int
resolve_and_write(const PkgManifest *m, int do_fetch, const char *new_manifest_text)
{
    PkgLock *old = calloc(1, sizeof *old), *lock = calloc(1, sizeof *lock);
    int rc = 1;
    if (!old || !lock) goto out;
    if (load_lock_opt(old) < 0) goto out;
    int unfetched = 0;
    if (resolve_graph(m, old, do_fetch, lock, &unfetched) != 0) goto out;
    if (do_fetch) {
        char err[PKG_MSG];
        if (pkg_lock_check_pinned(lock, err, (int)sizeof err) != 0) {
            pdiag(DIAG_ERROR, E9029, PKG_LOCKFILE, 0, "%s", err);
            goto out;
        }
    }
    if (new_manifest_text && write_atomic(PKG_MANIFEST, new_manifest_text) != 0) {
        pdiag(DIAG_ERROR, E9032, PKG_MANIFEST, 0, "cannot write pkg.toml: %s", strerror(errno));
        goto out;
    }
    if (lock_write_atomic(PKG_LOCKFILE, lock) != 0) {
        pdiag(DIAG_ERROR, E9032, PKG_LOCKFILE, 0, "cannot write pkg.lock: %s", strerror(errno));
        goto out;
    }
    print_lock(do_fetch ? "fetched" : "resolved", lock);
    if (unfetched)
        printf("note: %d package%s not in the package cache; their own dependencies "
               "are added by `tkc pkg fetch`\n", unfetched, unfetched == 1 ? " is" : "s are");
    rc = 0;
out:
    free(old); free(lock);
    return rc;
}

/* ── Commands ───────────────────────────────────────────────────────────── */

static int
cmd_init(const PkgArgs *a)
{
    if (want_args("init", a, 0, 1, "[<name>]") != 0) return TKC_EXIT_USAGE;
    if (file_exists(PKG_MANIFEST)) {
        pdiag(DIAG_WARNING, W9011, PKG_MANIFEST, 0,
              "pkg.toml already exists; it was left unchanged");
        return 0;
    }
    char name[PKG_PATH];
    if (a->npos == 1) {
        snprintf(name, sizeof name, "%s", a->pos[0]);
    } else {
        char cwd[PKG_PATH];
        if (!getcwd(cwd, sizeof cwd)) {
            pdiag(DIAG_ERROR, E9032, "", 0, "cannot read the current directory");
            return 1;
        }
        const char *b = strrchr(cwd, '/');
        snprintf(name, sizeof name, "%s", b ? b + 1 : cwd);
    }
    if (!pkg_name_valid(name)) {
        pdiag(DIAG_ERROR, E9031, "", 0,
              "'%s' is not a valid package name ([a-z][a-z0-9]{0,62}, not std, pkg, "
              "toke or tkc)%s", name, a->npos ? "" : "; pass one: tkc pkg init <name>");
        return TKC_EXIT_USAGE;
    }
    char text[PKG_PATH + 128];
    snprintf(text, sizeof text,
             "[package]\nname = \"%s\"\nversion = \"0.1.0\"\n\n[dependencies]\n", name);
    if (write_atomic(PKG_MANIFEST, text) != 0) {
        pdiag(DIAG_ERROR, E9032, PKG_MANIFEST, 0, "cannot write pkg.toml: %s", strerror(errno));
        return 1;
    }
    printf("created pkg.toml for package '%s'\n", name);
    return 0;
}

/* Is s safe inside a TOML "..." value (pkg.c's subset)? */
static bool
value_safe(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '"' || *p == '\\' || *p == '#')
            return false;
    return strlen(s) < 200;
}

static int
cmd_add_remove(const PkgArgs *a, int add)
{
    const char *cmd = add ? "add" : "remove";
    if (want_args(cmd, a, add ? 2 : 1, add ? 2 : 1,
                  add ? "<name> <constraint> [--git <url>]" : "<name>") != 0)
        return TKC_EXIT_USAGE;
    const char *name = a->pos[0];
    if (!pkg_name_valid(name)) {
        pdiag(DIAG_ERROR, E9031, "", 0,
              "'%s' is not a valid package name ([a-z][a-z0-9]{0,62}, not std, pkg, toke or tkc)",
              name);
        return TKC_EXIT_USAGE;
    }
    if (add) {
        const char *c = a->pos[1];
        char probe[600], err[PKG_MSG];
        PkgManifest pm;
        if (!value_safe(c)) {
            pdiag(DIAG_ERROR, E9031, "", 0, "invalid version constraint '%s'", c);
            return TKC_EXIT_USAGE;
        }
        snprintf(probe, sizeof probe,
                 "[package]\nname = \"probe\"\nversion = \"0.0.0\"\n[dependencies]\n%s = \"%s\"\n",
                 name, c);
        if (pkg_manifest_parse_str(probe, &pm, err, (int)sizeof err) != 0) {
            pdiag(DIAG_ERROR, E9031, "", 0,
                  "invalid version constraint '%s': use \"x.y.z\", \">=x.y.z\" or \">=x.y.z <a.b.c\"",
                  c);
            return TKC_EXIT_USAGE;
        }
        if (a->git && (!value_safe(a->git) || !pkg_source_valid(a->git))) {
            pdiag(DIAG_ERROR, E9031, "", 0,
                  "'%s' is not an allowed package source: use https://, ssh:// or "
                  "user@host:path%s", a->git,
                  strncmp(a->git, "file://", 7) == 0 && !pkg_allow_file_sources()
                      ? " (a file:// source needs --allow-file-source)" : "");
            return TKC_EXIT_USAGE;
        }
    } else if (a->git) {
        pdiag(DIAG_ERROR, E9031, "", 0, "tkc pkg remove takes no --git");
        return TKC_EXIT_USAGE;
    }

    PkgManifest *m = calloc(1, sizeof *m);
    if (!m) return 1;
    int rc = 1;
    char *text = NULL, *edited = NULL;
    Lines l = { 0 };
    if (load_manifest(PKG_MANIFEST, m) != 0) goto out;
    if (add && !strcmp(name, m->name)) {
        pdiag(DIAG_ERROR, E9031, "", 0, "package '%s' cannot depend on itself", name);
        rc = TKC_EXIT_USAGE;
        goto out;
    }
    text = read_text(PKG_MANIFEST, 1 << 20);
    if (!text || lines_split(text, &l) != 0) {
        pdiag(DIAG_ERROR, E9032, PKG_MANIFEST, 0, "cannot read pkg.toml");
        goto out;
    }
    if (add) {
        if (manifest_edit_add(&l, name, a->pos[1], a->git) != 0) {
            pdiag(DIAG_ERROR, E9032, PKG_MANIFEST, 0, "out of memory editing pkg.toml");
            goto out;
        }
    } else if (!manifest_edit_remove(&l, name)) {
        pdiag(DIAG_ERROR, E9031, PKG_MANIFEST, 0, "'%s' is not a dependency in pkg.toml", name);
        rc = TKC_EXIT_USAGE;
        goto out;
    }
    edited = lines_join(&l);
    char err[PKG_MSG];
    if (!edited || pkg_manifest_parse_str(edited, m, err, (int)sizeof err) != 0) {
        pdiag(DIAG_ERROR, E9012, PKG_MANIFEST, err_line(err),
              "editing pkg.toml would make it malformed (%s); it was left unchanged",
              edited ? err : "out of memory");
        goto out;
    }
    rc = resolve_and_write(m, 0, edited);
    if (rc == 0) printf("%s %s\n", add ? "added" : "removed", name);
out:
    lines_free(&l);
    free(text); free(edited); free(m);
    return rc;
}

static int
cmd_resolve_fetch(const PkgArgs *a, int fetch)
{
    if (want_args(fetch ? "fetch" : "resolve", a, 0, 0, "") != 0) return TKC_EXIT_USAGE;
    PkgManifest *m = calloc(1, sizeof *m);
    if (!m) return 1;
    int rc = load_manifest(PKG_MANIFEST, m) == 0 ? resolve_and_write(m, fetch, NULL) : 1;
    free(m);
    return rc;
}

static int
cmd_list(const PkgArgs *a)
{
    if (want_args("list", a, 0, 0, "") != 0) return TKC_EXIT_USAGE;
    PkgManifest *m = calloc(1, sizeof *m), *dm = calloc(1, sizeof *dm);
    PkgLock *lock = calloc(1, sizeof *lock);
    int rc = 1;
    if (!m || !dm || !lock) goto out;
    if (load_manifest(PKG_MANIFEST, m) != 0) goto out;
    int r = load_lock_opt(lock);
    if (r < 0) goto out;
    if (r == 0) {
        pdiag(DIAG_ERROR, E9028, PKG_LOCKFILE, 0, "no pkg.lock; run `tkc pkg resolve` first");
        goto out;
    }
    char cache[PKG_PATH];
    const char *home = getenv("HOME");
    int have_cache = home && home[0] == '/' &&
        (size_t)snprintf(cache, sizeof cache, "%s/.toke/pkg", home) < sizeof cache;

    for (int pass = 0; pass < 2; pass++) {
        int shown = 0;
        for (int i = 0; i < lock->count; i++) {
            const PkgResolved *e = &lock->entries[i];
            int direct = 0;
            for (int k = 0; k < m->dep_count; k++)
                if (!strcmp(m->deps[k].name, e->name)) direct = 1;
            if (direct != (pass == 0)) continue;
            if (!shown++) printf("%s:\n", pass == 0 ? "direct" : "transitive");
            char v[PKG_MAX_VERSION], via[PKG_MSG] = "";
            vfmt(e->version, v);
            /* (via X): the locked packages whose cached manifests require it */
            for (int j = 0; pass == 1 && have_cache && j < lock->count; j++) {
                char dir[PKG_PATH], p[PKG_PATH], err[PKG_MSG];
                cache_dir(cache, lock->entries[j].name, lock->entries[j].version, dir, sizeof dir);
                snprintf(p, sizeof p, "%s/%s", dir, PKG_MANIFEST);
                if (!file_exists(p) || pkg_manifest_parse(p, dm, err, (int)sizeof err) != 0)
                    continue;
                for (int k = 0; k < dm->dep_count; k++)
                    if (!strcmp(dm->deps[k].name, e->name)) {
                        strncat(via, via[0] ? ", " : "", sizeof via - strlen(via) - 1);
                        strncat(via, lock->entries[j].name, sizeof via - strlen(via) - 1);
                    }
            }
            if (via[0]) printf("  %-14s %s  (via %s)\n", e->name, v, via);
            else        printf("  %-14s %s\n", e->name, v);
        }
    }
    if (lock->count == 0) printf("no dependencies\n");
    rc = 0;
out:
    free(m); free(dm); free(lock);
    return rc;
}

int
pkg_cli_main(int argc, char **argv)
{
    diag_set_format(DIAG_FMT_JSON);
    if (argc < 1) {
        pdiag(DIAG_ERROR, E9031, "", 0,
              "usage: tkc pkg <init|add|remove|resolve|fetch|list> [args]");
        return TKC_EXIT_USAGE;
    }
    const char *cmd = argv[0];
    PkgArgs a;
    int is_add = !strcmp(cmd, "add");
    if (!strcmp(cmd, "init") || is_add || !strcmp(cmd, "remove") ||
        !strcmp(cmd, "resolve") || !strcmp(cmd, "fetch") || !strcmp(cmd, "list")) {
        if (parse_args(argc, argv, is_add, &a) != 0) return TKC_EXIT_USAGE;
    } else {
        pdiag(DIAG_ERROR, E9031, "", 0,
              "unknown command 'tkc pkg %s'; the commands are init, add, remove, "
              "resolve, fetch and list", cmd);
        return TKC_EXIT_USAGE;
    }
    if (!strcmp(cmd, "init"))    return cmd_init(&a);
    if (is_add)                  return cmd_add_remove(&a, 1);
    if (!strcmp(cmd, "remove"))  return cmd_add_remove(&a, 0);
    if (!strcmp(cmd, "resolve")) return cmd_resolve_fetch(&a, 0);
    if (!strcmp(cmd, "fetch"))   return cmd_resolve_fetch(&a, 1);
    return cmd_list(&a);
}

/* ── Build integration (76.1.3c) ─────────────────────────────────────────── */

static int      s_bp_code = 0;           /* 0 = ready */
static char     s_bp_msg[PKG_MSG * 2];
static int      s_bp_prepared = 0;
static PkgLock  s_bp_lock;
static char     s_bp_tki[PKG_PATH];
static char   **s_bp_srcs = NULL;
static int      s_bp_nsrc = 0;

static int
bp_fail(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_bp_msg, sizeof s_bp_msg, fmt, ap);
    va_end(ap);
    s_bp_code = code;
    return -1;
}

static void
bp_cleanup(void)
{
    if (s_bp_tki[0]) pkg_rmtree(s_bp_tki);
    for (int i = 0; i < s_bp_nsrc; i++) free(s_bp_srcs[i]);
    free(s_bp_srcs);
    s_bp_srcs = NULL; s_bp_nsrc = 0;
}

static int
cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Root-level *.tk files of a package directory, sorted. */
static int
list_modules(const char *dir, char ***out, int *n)
{
    *out = NULL; *n = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    int cap = 0;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 4 || strcmp(de->d_name + l - 3, ".tk") || de->d_name[0] == '.') continue;
        char p[PKG_PATH];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 8;
            char **nv = realloc(*out, (size_t)cap * sizeof **out);
            if (!nv) { closedir(d); return -1; }
            *out = nv;
        }
        (*out)[(*n)++] = strdup(de->d_name);
    }
    closedir(d);
    if (*n > 1) qsort(*out, (size_t)*n, sizeof **out, cmp_str);
    return 0;
}

/* Does the lock still satisfy every direct requirement of the manifest? */
static const char *
lock_stale(const PkgManifest *m, const PkgLock *lock)
{
    for (int k = 0; k < m->dep_count; k++) {
        const PkgDep *d = &m->deps[k];
        const PkgResolved *e = NULL;
        for (int i = 0; i < lock->count; i++)
            if (!strcmp(lock->entries[i].name, d->name)) e = &lock->entries[i];
        if (!e || !dep_admits(d, e->version) || (d->git[0] && strcmp(d->git, e->source)))
            return d->name;
    }
    return NULL;
}

int
pkg_build_prepare(PkgEmitIfaceFn emit, void *ud)
{
    if (s_bp_prepared) return s_bp_code ? -1 : 0;
    s_bp_prepared = 1;
    atexit(bp_cleanup);

    char cwd[PKG_PATH], err[PKG_MSG];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    if (!file_exists(PKG_MANIFEST))
        return bp_fail(E9011, "pkg.* import, but there is no pkg.toml in %s "
                       "(the project root; create one with `tkc pkg init`)", cwd);
    PkgManifest *m = calloc(1, sizeof *m);
    if (!m) return bp_fail(E9032, "out of memory");
    if (pkg_manifest_parse(PKG_MANIFEST, m, err, (int)sizeof err) != 0) {
        free(m);
        return bp_fail(E9012, "malformed pkg.toml: %s", err);
    }
    if (!file_exists(PKG_LOCKFILE)) {
        free(m);
        return bp_fail(E9028, "pkg.* import, but there is no pkg.lock; run `tkc pkg fetch`");
    }
    if (pkg_lock_read(PKG_LOCKFILE, &s_bp_lock) != 0) {
        free(m);
        return bp_fail(E9028, "pkg.lock is malformed%s", pkg_allow_file_sources() ? "" :
                       " (a file:// source needs --allow-file-source)");
    }
    const char *stale = lock_stale(m, &s_bp_lock);
    free(m);
    if (stale)
        pdiag(DIAG_WARNING, W9010, PKG_LOCKFILE, 0,
              "pkg.lock is out of date relative to pkg.toml ('%s'); run `tkc pkg fetch`", stale);
    if (pkg_lock_check_pinned(&s_bp_lock, err, (int)sizeof err) != 0)
        return bp_fail(E9029, "%s; run `tkc pkg fetch`", err);

    char cache[PKG_PATH];
    const char *home = getenv("HOME");
    if (!home || home[0] != '/' ||
        (size_t)snprintf(cache, sizeof cache, "%s/.toke/pkg", home) >= sizeof cache)
        return bp_fail(E9032, "the package cache is $HOME/.toke/pkg, and HOME is not an absolute path");

    /* The cache is not trusted: every package is re-hashed on every build. */
    for (int i = 0; i < s_bp_lock.count; i++) {
        const PkgResolved *e = &s_bp_lock.entries[i];
        char dir[PKG_PATH], v[PKG_MAX_VERSION], got[PKG_MAX_COMMIT];
        cache_dir(cache, e->name, e->version, dir, sizeof dir);
        vfmt(e->version, v);
        if (!dir_exists(dir) || read_sidecar(dir, got) != 0)
            return bp_fail(E9029, "package '%s' %s is not in the package cache; run `tkc pkg fetch`",
                           e->name, v);
        if (pkg_verify_fetched(e, got, dir, err, (int)sizeof err) != 0)
            return bp_fail(E9030, "cached package '%s' %s does not match pkg.lock: %s",
                           e->name, v, err);
    }

    /* Interfaces are regenerated from source, never taken from the fetched
     * tree (ADR-0004 section 4), into a private directory. */
    const char *tmpd = getenv("TMPDIR");
    snprintf(s_bp_tki, sizeof s_bp_tki, "%s/tkc_pkg_XXXXXX",
             tmpd && tmpd[0] ? tmpd : "/tmp");
    if (!mkdtemp(s_bp_tki)) {
        s_bp_tki[0] = '\0';
        return bp_fail(E9032, "cannot create a temporary directory for package interfaces");
    }

    /* Pending modules: source path, output dir, expected .tki. */
    typedef struct {
        char src[PKG_PATH], outdir[PKG_PATH], tki[PKG_PATH], mod[PKG_PATH];
        int  done;
    } Mod;
    Mod *mods = NULL;
    int nmod = 0, cap = 0, rc = -1;
    for (int i = 0; i < s_bp_lock.count; i++) {
        const PkgResolved *e = &s_bp_lock.entries[i];
        char dir[PKG_PATH], v[PKG_MAX_VERSION];
        cache_dir(cache, e->name, e->version, dir, sizeof dir);
        vfmt(e->version, v);
        char **files; int nf;
        if (list_modules(dir, &files, &nf) != 0) {
            bp_fail(E9032, "cannot read package directory %s", dir);
            goto done;
        }
        char entry[PKG_MAX_NAME + 4];
        snprintf(entry, sizeof entry, "%s.tk", e->name);
        int has_entry = 0;
        for (int k = 0; k < nf; k++) if (!strcmp(files[k], entry)) has_entry = 1;
        for (int k = 0; k < nf; k++) {
            if (nmod == cap) {
                cap = cap ? cap * 2 : 16;
                Mod *nm = realloc(mods, (size_t)cap * sizeof *mods);
                if (!nm) { bp_fail(E9032, "out of memory"); goto files_done; }
                mods = nm;
            }
            Mod *md = &mods[nmod++];
            char stem[PKG_PATH];
            snprintf(stem, sizeof stem, "%.*s", (int)(strlen(files[k]) - 3), files[k]);
            /* A package module declares its import path, as every toke
             * module does: m=pkg.<name>; for the entry module <name>.tk,
             * m=pkg.<name>.<file>; for the others.  Its interface is then
             * <that path>.tki, found by the importer's dotted-name lookup,
             * and its symbols are mangled under pkg_<name>_ -- so two
             * packages cannot collide. */
            snprintf(md->src, sizeof md->src, "%s/%s", dir, files[k]);
            snprintf(md->outdir, sizeof md->outdir, "%s", s_bp_tki);
            if (!strcmp(files[k], entry))
                snprintf(md->mod, sizeof md->mod, "pkg.%s", e->name);
            else
                snprintf(md->mod, sizeof md->mod, "pkg.%s.%s", e->name, stem);
            snprintf(md->tki, sizeof md->tki, "%s/%s.tki", md->outdir, md->mod);
            md->done = 0;
        }
        for (int k = 0; k < nf; k++) free(files[k]);
        free(files);
        if (!has_entry) {
            bp_fail(E9035, "package '%s' %s has no entry module %s", e->name, v, entry);
            goto done;
        }
        continue;
files_done:
        for (int k = 0; k < nf; k++) free(files[k]);
        free(files);
        goto done;
    }

    /* A module importing another package module needs that module's
     * interface first: emit in rounds until none is left or a round makes
     * no progress, then show the first failure's own diagnostics. */
    for (int left = nmod; left > 0; ) {
        int progress = 0;
        for (int k = 0; k < nmod; k++) {
            if (mods[k].done) continue;
            if (emit(mods[k].src, mods[k].outdir, s_bp_tki, 1, ud) != 0) continue;
            if (!file_exists(mods[k].tki)) {
                bp_fail(E9035, "package module %s must declare its import path: m=%s;",
                        mods[k].src, mods[k].mod);
                goto done;
            }
            mods[k].done = 1; progress = 1; left--;
        }
        if (!progress) {
            for (int k = 0; k < nmod; k++)
                if (!mods[k].done) {
                    (void)emit(mods[k].src, mods[k].outdir, s_bp_tki, 0, ud);
                    bp_fail(E9035, "package module %s does not compile", mods[k].src);
                    goto done;
                }
        }
    }

    s_bp_srcs = calloc((size_t)(nmod ? nmod : 1), sizeof *s_bp_srcs);
    if (!s_bp_srcs) { bp_fail(E9032, "out of memory"); goto done; }
    for (int k = 0; k < nmod; k++) {
        s_bp_srcs[k] = strdup(mods[k].src);
        if (!s_bp_srcs[k]) { bp_fail(E9032, "out of memory"); goto done; }
        s_bp_nsrc = k + 1;
    }
    rc = 0;
done:
    free(mods);
    if (rc != 0 && !s_bp_code) bp_fail(E9032, "package preparation failed");
    return rc;
}

const char *
pkg_build_tki_dir(void)
{
    return (s_bp_prepared && !s_bp_code && s_bp_tki[0]) ? s_bp_tki : NULL;
}

const char *const *
pkg_build_sources(int *n)
{
    *n = (s_bp_prepared && !s_bp_code) ? s_bp_nsrc : 0;
    return (const char *const *)s_bp_srcs;
}

int
pkg_build_import_check(const char *name, char *msg, int msgsz)
{
    if (!s_bp_prepared) return 0;
    if (s_bp_code) {
        snprintf(msg, (size_t)msgsz, "%s", s_bp_msg);
        return s_bp_code;
    }
    if (!name || !name[0]) {
        snprintf(msg, (size_t)msgsz, "import path 'pkg' names no package: write pkg.<name>");
        return E9011;
    }
    for (int i = 0; i < s_bp_lock.count; i++)
        if (!strcmp(s_bp_lock.entries[i].name, name)) return 0;
    snprintf(msg, (size_t)msgsz,
             "package '%s' is not a dependency: it is not in pkg.lock "
             "(add it with `tkc pkg add %s <constraint> --git <url>`, then `tkc pkg fetch`)",
             name, name);
    return E9011;
}

int
pkg_build_sources_mention_pkg(const char *const *files, int n)
{
    for (int i = 0; i < n; i++) {
        /* Only regular files are scanned: a FIFO source (121.85, C210/C700)
         * can be read once, by the driver, and reading it here would drain
         * it or block on a writerless pipe. */
        struct stat st;
        if (stat(files[i], &st) != 0 || !S_ISREG(st.st_mode)) continue;
        char *t = read_text(files[i], (size_t)64 << 20);
        if (!t) continue;
        int hit = strstr(t, "pkg.") != NULL;
        free(t);
        if (hit) return 1;
    }
    return 0;
}
