#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <sys/stat.h>
#include <unistd.h>

#include "pkg.h"
#include "sha256.h"

/*
 * pkg.c — package manifest parser, MVS resolver, and lock file I/O.
 *
 * Story 76.1.3a — Minimum Version Selection resolver and pkg.toml parser.
 *
 * The TOML parser is intentionally minimal: it handles only [section] headers
 * and key = "value" pairs.  This is sufficient for pkg.toml and pkg.lock.
 */

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void
errf(char *buf, int bufsz, const char *fmt, ...)
{
    if (!buf || bufsz <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)bufsz, fmt, ap);
    va_end(ap);
}

static const char *
skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ── SemVer ────────────────────────────────────────────────────────────────── */

/* version_core — parse MAJOR.MINOR.PATCH at s (SemVer 2.0 section 2:
 * non-negative integers without leading zeros).  Returns the first byte after
 * it, or NULL.  121.46: each component is bounded, so a long digit run is
 * refused rather than overflowing (it was signed overflow). */
static const char *
version_core(const char *p, PkgVersion *out)
{
    int part[3];
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            if (*p != '.') return NULL;
            p++;
        }
        if (!isdigit((unsigned char)*p)) return NULL;
        if (p[0] == '0' && isdigit((unsigned char)p[1])) return NULL;
        long long v = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            if (v > PKG_VERSION_MAX) return NULL;
            p++;
        }
        part[i] = (int)v;
    }
    out->major = part[0];
    out->minor = part[1];
    out->patch = part[2];
    return p;
}

/* semver_idents — dot-separated non-empty identifiers of [0-9A-Za-z-]; a
 * pre-release identifier that is numeric has no leading zero (SemVer 2.0
 * sections 9 and 10).  Returns the first byte after them, or NULL. */
static const char *
semver_idents(const char *p, bool prerelease)
{
    for (;;) {
        const char *b = p;
        bool numeric = true;
        while (isalnum((unsigned char)*p) || *p == '-') {
            if (!isdigit((unsigned char)*p)) numeric = false;
            p++;
        }
        if (p == b) return NULL;
        if (prerelease && numeric && p - b > 1 && b[0] == '0') return NULL;
        if (*p != '.') return p;
        p++;
    }
}

bool
pkg_version_parse(const char *s, PkgVersion *out)
{
    if (!s || !out) return false;
    PkgVersion v;
    const char *p = version_core(s, &v);
    if (!p) return false;
    if (*p == '-' && !(p = semver_idents(p + 1, true))) return false;
    if (*p == '+' && !(p = semver_idents(p + 1, false))) return false;
    if (*p != '\0') return false;
    *out = v;
    return true;
}

int
pkg_version_cmp(PkgVersion a, PkgVersion b)
{
    if (a.major != b.major) return a.major - b.major;
    if (a.minor != b.minor) return a.minor - b.minor;
    return a.patch - b.patch;
}

void
pkg_version_fmt(PkgVersion v, char *buf, int bufsz)
{
    snprintf(buf, (size_t)bufsz, "%d.%d.%d", v.major, v.minor, v.patch);
}

/* ── TOML subset parser ────────────────────────────────────────────────────── */

/*
 * States for the mini TOML parser.  We only need to know which [section]
 * we are in, then collect key = "value" pairs.
 */
typedef enum {
    SECT_NONE,
    SECT_PACKAGE,
    SECT_DEPENDENCIES,
    SECT_RESOLVED,
    SECT_COMMIT,
    SECT_HASH,
    SECT_SOURCE,
    SECT_OTHER
} TomlSection;

static TomlSection
parse_section(const char *line)
{
    const char *p = skip_ws(line);
    if (*p != '[') return SECT_OTHER;
    p++;
    const char *end = strchr(p, ']');
    if (!end) return SECT_OTHER;

    int len = (int)(end - p);
    if (len == 7 && strncmp(p, "package", 7) == 0) return SECT_PACKAGE;
    if (len == 12 && strncmp(p, "dependencies", 12) == 0) return SECT_DEPENDENCIES;
    if (len == 8 && strncmp(p, "resolved", 8) == 0) return SECT_RESOLVED;
    if (len == 6 && strncmp(p, "commit", 6) == 0) return SECT_COMMIT;
    if (len == 4 && strncmp(p, "hash", 4) == 0) return SECT_HASH;
    if (len == 6 && strncmp(p, "source", 6) == 0) return SECT_SOURCE;
    return SECT_OTHER;
}

/*
 * Parse key = "value" from a line.
 * Returns true if successful; key and value are written into provided buffers.
 */
