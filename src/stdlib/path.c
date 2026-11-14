/* SPDX-License-Identifier: MIT */
/*
 * path.c — Implementation of the std.path standard library module.
 *
 * Pure path string operations. No filesystem calls — all logic is
 * string manipulation only.
 *
 * 127.295 (S1 step 5, runtime-abi §3.1): every result is a sealed string
 * from the string heap (tk_strhdr.h) and inputs are measured with
 * tk_strlen.  A result is never freed, written or reallocated -- C callers
 * included.
 *
 * Story: 55.1  Branch: feature/stdlib-55.1-path
 */

#include "path.h"
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdlib.h>
#include "tk_strhdr.h"

/*
 * path_join — concatenate two path segments with exactly one '/' between them.
 * "a" + "b"  → "a/b"
 * "a/" + "b" → "a/b"
 * "a" + "/b" → "a/b"
 * Returns a sealed string.
 */
const char *path_join(const char *a, const char *b)
{
    size_t alen = tk_strlen(a);
    size_t blen = tk_strlen(b);

    /* Strip trailing slash from a. */
    while (alen > 0 && a[alen - 1] == '/') alen--;

    /* Strip leading slash from b. */
    size_t boff = 0;
    while (boff < blen && b[boff] == '/') boff++;

    /* Result: a[0..alen-1] + '/' + b[boff..blen-1] + '\0' */
    size_t rlen = alen + 1 + (blen - boff);
    char *out = tks_alloc(rlen);
    if (!out) return NULL;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b + boff, blen - boff);
    out[rlen] = '\0';
    return tks_seal(out, rlen);
}

/*
 * path_normalize — AMB-06: lexically resolve '.', '..', and redundant '/' with
 * NO filesystem access (path.c is string-only). '..' is clamped at the root: a
 * leading/over-popping '..' is dropped rather than escaping above the base, so a
 * normalized relative path stays within its starting point — making it safe as a
 * containment primitive when combined with a prefix check. An absolute path keeps
 * its leading '/'. An empty relative result normalizes to ".".
 * Returns a sealed string.
 */
const char *path_normalize(const char *p)
{
    if (!p) return NULL;
    size_t len = tk_strlen(p);
    int absolute = (len > 0 && p[0] == '/');

    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, p, len + 1);

    const char **stack = malloc(sizeof(char *) * (len + 1));
    if (!stack) { free(buf); return NULL; }
    size_t sp = 0;

    char *save = NULL;
    for (char *tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0) continue;
        if (strcmp(tok, "..") == 0) {
            if (sp > 0) sp--;   /* pop a real segment; clamp at root when empty */
            continue;
        }
        stack[sp++] = tok;
    }

    char *out = tks_alloc(len + 1);   /* <= input + optional leading '/' (+ NUL) */
    if (!out) { free(stack); free(buf); return NULL; }
    size_t o = 0;
    if (absolute) out[o++] = '/';
    for (size_t i = 0; i < sp; i++) {
        if (i > 0) out[o++] = '/';
        size_t sl = tk_strlen(stack[i]);   /* a private token: strlen fallback */
        memcpy(out + o, stack[i], sl);
        o += sl;
    }
    if (o == 0) out[o++] = '.';   /* empty relative result */
    out[o] = '\0';
    free(stack);
    free(buf);
    return tks_seal(out, o);
}

/*
 * path_ext — return the file extension of the last path component, including
 * the leading dot (e.g. ".tk"), or "" if there is no extension.
 *
 * Rules:
 *   "foo.tar.gz" → ".gz"
 *   "foo"        → ""
 *   ".hidden"    → ".hidden"  (pure dotfile: entire basename is the extension)
 *   "foo."       → "."
 *
 * Returns a sealed string.
 */
