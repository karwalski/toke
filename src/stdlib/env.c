/* SPDX-License-Identifier: MIT */
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
 *
 * 127.295 (S1 step 5, runtime-abi §3.1): every reader measures with
 * tk_strlen, and env_expand returns a sealed string from the string heap
 * (never freed, C callers included).  Not sealed, on purpose:
 *   - env_get / env_get_or: env.tki declares both "ownership": "caller"
 *     (121.78), and a caller that frees an owned result must be able to free
 *     every one (135.23); a sealed string cannot be freed.  Sealing them waits
 *     on what "caller" means for a sealed string.
 *   - env_list: a C-only API (no toke wrapper) whose caller frees each name.
 */

#include "env.h"
#include "tk_strhdr.h"
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
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

/*
 * AMB-04: dangerous keys an untrusted dotenv file must not be allowed to set —
 * they change how child processes load code / resolve commands (code injection).
 * Applies to env_file_load only; an explicit env.set is a deliberate program act.
 */
static int dotenv_key_denied(const char *key)
{
    return strncmp(key, "LD_", 3) == 0 ||
           strncmp(key, "DYLD_", 5) == 0 ||
           strcmp(key, "PATH") == 0 ||
           strcmp(key, "IFS") == 0;
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

    /* 121.78: a copy the caller owns, not getenv()'s pointer into the
     * process environment -- a later env.set of the same name (or another
     * thread's setenv/unsetenv) may overwrite or free that storage, and on
     * Darwin a value read before a second env.set then read the new value.
     * Same defect and fix as os.getenv (121.75). */
    char *copy = strdup(val);
    if (!copy) {
        r.is_err  = 1;
        r.err_kind = ENV_ERR_NOT_FOUND;
        r.err_msg  = "allocation failed copying the environment value";
        return r;
    }
    r.ok = copy;
    return r;
}

/* 121.78: always a fresh heap copy -- of the value, or of default_val when
 * the key is absent or invalid -- so the result has one ownership
 * ("ownership": "caller" in env.tki) whichever way it was produced and never
 * aliases the environment.  Allocation failure falls back to the default
 * itself (or ""), the only answer left that needs no allocation. */
const char *env_get_or(const char *key, const char *default_val)
{
    const char *val = key_is_valid(key) ? getenv(key) : NULL;
    const char *src = val ? val : default_val;
    if (!src) src = "";
    char *copy = strdup(src);
    if (copy) return copy;
    return default_val ? default_val : "";
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
        size_t      namelen = eq ? (size_t)(eq - entry) : tk_strlen(entry);
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
 * Returns a sealed string (127.295): never freed.  The result is built in a
 * growing malloc'd buffer and handed over as one sealed copy.
 * ------------------------------------------------------------------------- */
const char *env_expand(const char *tmpl)
{
    if (!tmpl) return NULL;

    /* Build result in a dynamically-sized buffer */
    size_t cap = tk_strlen(tmpl) * 2 + 64;
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
            size_t vlen = tk_strlen(val);   /* getenv's: strlen fallback */
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
            size_t vlen = tk_strlen(val);   /* getenv's: strlen fallback */
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
    char *r = tks_dup(buf, out);
    free(buf);
    return r;
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
    char *end = s + tk_strlen(s);   /* a line buffer: strlen fallback */
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
        size_t linelen = tk_strlen(line);
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
            size_t vlen = tk_strlen(raw);
            if (vlen >= sizeof(processed)) vlen = sizeof(processed) - 1;
            memcpy(processed, raw, vlen);
            processed[vlen] = '\0';
        } else {
            /* Unquoted: trim whitespace */
            char *trimmed = trim_inplace(raw);
            size_t vlen = tk_strlen(trimmed);
            if (vlen >= sizeof(processed)) vlen = sizeof(processed) - 1;
            memcpy(processed, trimmed, vlen);
            processed[vlen] = '\0';
        }

        /* AMB-04: silently skip dangerous keys from the (untrusted) dotenv file. */
        if (dotenv_key_denied(key)) continue;

        if (setenv(key, processed, 1) == 0) count++;
    }

    fclose(f);
    return count;
}