static bool
parse_kv(const char *line, char *key, int keysz, char *val, int valsz)
{
    const char *p = skip_ws(line);
    if (*p == '\0' || *p == '#' || *p == '[') return false;

    /* key */
    const char *ks = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '=') p++;
    int klen = (int)(p - ks);
    if (klen <= 0 || klen >= keysz) return false;
    memcpy(key, ks, (size_t)klen);
    key[klen] = '\0';

    /* = */
    p = skip_ws(p);
    if (*p != '=') return false;
    p++;
    p = skip_ws(p);

    /* value: "..."  -- 121.39: closed by a quote, followed by nothing but
     * whitespace or a comment.  `web = ">=1.0.0" ; rm -rf ~` used to read as
     * ">=1.0.0" with the rest ignored, and an unclosed quote as a value. */
    if (*p == '"') {
        p++;
        const char *vs = p;
        while (*p && *p != '"') p++;
        if (*p != '"') return false;
        int vlen = (int)(p - vs);
        if (vlen >= valsz) return false;
        p = skip_ws(p + 1);
        if (*p != '\0' && *p != '#') return false;
        memcpy(val, vs, (size_t)vlen);
        val[vlen] = '\0';
        return true;
    }

    /* bare value (e.g. unquoted — not standard TOML but tolerate for numbers) */
    const char *vs = p;
    while (*p && *p != '\n' && *p != '\r' && *p != '#') p++;
    /* trim trailing whitespace */
    while (p > vs && (p[-1] == ' ' || p[-1] == '\t')) p--;
    int vlen = (int)(p - vs);
    if (vlen >= valsz) vlen = valsz - 1;
    memcpy(val, vs, (size_t)vlen);
    val[vlen] = '\0';
    return true;
}

/*
 * parse_constraint — one of the three documented forms (package-manifest.md,
 * "Version Constraint Syntax"):  "x.y.z" (exactly), ">=x.y.z", and
 * ">=x.y.z <a.b.c" with a.b.c above x.y.z.  Versions here are releases:
 * no pre-release or build suffix.  121.46: anything else is refused -- the
 * version used to be read up to the first space and the rest ignored, so
 * the upper bound in ">=1.2.0 <2.0.0" was never seen.
 */
static bool
parse_constraint(const char *s, PkgDep *d)
{
    const char *p = skip_ws(s);
    d->exact = false;
    d->has_below = false;
    if (p[0] == '>' && p[1] == '=') {
        p = skip_ws(p + 2);
        if (!(p = version_core(p, &d->minimum))) return false;
        const char *q = skip_ws(p);
        if (*q == '<' && q != p) {
            if (!(p = version_core(skip_ws(q + 1), &d->below))) return false;
            if (pkg_version_cmp(d->below, d->minimum) <= 0) return false;
            d->has_below = true;
        }
    } else {
        if (!(p = version_core(p, &d->minimum))) return false;
        d->exact = true;
    }
    return *skip_ws(p) == '\0';
}

/* ── Manifest parser ───────────────────────────────────────────────────────── */

/* find_dep — index of the dependency called name, or -1. */
static int
find_dep(const PkgManifest *m, const char *name)
{
    for (int i = 0; i < m->dep_count; i++)
        if (strcmp(m->deps[i].name, name) == 0) return i;
    return -1;
}