const char *path_ext(const char *p)
{
    /* Work on the basename portion only. */
    const char *base = strrchr(p, '/');
    base = (base != NULL) ? base + 1 : p;

    /* Empty basename → no extension. */
    if (*base == '\0') return tks_dup("", 0);

    /* Pure dotfile (starts with '.' and has no further dot): entire name is
     * the extension. */
    if (base[0] == '.') {
        const char *dot2 = strchr(base + 1, '.');
        if (dot2 == NULL) {
            /* e.g. ".hidden" → ".hidden" */
            return tks_strdup(base);
        }
        /* e.g. ".foo.bar" — fall through and find the last dot. */
    }

    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        return tks_dup("", 0);
    }
    return tks_strdup(dot);
}

/*
 * path_stem — return the filename without its extension.
 *
 * Rules:
 *   "pages/foo.tk" → "foo"
 *   "foo.tar.gz"   → "foo.tar"
 *   ".hidden"      → ""   (stem of a pure dotfile is empty)
 *   "foo"          → "foo"
 *
 * Returns a sealed string.
 */
const char *path_stem(const char *p)
{
    /* Work on the basename portion only. */
    const char *base = strrchr(p, '/');
    base = (base != NULL) ? base + 1 : p;

    size_t blen = tk_strlen(base);

    /* Empty basename. */
    if (blen == 0) return tks_dup("", 0);

    /* Pure dotfile: starts with '.' and has no further dot.  Stem is "". */
    if (base[0] == '.') {
        const char *dot2 = strchr(base + 1, '.');
        if (dot2 == NULL) {
            return tks_dup("", 0);
        }
        /* e.g. ".foo.bar" — stem is everything up to the last dot. */
        const char *dot = strrchr(base, '.');
        size_t stem_len = (size_t)(dot - base);
        return tks_dup(base, stem_len);
    }

    /* General case: find the last dot. */
    const char *dot = strrchr(base, '.');
    if (dot == NULL) {
        /* No extension — stem is the whole basename. */
        return tks_strdup(base);
    }

    size_t stem_len = (size_t)(dot - base);
    return tks_dup(base, stem_len);
}

/*
 * path_dir — return the parent directory (everything before the final '/').
 *
 * Special cases:
 *   "foo"    → "."
 *   "/"      → "/"
 *   "/foo"   → "/"
 *
 * Returns a sealed string.
 */
const char *path_dir(const char *p)
{
    size_t len = tk_strlen(p);
    if (len == 0) return tks_dup(".", 1);

    /* Strip trailing slashes. */
    size_t end = len;
    while (end > 1 && p[end - 1] == '/') end--;

    /* Find the last '/' within p[0..end-1]. */
    size_t last_slash = end;
    while (last_slash > 0 && p[last_slash - 1] != '/') last_slash--;

    if (last_slash == 0) {
        /* No '/' found — directory is current directory. */
        return tks_dup(".", 1);
    }

    /* Trim trailing slashes from the directory portion, but keep "/" as-is. */
    size_t dir_end = last_slash;
    while (dir_end > 1 && p[dir_end - 1] == '/') dir_end--;

    if (dir_end == 0) return tks_dup("/", 1);

    return tks_dup(p, dir_end);
}

/*
 * path_base — return the last path component (everything after the final '/').
 *
 * Special cases:
 *   "/"  → "/"
 *   ""   → ""
 *
 * Returns a sealed string.
 */
const char *path_base(const char *p)
{
    size_t len = tk_strlen(p);
    if (len == 0) return tks_dup("", 0);
    if (len == 1 && p[0] == '/') return tks_dup("/", 1);

    /* Strip trailing slashes (but keep at least one character). */
    size_t end = len;
    while (end > 1 && p[end - 1] == '/') end--;

    /* Find the last '/' before end. */
    size_t start = end;
    while (start > 0 && p[start - 1] != '/') start--;

    size_t seg = end - start;
    return tks_dup(p + start, seg);
}

/*
 * path_isabs — return 1 if the path is absolute (starts with '/'), 0 otherwise.
 */
int path_isabs(const char *p)
{
    return (p != NULL && p[0] == '/') ? 1 : 0;
}
