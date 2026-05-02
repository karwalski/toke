#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"

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

bool
pkg_version_parse(const char *s, PkgVersion *out)
{
    if (!s || !out) return false;
    int maj = 0, min = 0, pat = 0;
    int n = 0;
    /* manual parse to avoid sscanf portability quirks */
    const char *p = s;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) { maj = maj * 10 + (*p - '0'); p++; n++; }
    if (*p != '.') return false; p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) { min = min * 10 + (*p - '0'); p++; n++; }
    if (*p != '.') return false; p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) { pat = pat * 10 + (*p - '0'); p++; n++; }
    if (*p != '\0' && *p != '"' && *p != '\n' && *p != '\r' && *p != ' ')
        return false;
    (void)n;
    out->major = maj;
    out->minor = min;
    out->patch = pat;
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

    /* value: "..." */
    if (*p == '"') {
        p++;
        const char *vs = p;
        while (*p && *p != '"') p++;
        int vlen = (int)(p - vs);
        if (vlen >= valsz) vlen = valsz - 1;
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
 * Parse a >=x.y.z constraint string.  Extracts the version after ">=".
 * Returns true on success.
 */
static bool
parse_constraint(const char *s, PkgVersion *out)
{
    const char *p = skip_ws(s);
    if (p[0] == '>' && p[1] == '=') {
        p += 2;
        p = skip_ws(p);
        return pkg_version_parse(p, out);
    }
    /* bare version: treat as exact minimum */
    return pkg_version_parse(p, out);
}

/* ── Manifest parser ───────────────────────────────────────────────────────── */

static int
manifest_parse_lines(const char *text, PkgManifest *out,
                     char *errbuf, int errbufsz)
{
    memset(out, 0, sizeof(*out));

    TomlSection sect = SECT_NONE;
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

        /* strip \r */
        if (llen > 0 && line[llen - 1] == '\r') line[llen - 1] = '\0';

        const char *trimmed = skip_ws(line);

        /* skip blank lines and comments */
        if (*trimmed == '\0' || *trimmed == '#') {
            p = eol ? eol + 1 : p + llen;
            continue;
        }

        /* section header? */
        if (*trimmed == '[') {
            sect = parse_section(trimmed);
            p = eol ? eol + 1 : p + llen;
            continue;
        }

        /* key = "value" */
        char key[PKG_MAX_NAME];
        char val[PKG_MAX_LINE];
        if (!parse_kv(trimmed, key, (int)sizeof(key), val, (int)sizeof(val))) {
            p = eol ? eol + 1 : p + llen;
            continue;
        }

        if (sect == SECT_PACKAGE) {
            if (strcmp(key, "name") == 0) {
                snprintf(out->name, sizeof(out->name), "%s", val);
            } else if (strcmp(key, "version") == 0) {
                if (!pkg_version_parse(val, &out->version)) {
                    errf(errbuf, errbufsz,
                         "line %d: invalid version '%s'", lineno, val);
                    return -1;
                }
            }
        } else if (sect == SECT_DEPENDENCIES) {
            if (out->dep_count >= PKG_MAX_DEPS) {
                errf(errbuf, errbufsz,
                     "line %d: too many dependencies (max %d)",
                     lineno, PKG_MAX_DEPS);
                return -1;
            }
            PkgDep *d = &out->deps[out->dep_count];
            snprintf(d->name, sizeof(d->name), "%s", key);
            if (!parse_constraint(val, &d->minimum)) {
                errf(errbuf, errbufsz,
                     "line %d: invalid constraint '%s'", lineno, val);
                return -1;
            }
            out->dep_count++;
        }

        p = eol ? eol + 1 : p + llen;
    }

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

        if (found >= 0) {
            /* raise to max of the two minimums */
            if (pkg_version_cmp(deps[i].minimum,
                                out->entries[found].version) > 0) {
                out->entries[found].version = deps[i].minimum;
            }
        } else {
            if (out->count >= PKG_MAX_DEPS) return -1;
            PkgResolved *r = &out->entries[out->count];
            snprintf(r->name, sizeof(r->name), "%s", deps[i].name);
            r->version = deps[i].minimum;
            out->count++;
        }
    }

    return 0;
}

/* ── Lock file writer ──────────────────────────────────────────────────────── */

int
pkg_lock_write(const char *path, const PkgLock *lock)
{
    if (!path || !lock) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# pkg.lock — auto-generated by tkc pkg resolve\n");
    fprintf(f, "# Do not edit manually.\n\n");
    fprintf(f, "[resolved]\n");

    for (int i = 0; i < lock->count; i++) {
        char vbuf[PKG_MAX_VERSION];
        pkg_version_fmt(lock->entries[i].version, vbuf, (int)sizeof(vbuf));
        fprintf(f, "%s = \"%s\"\n", lock->entries[i].name, vbuf);
    }

    fclose(f);
    return 0;
}

/* ── Lock file reader ──────────────────────────────────────────────────────── */

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
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        const char *trimmed = skip_ws(line);
        if (*trimmed == '\0' || *trimmed == '#') continue;

        if (*trimmed == '[') {
            sect = parse_section(trimmed);
            continue;
        }

        if (sect != SECT_RESOLVED) continue;

        char key[PKG_MAX_NAME];
        char val[PKG_MAX_LINE];
        if (!parse_kv(trimmed, key, (int)sizeof(key), val, (int)sizeof(val)))
            continue;

        if (out->count >= PKG_MAX_DEPS) { fclose(f); return -1; }

        PkgResolved *r = &out->entries[out->count];
        snprintf(r->name, sizeof(r->name), "%s", key);
        if (!pkg_version_parse(val, &r->version)) { fclose(f); return -1; }
        out->count++;
    }

    fclose(f);
    return 0;
}