/* add_dep — append a dependency, refusing a bad name or a second entry. */
static PkgDep *
add_dep(PkgManifest *out, const char *name, int lineno,
        char *errbuf, int errbufsz)
{
    if (!pkg_name_valid(name)) {
        errf(errbuf, errbufsz,
             "line %d: invalid dependency name '%s' "
             "(want [a-z][a-z0-9]*, not reserved)", lineno, name);
        return NULL;
    }
    if (find_dep(out, name) >= 0) {
        errf(errbuf, errbufsz, "line %d: dependency '%s' is declared twice",
             lineno, name);
        return NULL;
    }
    if (out->dep_count >= PKG_MAX_DEPS) {
        errf(errbuf, errbufsz, "line %d: too many dependencies (max %d)",
             lineno, PKG_MAX_DEPS);
        return NULL;
    }
    PkgDep *d = &out->deps[out->dep_count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", name);
    return d;
}

/* The [dependencies.<name>] table being read, and whether it has had its
 * `version` yet. */
typedef struct { PkgDep *dep; int has_version; int line; } DepTable;

static int
dep_table_close(const DepTable *t, char *errbuf, int errbufsz)
{
    if (t->dep && !t->has_version) {
        errf(errbuf, errbufsz, "line %d: [dependencies.%s] has no version",
             t->line, t->dep->name);
        return -1;
    }
    return 0;
}

static int
manifest_parse_lines(const char *text, PkgManifest *out,
                     char *errbuf, int errbufsz)
{
    memset(out, 0, sizeof(*out));

    enum { M_NONE, M_PACKAGE, M_DEPS, M_DEPTABLE } sect = M_NONE;
    DepTable tbl = { NULL, 0, 0 };
    const char *p = text;
    int lineno = 0;

    while (*p) {
        lineno++;
        /* Extract one line */
        const char *eol = strchr(p, '\n');
        int llen;
        if (eol) {
            llen = (int)(eol - p);
        } else {
            llen = (int)strlen(p);
        }
        if (llen >= PKG_MAX_LINE) {
            errf(errbuf, errbufsz, "line %d too long", lineno);
            return -1;
        }

        char line[PKG_MAX_LINE];
        memcpy(line, p, (size_t)llen);
        line[llen] = '\0';
        p = eol ? eol + 1 : p + llen;

        /* strip \r */
        if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

        const char *trimmed = skip_ws(line);

        /* skip blank lines and comments */
        if (*trimmed == '\0' || *trimmed == '#') continue;

        /* section header?  121.39: the schema is closed -- an unknown table
         * (a [scripts] of install hooks, say) is an error, not ignored. */
        if (*trimmed == '[') {
            if (dep_table_close(&tbl, errbuf, errbufsz)) return -1;
            tbl.dep = NULL;
            const char *end = strchr(trimmed, ']');
            const char *rest = end ? skip_ws(end + 1) : NULL;
            if (!end || trimmed[1] == '[' || (*rest != '\0' && *rest != '#')) {
                errf(errbuf, errbufsz, "line %d: malformed table header", lineno);
                return -1;
            }
            size_t n = (size_t)(end - trimmed - 1);
            const char *h = trimmed + 1;
            if (n == 7 && strncmp(h, "package", 7) == 0) {
                sect = M_PACKAGE;
            } else if (n == 12 && strncmp(h, "dependencies", 12) == 0) {
                sect = M_DEPS;
            } else if (n > 13 && strncmp(h, "dependencies.", 13) == 0 &&
                       n - 13 < PKG_MAX_NAME) {
                char name[PKG_MAX_NAME];
                memcpy(name, h + 13, n - 13);
                name[n - 13] = '\0';
                tbl.dep = add_dep(out, name, lineno, errbuf, errbufsz);
                if (!tbl.dep) return -1;
                tbl.has_version = 0;
                tbl.line = lineno;
                sect = M_DEPTABLE;
            } else {
                errf(errbuf, errbufsz,
                     "line %d: unknown table [%.*s]: pkg.toml has [package], "
                     "[dependencies] and [dependencies.<name>] only -- a "
                     "package has no install hooks", lineno, (int)n, h);
                return -1;
            }
            continue;
        }

        /* key = "value" -- anything else is an error, not skipped */
        char key[PKG_MAX_NAME];
        char val[PKG_MAX_LINE];
        if (!parse_kv(trimmed, key, (int)sizeof(key), val, (int)sizeof(val))) {
            errf(errbuf, errbufsz, "line %d: expected key = \"value\"", lineno);
            return -1;
        }

        if (sect == M_PACKAGE) {
            if (strcmp(key, "name") == 0) {
                if (!pkg_name_valid(val)) {
                    errf(errbuf, errbufsz,
                         "line %d: invalid package name '%s' "
                         "(want [a-z][a-z0-9]*, not reserved)", lineno, val);
                    return -1;
                }
                snprintf(out->name, sizeof(out->name), "%s", val);
            } else if (strcmp(key, "version") == 0) {
                if (!pkg_version_parse(val, &out->version)) {
                    errf(errbuf, errbufsz,
                         "line %d: invalid version '%s'", lineno, val);
                    return -1;
                }
            } else {
                errf(errbuf, errbufsz,
                     "line %d: unknown key '%s' in [package] (name, version)",
                     lineno, key);
                return -1;
            }
        } else if (sect == M_DEPS) {
            PkgDep *d = add_dep(out, key, lineno, errbuf, errbufsz);
            if (!d) return -1;
            if (!parse_constraint(val, d)) {
                errf(errbuf, errbufsz,
                     "line %d: invalid constraint '%s'", lineno, val);
                return -1;
            }
        } else if (sect == M_DEPTABLE) {
            if (strcmp(key, "version") == 0 && !tbl.has_version) {
                if (!parse_constraint(val, tbl.dep)) {
                    errf(errbuf, errbufsz,
                         "line %d: invalid constraint '%s'", lineno, val);
                    return -1;
                }
                tbl.has_version = 1;
            } else if (strcmp(key, "git") == 0 && !tbl.dep->git[0]) {
                if (!pkg_source_valid(val)) {
                    errf(errbuf, errbufsz,
                         "line %d: '%s': git = \"%s\" is not an https or ssh "
                         "source (ADR-0004: no unauthenticated transport)",
                         lineno, tbl.dep->name, val);
                    return -1;
                }
                snprintf(tbl.dep->git, sizeof(tbl.dep->git), "%s", val);
            } else {
                errf(errbuf, errbufsz,
                     "line %d: unknown or repeated key '%s' in "
                     "[dependencies.%s] (version, git)",
                     lineno, key, tbl.dep->name);
                return -1;
            }
        } else {
            errf(errbuf, errbufsz, "line %d: key '%s' outside any table",
                 lineno, key);
            return -1;
        }
    }

    if (dep_table_close(&tbl, errbuf, errbufsz)) return -1;

    if (out->name[0] == '\0') {
        errf(errbuf, errbufsz, "missing [package] name");
        return -1;
    }

    return 0;
}

int
pkg_manifest_parse_str(const char *toml, PkgManifest *out,
                       char *errbuf, int errbufsz)
{
    if (!toml || !out) return -1;
    return manifest_parse_lines(toml, out, errbuf, errbufsz);
}

int
pkg_manifest_parse(const char *path, PkgManifest *out,
                   char *errbuf, int errbufsz)
{
    if (!path || !out) return -1;

    FILE *f = fopen(path, "r");
    if (!f) {
        errf(errbuf, errbufsz, "cannot open '%s'", path);
        return -1;
    }

    /* read entire file */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0 || sz > 1024 * 1024) {
        fclose(f);
        errf(errbuf, errbufsz, "file too large or unreadable");
        return -1;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    int rc = manifest_parse_lines(buf, out, errbuf, errbufsz);
    free(buf);
    return rc;
}

/* ── MVS resolver ──────────────────────────────────────────────────────────── */

int
pkg_resolve(const PkgDep *deps, int dep_count, PkgLock *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /*
     * For each unique package name, take the maximum of all minimum version
     * requirements.  This is the core MVS rule: select the minimum version
     * that satisfies every constraint.
     */
    for (int i = 0; i < dep_count; i++) {
        /* find existing entry */
        int found = -1;
        for (int j = 0; j < out->count; j++) {
            if (strcmp(out->entries[j].name, deps[i].name) == 0) {
                found = j;
                break;
            }
        }

        /* 121.39: a `git` source is carried into the lock's [source].  Two
         * manifests naming different sources for one package is an error:
         * picking either would let one of them redirect the other. */
        if (deps[i].git[0] && !pkg_source_valid(deps[i].git)) return -1;
        if (found >= 0) {
            /* raise to max of the two minimums */
            if (pkg_version_cmp(deps[i].minimum,
                                out->entries[found].version) > 0) {
                out->entries[found].version = deps[i].minimum;
            }
            PkgResolved *r = &out->entries[found];
            if (deps[i].git[0]) {
                if (r->source[0] && strcmp(r->source, deps[i].git) != 0)
                    return -1;
                snprintf(r->source, sizeof(r->source), "%s", deps[i].git);
            }
        } else {
            if (out->count >= PKG_MAX_DEPS) return -1;
            PkgResolved *r = &out->entries[out->count];
            memset(r, 0, sizeof(*r));
            snprintf(r->name, sizeof(r->name), "%s", deps[i].name);
            r->version = deps[i].minimum;
            snprintf(r->source, sizeof(r->source), "%s", deps[i].git);
            out->count++;
        }
    }

    /* 121.46: MVS picked the highest minimum; it must now satisfy every
     * constraint on that package -- equal to each exact version, below each
     * upper bound.  If it does not, no version does (every other candidate
     * is below some minimum): resolution fails. */
    for (int i = 0; i < dep_count; i++)
        for (int j = 0; j < out->count; j++) {
            if (strcmp(out->entries[j].name, deps[i].name) != 0) continue;
            PkgVersion v = out->entries[j].version;
            if (deps[i].exact && pkg_version_cmp(v, deps[i].minimum) != 0)
                return -1;
            if (deps[i].has_below && pkg_version_cmp(v, deps[i].below) >= 0)
                return -1;
        }

    return 0;
}

/* ── Supply-chain checks (story 122.10) ───────────────────────────────────── */

static bool
all_lower_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool
pkg_name_valid(const char *name)
{
    static const char *const reserved[] = { "std", "pkg", "toke", "tkc", NULL };
    if (!name || !(*name >= 'a' && *name <= 'z')) return false;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
    }
    if (n >= PKG_MAX_NAME) return false;
    for (int i = 0; reserved[i]; i++)
        if (strcmp(name, reserved[i]) == 0) return false;
    return true;
}

