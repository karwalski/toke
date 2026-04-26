/*
 * env.c — Implementation of the std.env standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.
 *
 * POSIX APIs used: getenv, setenv, unsetenv, environ.
 *
 * Story: 2.7.2  Branch: feature/stdlib-2.7-process-env
 * Story: 28.5.1 env_list, env_delete, env_expand, env_file_load
 */

#include "env.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

/* POSIX: all environment strings are accessible via environ */
extern char **environ;

/*
 * Validate that a key is non-empty and contains no '=' or NUL bytes.
 * setenv(3) has undefined behaviour with such keys.
 */
static int key_is_valid(const char *key)
{
    if (!key || *key == '\0') return 0;
    for (const char *p = key; *p; p++) {
        if (*p == '=') return 0;
    }
    return 1;
}

EnvGetResult env_get(const char *key)
{
    EnvGetResult r = {NULL, 0, ENV_ERR_NONE, NULL};

    if (!key_is_valid(key)) {
        r.is_err  = 1;
        r.err_kind = ENV_ERR_INVALID;
        r.err_msg  = "invalid environment variable key";
        return r;
    }

    const char *val = getenv(key);
    if (!val) {
        r.is_err  = 1;
        r.err_kind = ENV_ERR_NOT_FOUND;
        r.err_msg  = "environment variable not found";
        return r;
    }

    r.ok = val;
    return r;
}

const char *env_get_or(const char *key, const char *default_val)
{
    if (!key_is_valid(key)) return default_val;
    const char *val = getenv(key);
    return val ? val : default_val;
}

int64_t env_getint(const char *key, int64_t default_val)
{
    const char *val = getenv(key);
    if (!val || !*val) return default_val;
    char *end;
    long long result = strtoll(val, &end, 10);
    if (*end != '\0') return default_val;
    return (int64_t)result;
}

