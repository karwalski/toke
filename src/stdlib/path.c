/*
 * path.c — Implementation of the std.path standard library module.
 *
 * Pure path string operations. No filesystem calls — all logic is
 * string manipulation only.  Callers own returned pointers.
 *
 * Story: 55.1  Branch: feature/stdlib-55.1-path
 */

#include "path.h"
#include <string.h>
#include <stdlib.h>

/*
 * path_join — concatenate two path segments with exactly one '/' between them.
 * "a" + "b"  → "a/b"
 * "a/" + "b" → "a/b"
 * "a" + "/b" → "a/b"
 * Returns a heap-allocated string; caller owns it.
 */
const char *path_join(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);

    /* Strip trailing slash from a. */
    while (alen > 0 && a[alen - 1] == '/') alen--;

    /* Strip leading slash from b. */
    size_t boff = 0;
    while (boff < blen && b[boff] == '/') boff++;

    /* Result: a[0..alen-1] + '/' + b[boff..blen-1] + '\0' */
    size_t rlen = alen + 1 + (blen - boff);
    char *out = malloc(rlen + 1);
    if (!out) return NULL;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b + boff, blen - boff);
    out[rlen] = '\0';
    return out;
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
 * Returns a heap-allocated string; caller owns it.
 */
const char *path_ext(const char *p)
{
    /* Work on the basename portion only. */
    const char *base = strrchr(p, '/');
    base = (base != NULL) ? base + 1 : p;

    /* Empty basename → no extension. */
    if (*base == '\0') return strdup("");

    /* Pure dotfile (starts with '.' and has no further dot): entire name is
     * the extension. */
    if (base[0] == '.') {
        const char *dot2 = strchr(base + 1, '.');
        if (dot2 == NULL) {
            /* e.g. ".hidden" → ".hidden" */
            return strdup(base);
        }
        /* e.g. ".foo.bar" — fall through and find the last dot. */
    }

    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        return strdup("");
    }
    return strdup(dot);
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
 * Returns a heap-allocated string; caller owns it.
 */
const char *path_stem(const char *p)
{
    /* Work on the basename portion only. */
    const char *base = strrchr(p, '/');
    base = (base != NULL) ? base + 1 : p;

    size_t blen = strlen(base);

    /* Empty basename. */
    if (blen == 0) return strdup("");

    /* Pure dotfile: starts with '.' and has no further dot.  Stem is "". */
    if (base[0] == '.') {
        const char *dot2 = strchr(base + 1, '.');
        if (dot2 == NULL) {
            return strdup("");
        }
        /* e.g. ".foo.bar" — stem is everything up to the last dot. */
        const char *dot = strrchr(base, '.');
        size_t stem_len = (size_t)(dot - base);
        char *out = malloc(stem_len + 1);
        if (!out) return NULL;
        memcpy(out, base, stem_len);
        out[stem_len] = '\0';
        return out;
    }

    /* General case: find the last dot. */
    const char *dot = strrchr(base, '.');
    if (dot == NULL) {
        /* No extension — stem is the whole basename. */
        return strdup(base);
    }

    size_t stem_len = (size_t)(dot - base);
    char *out = malloc(stem_len + 1);
    if (!out) return NULL;
    memcpy(out, base, stem_len);
    out[stem_len] = '\0';
    return out;
}

/*
 * path_dir — return the parent directory (everything before the final '/').
 *
 * Special cases:
 *   "foo"    → "."
 *   "/"      → "/"
 *   "/foo"   → "/"
 *
 * Returns a heap-allocated string; caller owns it.
 */
const char *path_dir(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) return strdup(".");

    /* Strip trailing slashes. */
    size_t end = len;
    while (end > 1 && p[end - 1] == '/') end--;

    /* Find the last '/' within p[0..end-1]. */
    size_t last_slash = end;
    while (last_slash > 0 && p[last_slash - 1] != '/') last_slash--;

    if (last_slash == 0) {
        /* No '/' found — directory is current directory. */
        return strdup(".");
    }

    /* Trim trailing slashes from the directory portion, but keep "/" as-is. */
    size_t dir_end = last_slash;
    while (dir_end > 1 && p[dir_end - 1] == '/') dir_end--;

    if (dir_end == 0) return strdup("/");

    char *out = malloc(dir_end + 1);
    if (!out) return NULL;
    memcpy(out, p, dir_end);
    out[dir_end] = '\0';
    return out;
}

/*
 * path_base — return the last path component (everything after the final '/').
 *
 * Special cases:
 *   "/"  → "/"
 *   ""   → ""
 *
 * Returns a heap-allocated string; caller owns it.
 */
const char *path_base(const char *p)
{
    size_t len = strlen(p);
    if (len == 0) return strdup("");
    if (len == 1 && p[0] == '/') return strdup("/");

    /* Strip trailing slashes (but keep at least one character). */
    size_t end = len;
    while (end > 1 && p[end - 1] == '/') end--;

    /* Find the last '/' before end. */
    size_t start = end;
    while (start > 0 && p[start - 1] != '/') start--;

    size_t seg = end - start;
    char *out = malloc(seg + 1);
    if (!out) return NULL;
    memcpy(out, p + start, seg);
    out[seg] = '\0';
    return out;
}

/*
 * path_isabs — return 1 if the path is absolute (starts with '/'), 0 otherwise.
 */
int path_isabs(const char *p)
{
    return (p != NULL && p[0] == '/') ? 1 : 0;
}