bool
pkg_commit_valid(const char *commit)
{
    if (!commit) return false;
    size_t n = strlen(commit);
    if (n != 40 && n != 64) return false;
    if (!all_lower_hex(commit, n)) return false;
    return strspn(commit, "0") != n;               /* the null object id */
}

bool
pkg_hash_valid(const char *hash)
{
    if (!hash || strncmp(hash, "sha256:", 7) != 0) return false;
    return strlen(hash + 7) == 64 && all_lower_hex(hash + 7, 64);
}

/* ── Sources (121.39) ──────────────────────────────────────────────────── */

/* host_ok — [A-Za-z0-9.-]+ not starting with '-' or '.', over s[0..n). */
static bool
host_ok(const char *s, size_t n)
{
    if (n == 0 || s[0] == '-' || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-')) return false;
    }
    return true;
}

/* user_ok — [A-Za-z0-9._-]+ not starting with '-'. */
static bool
user_ok(const char *s, size_t n)
{
    if (n == 0 || s[0] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* port_ok — 1..5 digits. */
static bool
port_ok(const char *s, size_t n)
{
    if (n == 0 || n > 5) return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* path_ok — non-empty, not an option, no "::" left in it. */
static bool
path_ok(const char *s)
{
    return s[0] != '\0' && s[0] != '-' && strstr(s, "::") == NULL;
}

/* 76.1.3d: the explicit local-path override of ADR-0004 section 2.  Off by
 * default; `tkc pkg ... --allow-file-source` (and the same flag on a build)
 * turns it on for that one process. */
static bool s_allow_file_sources = false;

void
pkg_set_allow_file_sources(bool on)
{
    s_allow_file_sources = on;
}

bool
pkg_allow_file_sources(void)
{
    return s_allow_file_sources;
}

bool
pkg_source_valid(const char *url)
{
    if (!url) return false;
    size_t n = strlen(url);
    if (n == 0 || n >= PKG_MAX_URL || url[0] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '\\')
            return false;
    }
    /* git reads "<transport>::<address>" as a remote helper: ext:: runs a
     * command, fd:: reads a descriptor.  Never a package source. */
    if (strstr(url, "::")) return false;

    if (strncmp(url, "https://", 8) == 0) {
        const char *a = url + 8, *slash = strchr(a, '/');
        if (!slash) return false;                        /* no path */
        size_t alen = (size_t)(slash - a);
        if (memchr(a, '@', alen)) return false;          /* credentials */
        const char *colon = memchr(a, ':', alen);
        size_t hlen = colon ? (size_t)(colon - a) : alen;
        if (!host_ok(a, hlen)) return false;
        if (colon && !port_ok(colon + 1, alen - hlen - 1)) return false;
        return path_ok(slash + 1);
    }
    if (strncmp(url, "ssh://", 6) == 0) {
        const char *a = url + 6, *slash = strchr(a, '/');
        if (!slash) return false;
        size_t alen = (size_t)(slash - a);
        const char *at = memchr(a, '@', alen);
        if (at) {
            if (!user_ok(a, (size_t)(at - a))) return false;
            alen -= (size_t)(at + 1 - a);
            a = at + 1;
        }
        const char *colon = memchr(a, ':', alen);
        size_t hlen = colon ? (size_t)(colon - a) : alen;
        if (!host_ok(a, hlen)) return false;
        if (colon && !port_ok(colon + 1, alen - hlen - 1)) return false;
        return path_ok(slash + 1);
    }
    /* 76.1.3d: file:///absolute/path, only under the explicit override.  The
     * character rules above still apply; the path must be absolute and may
     * not contain a ".." segment or begin with '-'. */
    if (s_allow_file_sources && strncmp(url, "file:///", 8) == 0) {
        const char *p = url + 7;                         /* the leading '/' */
        if (p[1] == '\0' || p[1] == '-' || strstr(p, "/../") ||
            (strlen(p) >= 3 && strcmp(p + strlen(p) - 3, "/..") == 0))
            return false;
        return true;
    }
    /* Any other scheme (http, git, file, git+ssh, ...) is refused. */
    if (strstr(url, "://")) return false;

    /* scp form, [user@]host:path, which git sends over ssh.  A local path
     * ("/x", "./x", "../x", "x/y:z") has a '/' before any ':' and is not. */
    const char *colon = strchr(url, ':');
    if (!colon) return false;
    const char *slash = strchr(url, '/');
    if (slash && slash < colon) return false;
    const char *h = url;
    const char *at = memchr(url, '@', (size_t)(colon - url));
    if (at) {
        if (!user_ok(url, (size_t)(at - url))) return false;
        h = at + 1;
    }
    if (!host_ok(h, (size_t)(colon - h))) return false;
    return path_ok(colon + 1);
}

/* entry_pinned — everything a fetch needs to trust an entry (121.39). */
static bool
entry_pinned(const PkgResolved *e)
{
    return pkg_source_valid(e->source) && pkg_commit_valid(e->commit) &&
           pkg_hash_valid(e->hash);
}

int
pkg_lock_check_pinned(const PkgLock *lock, char *errbuf, int errbufsz)
{
    if (!lock) return -1;
    for (int i = 0; i < lock->count; i++) {
        const PkgResolved *e = &lock->entries[i];
        if (!pkg_source_valid(e->source)) {
            errf(errbuf, errbufsz,
                 "pkg.lock: '%s' has no %s source", e->name,
                 s_allow_file_sources ? "https, ssh or file" : "https or ssh");
            return -1;
        }
        if (!pkg_commit_valid(e->commit)) {
            errf(errbuf, errbufsz, "pkg.lock: '%s' is not pinned to a commit",
                 e->name);
            return -1;
        }
        if (!pkg_hash_valid(e->hash)) {
            errf(errbuf, errbufsz, "pkg.lock: '%s' has no content hash",
                 e->name);
            return -1;
        }
    }
    return 0;
}

int
pkg_lock_verify(const PkgResolved *want, const char *got_commit,
                const char *got_hash, char *errbuf, int errbufsz)
{
    if (!want || !got_commit || !got_hash) return -1;
    if (!entry_pinned(want)) {
        errf(errbuf, errbufsz, "pkg.lock: '%s' is not pinned", want->name);
        return -1;
    }
    if (strcmp(want->commit, got_commit) != 0) {
        errf(errbuf, errbufsz, "'%s': commit mismatch: lock has %s, fetched %s",
             want->name, want->commit, got_commit);
        return -1;
    }
    if (strcmp(want->hash, got_hash) != 0) {
        errf(errbuf, errbufsz, "'%s': content hash mismatch: lock has %s, fetched %s",
             want->name, want->hash, got_hash);
        return -1;
    }
    return 0;
}

/* ── Canonical tree hash (121.39, ADR-0004 section 1) ─────────────────── */

#define TREE_MAX_FILES  65536
#define TREE_MAX_DEPTH  64
#define TREE_MAX_PATH   4096

typedef struct {
    char  **paths;          /* relative, '/'-separated */
    size_t  n, cap;
    char   *err;
    int     errsz;
} TreeList;

static int
tree_add(TreeList *t, const char *rel)
{
    if (t->n >= TREE_MAX_FILES) {
        errf(t->err, t->errsz, "tree: more than %d files", TREE_MAX_FILES);
        return -1;
    }
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64;
        char **np = realloc(t->paths, nc * sizeof(*np));
        if (!np) { errf(t->err, t->errsz, "tree: out of memory"); return -1; }
        t->paths = np; t->cap = nc;
    }
    t->paths[t->n] = strdup(rel);
    if (!t->paths[t->n]) { errf(t->err, t->errsz, "tree: out of memory"); return -1; }
    t->n++;
    return 0;
}

/* tree_walk — collect the regular files under root/rel ("" = root). */
static int
tree_walk(TreeList *t, const char *root, const char *rel, int depth)
{
    if (depth > TREE_MAX_DEPTH) {
        errf(t->err, t->errsz, "tree: deeper than %d directories at '%s'",
             TREE_MAX_DEPTH, rel);
        return -1;
    }
    char dpath[TREE_MAX_PATH];
    int dn = snprintf(dpath, sizeof dpath, "%s%s%s", root, *rel ? "/" : "", rel);
    if (dn < 0 || (size_t)dn >= sizeof dpath) {
        errf(t->err, t->errsz, "tree: path too long at '%s'", rel);
        return -1;
    }
    DIR *d = opendir(dpath);
    if (!d) {
        errf(t->err, t->errsz, "tree: cannot open directory '%s': %s",
             *rel ? rel : ".", strerror(errno));
        return -1;
    }
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (strcmp(nm, ".git") == 0) {
            if (!*rel) continue;           /* the checkout's own .git */
            errf(t->err, t->errsz, "tree: nested .git at '%s/.git'", rel);
            rc = -1; break;
        }
        char crel[TREE_MAX_PATH], cpath[TREE_MAX_PATH];
        int a = snprintf(crel, sizeof crel, "%s%s%s", rel, *rel ? "/" : "", nm);
        int b = snprintf(cpath, sizeof cpath, "%s/%s", dpath, nm);
        if (a < 0 || (size_t)a >= sizeof crel || b < 0 || (size_t)b >= sizeof cpath) {
            errf(t->err, t->errsz, "tree: path too long under '%s'", *rel ? rel : ".");
            rc = -1; break;
        }
        struct stat st;
        if (lstat(cpath, &st) != 0) {
            errf(t->err, t->errsz, "tree: cannot stat '%s': %s", crel, strerror(errno));
            rc = -1; break;
        }
        if (S_ISDIR(st.st_mode))      rc = tree_walk(t, root, crel, depth + 1);
        else if (S_ISREG(st.st_mode)) rc = tree_add(t, crel);
        else {
            errf(t->err, t->errsz, "tree: '%s' is a %s, not a regular file",
                 crel, S_ISLNK(st.st_mode) ? "symlink" : "special file");
            rc = -1;
        }
    }
    closedir(d);
    return rc;
}