int env_set(const char *key, const char *val)
{
    if (!key_is_valid(key)) return 0;
    if (!val) return 0;
    /* setenv overwrites existing value (overwrite=1) */
    return setenv(key, val, 1) == 0 ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * env_list
 * Returns all environment variable names (not values) as a heap-allocated
 * EnvStrArray.  Each name string is separately heap-allocated; caller frees.
 * ------------------------------------------------------------------------- */
EnvStrArray env_list(void)
{
    EnvStrArray result;
    result.data = NULL;
    result.len  = 0;

    if (!environ) return result;

    /* First pass: count entries */
    uint64_t count = 0;
    for (char **e = environ; *e; e++) count++;

    if (count == 0) return result;

    result.data = (const char **)malloc(count * sizeof(const char *));
    if (!result.data) return result;

    /* Second pass: copy names (up to '=') */
    uint64_t i = 0;
    for (char **e = environ; *e && i < count; e++) {
        const char *entry = *e;
        const char *eq    = strchr(entry, '=');
        size_t      namelen = eq ? (size_t)(eq - entry) : strlen(entry);
        char       *name    = (char *)malloc(namelen + 1);
        if (!name) {
            /* Free already-allocated names and bail */
            for (uint64_t j = 0; j < i; j++) free((void *)result.data[j]);
            free(result.data);
            result.data = NULL;
            result.len  = 0;
            return result;
        }
        memcpy(name, entry, namelen);
        name[namelen] = '\0';
        result.data[i++] = name;
    }
    result.len = i;
    return result;
}

/* -------------------------------------------------------------------------
 * env_delete
 * Unsets the named environment variable via unsetenv(3).
 * Returns 1 on success, 0 on failure.
 * ------------------------------------------------------------------------- */
int env_delete(const char *key)
{
    if (!key_is_valid(key)) return 0;
    return unsetenv(key) == 0 ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * env_expand
 * Substitute $VAR, ${VAR}, and $$ in tmpl.
 * Returns a heap-allocated NUL-terminated string; caller must free.
 * ------------------------------------------------------------------------- */
char *env_expand(const char *tmpl)
{
    if (!tmpl) return NULL;

    /* Build result in a dynamically-sized buffer */
    size_t cap = strlen(tmpl) * 2 + 64;
    char  *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    size_t out = 0;  /* index of next write position */
    const char *p = tmpl;

    /* Helper macro: ensure at least `need` free bytes in buf */
#define ENSURE(need) \
    do { \
        if (out + (need) + 1 > cap) { \
            cap = (out + (need) + 1) * 2; \
            char *_tmp = (char *)realloc(buf, cap); \
            if (!_tmp) { free(buf); return NULL; } \
            buf = _tmp; \
        } \
    } while (0)

    while (*p) {
        if (*p != '$') {
            ENSURE(1);
            buf[out++] = *p++;
            continue;
        }

        /* p points at '$' */
        p++; /* skip '$' */

        if (*p == '$') {
            /* $$ → literal '$' */
            ENSURE(1);
            buf[out++] = '$';
            p++;
            continue;
        }

        if (*p == '{') {
            /* ${VARNAME} — read until '}' */
            p++; /* skip '{' */
            const char *start = p;
            while (*p && *p != '}') p++;
            size_t namelen = (size_t)(p - start);
            if (*p == '}') p++; /* skip '}' */

            /* Copy name, look up */
            char *name = (char *)malloc(namelen + 1);
            if (!name) { free(buf); return NULL; }
            memcpy(name, start, namelen);
            name[namelen] = '\0';
            const char *val = getenv(name);
            free(name);
            if (!val) val = "";
            size_t vlen = strlen(val);
            ENSURE(vlen);
            memcpy(buf + out, val, vlen);
            out += vlen;
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            /* $VARNAME — ends at first non-[A-Za-z0-9_] */
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t namelen = (size_t)(p - start);
            char *name = (char *)malloc(namelen + 1);
            if (!name) { free(buf); return NULL; }
            memcpy(name, start, namelen);
            name[namelen] = '\0';
            const char *val = getenv(name);
            free(name);
            if (!val) val = "";
            size_t vlen = strlen(val);
            ENSURE(vlen);
            memcpy(buf + out, val, vlen);
            out += vlen;
            continue;
        }

        /* '$' not followed by a recognised sequence — emit literally */
        ENSURE(1);
        buf[out++] = '$';
    }

#undef ENSURE

    buf[out] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * Helpers for env_file_load
 * ------------------------------------------------------------------------- */

/* Strip leading and trailing ASCII whitespace in-place; returns s. */
static char *trim_inplace(char *s)
{
    /* Leading */
    while (*s && isspace((unsigned char)*s)) s++;
    /* Trailing */
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

/*
 * Process escape sequences (\n \t \\) inside a double-quoted value.
 * Writes result into dst (must be at least strlen(src)+1 bytes).
 */
static void unescape_dq(const char *src, char *dst)
{
    const char *p = src;
    char       *q = dst;
    while (*p) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n':  *q++ = '\n'; break;
                case 't':  *q++ = '\t'; break;
                case '\\': *q++ = '\\'; break;
                default:   *q++ = '\\'; *q++ = *p; break;
            }
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
}

/* -------------------------------------------------------------------------
 * env_file_load
 * Parse a KEY=VALUE dotenv file; set each pair via setenv(3).
 * Returns count of vars set, or -1 on file-open failure.
 * ------------------------------------------------------------------------- */
int env_file_load(const char *path)
{
    if (!path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int count = 0;
    char line[4096];

    while (fgets(line, (int)sizeof(line), f)) {
        /* Strip trailing newline */
        size_t linelen = strlen(line);
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                                line[linelen - 1] == '\r')) {
            line[--linelen] = '\0';
        }

        char *p = trim_inplace(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#') continue;

        /* Find the '=' separator */
        char *eq = strchr(p, '=');
        if (!eq) continue;  /* malformed line — no '=' */

        /* Split into key and raw value */
        *eq = '\0';
        char *key   = trim_inplace(p);
        char *raw   = eq + 1;  /* NOT trimmed yet — may be quoted */

        if (!key_is_valid(key)) continue;

        /* Determine value: quoted or plain */
        char processed[4096];
        if (*raw == '"') {
            /* Double-quoted: find closing '"' */
            raw++;  /* skip opening '"' */
            char *closing = strrchr(raw, '"');
            if (closing) *closing = '\0';
            unescape_dq(raw, processed);
        } else if (*raw == '\'') {
            /* Single-quoted: no escape processing */
            raw++;  /* skip opening '\'' */
            char *closing = strrchr(raw, '\'');
            if (closing) *closing = '\0';
            /* Copy verbatim */
            size_t vlen = strlen(raw);
            if (vlen >= sizeof(processed)) vlen = sizeof(processed) - 1;
            memcpy(processed, raw, vlen);
            processed[vlen] = '\0';
        } else {
            /* Unquoted: trim whitespace */
            char *trimmed = trim_inplace(raw);
            size_t vlen = strlen(trimmed);
            if (vlen >= sizeof(processed)) vlen = sizeof(processed) - 1;
            memcpy(processed, trimmed, vlen);
            processed[vlen] = '\0';
        }

        if (setenv(key, processed, 1) == 0) count++;
    }

    fclose(f);
    return count;
}