static int
cmp_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* tree_hash_file — absorb "path NUL size NUL bytes" for one file. */
static int
tree_hash_file(TkSha256 *c, const char *root, const char *rel,
               char *errbuf, int errbufsz)
{
    char path[TREE_MAX_PATH];
    int pn = snprintf(path, sizeof path, "%s/%s", root, rel);
    if (pn < 0 || (size_t)pn >= sizeof path) return -1;
    /* O_NOFOLLOW|O_NONBLOCK: a file swapped for a symlink or a FIFO after
     * the walk is refused here rather than followed or blocked on. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        errf(errbuf, errbufsz, "tree: cannot read '%s': %s", rel, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        errf(errbuf, errbufsz, "tree: '%s' is not a regular file", rel);
        close(fd);
        return -1;
    }
    char sz[32];
    int k = snprintf(sz, sizeof sz, "%lld", (long long)st.st_size);
    tk_sha256_update(c, (const unsigned char *)rel, (unsigned long)strlen(rel) + 1);
    tk_sha256_update(c, (const unsigned char *)sz, (unsigned long)k + 1);
    unsigned char buf[16384];
    long long total = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            errf(errbuf, errbufsz, "tree: reading '%s': %s", rel, strerror(errno));
            close(fd);
            return -1;
        }
        if (r == 0) break;
        total += r;
        if (total > (long long)st.st_size) break;
        tk_sha256_update(c, buf, (unsigned long)r);
    }
    close(fd);
    if (total != (long long)st.st_size) {
        errf(errbuf, errbufsz, "tree: '%s' changed size while it was hashed", rel);
        return -1;
    }
    return 0;
}

int
pkg_tree_hash(const char *dir, char out[PKG_MAX_HASH], char *errbuf, int errbufsz)
{
    if (!dir || !out) return -1;
    out[0] = '\0';
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errf(errbuf, errbufsz, "tree: '%s' is not a directory", dir);
        return -1;
    }
    TreeList t = { NULL, 0, 0, errbuf, errbufsz };
    int rc = tree_walk(&t, dir, "", 0);
    if (rc == 0) {
        /* 127.439: an empty tree has t.paths == NULL, and qsort's array
         * argument must not be NULL even for zero elements. */
        if (t.n > 1)
            qsort(t.paths, t.n, sizeof(*t.paths), cmp_path);
        TkSha256 c;
        tk_sha256_init(&c);
        for (size_t i = 0; rc == 0 && i < t.n; i++)
            rc = tree_hash_file(&c, dir, t.paths[i], errbuf, errbufsz);
        if (rc == 0) {
            unsigned char d[32];
            static const char hx[] = "0123456789abcdef";
            tk_sha256_final(&c, d);
            memcpy(out, "sha256:", 7);
            for (int i = 0; i < 32; i++) {
                out[7 + 2 * i] = hx[d[i] >> 4];
                out[8 + 2 * i] = hx[d[i] & 15];
            }
            out[71] = '\0';
        }
    }
    for (size_t i = 0; i < t.n; i++) free(t.paths[i]);
    free(t.paths);
    return rc;
}

int
pkg_verify_fetched(const PkgResolved *want, const char *got_commit,
                   const char *dir, char *errbuf, int errbufsz)
{
    if (!want || !got_commit || !dir) return -1;
    if (!entry_pinned(want)) {
        errf(errbuf, errbufsz, "pkg.lock: '%s' is not pinned (source, commit, hash)",
             want->name);
        return -1;
    }
    char got_hash[PKG_MAX_HASH];
    if (pkg_tree_hash(dir, got_hash, errbuf, errbufsz) != 0) return -1;
    return pkg_lock_verify(want, got_commit, got_hash, errbuf, errbufsz);
}

/* ── Lock file writer ──────────────────────────────────────────────────────── */

int
pkg_lock_write(const char *path, const PkgLock *lock)
{
    if (!path || !lock || lock->count < 0 || lock->count > PKG_MAX_DEPS)
        return -1;

    /* Validate everything before touching the file: a name is written
     * unquoted as a TOML key, so a name with '"' or a newline would inject
     * lock lines (122.10). */
    for (int i = 0; i < lock->count; i++) {
        const PkgResolved *e = &lock->entries[i];
        if (!pkg_name_valid(e->name)) return -1;
        if (e->commit[0] && !pkg_commit_valid(e->commit)) return -1;
        if (e->hash[0] && !pkg_hash_valid(e->hash)) return -1;
        if (e->source[0] && !pkg_source_valid(e->source)) return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# pkg.lock — auto-generated by tkc pkg resolve\n");
    fprintf(f, "# Do not edit manually.\n");
    fprintf(f, "lock-version = \"2\"\n\n");
    fprintf(f, "[resolved]\n");

    int pinned_commits = 0, pinned_hashes = 0, pinned_sources = 0;
    for (int i = 0; i < lock->count; i++) {
        char vbuf[PKG_MAX_VERSION];
        pkg_version_fmt(lock->entries[i].version, vbuf, (int)sizeof(vbuf));
        fprintf(f, "%s = \"%s\"\n", lock->entries[i].name, vbuf);
        if (lock->entries[i].commit[0]) pinned_commits++;
        if (lock->entries[i].hash[0])   pinned_hashes++;
        if (lock->entries[i].source[0]) pinned_sources++;
    }
    if (pinned_sources) {
        fprintf(f, "\n[source]\n");
        for (int i = 0; i < lock->count; i++)
            if (lock->entries[i].source[0])
                fprintf(f, "%s = \"%s\"\n", lock->entries[i].name,
                        lock->entries[i].source);
    }
    if (pinned_commits) {
        fprintf(f, "\n[commit]\n");
        for (int i = 0; i < lock->count; i++)
            if (lock->entries[i].commit[0])
                fprintf(f, "%s = \"%s\"\n", lock->entries[i].name,
                        lock->entries[i].commit);
    }
    if (pinned_hashes) {
        fprintf(f, "\n[hash]\n");
        for (int i = 0; i < lock->count; i++)
            if (lock->entries[i].hash[0])
                fprintf(f, "%s = \"%s\"\n", lock->entries[i].name,
                        lock->entries[i].hash);
    }

    return fclose(f) == 0 ? 0 : -1;
}

/* ── Lock file reader ──────────────────────────────────────────────────────── */

static PkgResolved *
lock_find(PkgLock *lock, const char *name)
{
    for (int i = 0; i < lock->count; i++)
        if (strcmp(lock->entries[i].name, name) == 0) return &lock->entries[i];
    return NULL;
}

/* Apply one key = "value" line of the lock.  Returns 0, or -1 if malformed. */
static int
lock_apply(PkgLock *out, TomlSection sect, const char *key, const char *val)
{
    if (sect == SECT_NONE) {
        /* only lock-version is defined at top level; 1 = no pins (pre-122.10) */
        if (strcmp(key, "lock-version") != 0) return -1;     /* 121.39 */
        if (strcmp(val, "1") != 0 && strcmp(val, "2") != 0) return -1;
        return 0;
    }
    /* 121.39: a lock has these four tables and no others. */
    if (sect != SECT_RESOLVED && sect != SECT_COMMIT && sect != SECT_HASH &&
        sect != SECT_SOURCE)
        return -1;
    if (!pkg_name_valid(key)) return -1;

    if (sect == SECT_RESOLVED) {
        if (lock_find(out, key)) return -1;               /* duplicate */
        if (out->count >= PKG_MAX_DEPS) return -1;
        PkgResolved *r = &out->entries[out->count];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", key);
        if (!pkg_version_parse(val, &r->version)) return -1;
        out->count++;
        return 0;
    }

    PkgResolved *r = lock_find(out, key);
    if (!r) return -1;                        /* pin for an unresolved name */
    if (sect == SECT_COMMIT) {
        if (r->commit[0] || !pkg_commit_valid(val)) return -1;
        snprintf(r->commit, sizeof(r->commit), "%s", val);
    } else if (sect == SECT_SOURCE) {
        if (r->source[0] || !pkg_source_valid(val)) return -1;
        snprintf(r->source, sizeof(r->source), "%s", val);
    } else {
        if (r->hash[0] || !pkg_hash_valid(val)) return -1;
        snprintf(r->hash, sizeof(r->hash), "%s", val);
    }
    return 0;
}

int
pkg_lock_read(const char *path, PkgLock *out)
{
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    TomlSection sect = SECT_NONE;
    char line[PKG_MAX_LINE];

    while (fgets(line, (int)sizeof(line), f)) {
        /* strip newline */
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        else if (!feof(f)) { fclose(f); return -1; }   /* over-long line */
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        const char *trimmed = skip_ws(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;

        if (*trimmed == '[') {
            sect = parse_section(trimmed);
            if (sect == SECT_OTHER) { fclose(f); return -1; }   /* 121.39 */
            continue;
        }

        char key[PKG_MAX_NAME];
        char val[PKG_MAX_LINE];
        if (!parse_kv(trimmed, key, (int)sizeof(key), val, (int)sizeof(val))) {
            fclose(f);          /* 121.39: a malformed line is never skipped */
            return -1;
        }

        if (lock_apply(out, sect, key, val) != 0) { fclose(f); return -1; }
    }

    fclose(f);
    return 0;
}
