/*
 * file.c — Implementation of the std.file standard library module.
 *
 * Uses POSIX APIs only. malloc is permitted at this stdlib boundary.
 * Callers own returned pointers.
 *
 * Story: 1.3.5  Branch: feature/stdlib-file
 */

#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <glob.h>

/* Map errno to a FileErrKind after a failed POSIX call. */
static FileErr make_err(int err_no, const char *fallback)
{
    FileErr e;
    if (err_no == ENOENT) {
        e.kind = FILE_ERR_NOT_FOUND;
        e.msg  = "file not found";
    } else if (err_no == EACCES || err_no == EPERM) {
        e.kind = FILE_ERR_PERMISSION;
        e.msg  = "permission denied";
    } else if (err_no == EINVAL || err_no == EEXIST || err_no == ENOTEMPTY ||
               err_no == ENOTDIR || err_no == EISDIR) {
        e.kind = FILE_ERR_INVALID;
        e.msg  = fallback ? fallback : "invalid operation";
    } else {
        e.kind = FILE_ERR_IO;
        e.msg  = fallback ? fallback : "I/O error";
    }
    return e;
}

StrFileResult file_read(const char *path)
{
    StrFileResult r = {NULL, 0, {0, NULL}};
    FILE *f = fopen(path, "rb");
    if (!f) { r.is_err = 1; r.err = make_err(errno, NULL); return r; }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        r.is_err = 1; r.err = make_err(errno, "seek failed"); return r;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        r.is_err = 1; r.err = make_err(errno, "ftell failed"); return r;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    r.ok = buf;
    return r;
}

BoolFileResult file_write(const char *path, const char *content)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    FILE *f = fopen(path, "w");
    if (!f) { r.is_err = 1; r.err = make_err(errno, NULL); return r; }
    if (fputs(content, f) == EOF) {
        int saved = errno; fclose(f);
        r.is_err = 1; r.err = make_err(saved, "write failed"); return r;
    }
    fclose(f);
    r.ok = 1;
    return r;
}

BoolFileResult file_append(const char *path, const char *content)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    FILE *f = fopen(path, "a");
    if (!f) { r.is_err = 1; r.err = make_err(errno, NULL); return r; }
    if (fputs(content, f) == EOF) {
        int saved = errno; fclose(f);
        r.is_err = 1; r.err = make_err(saved, "append failed"); return r;
    }
    fclose(f);
    r.ok = 1;
    return r;
}

int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

BoolFileResult file_delete(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (unlink(path) != 0) {
        r.is_err = 1; r.err = make_err(errno, "unlink failed"); return r;
    }
    r.ok = 1;
    return r;
}

StrArrayFileResult file_list(const char *dir)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};
    DIR *d = opendir(dir);
    if (!d) { r.is_err = 1; r.err = make_err(errno, NULL); return r; }

    /* Two-pass: count then fill. */
    size_t cap = 16, count = 0;
    const char **entries = malloc(cap * sizeof(const char *));
    if (!entries) {
        closedir(d);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (count == cap) {
            cap *= 2;
            const char **tmp = realloc(entries, cap * sizeof(const char *));
            if (!tmp) { free(entries); closedir(d);
                r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r; }
            entries = tmp;
        }
        entries[count++] = strdup(ent->d_name);
    }
    closedir(d);
    r.ok.data = entries;
    r.ok.len  = (uint64_t)count;
    return r;
}

/* ── 28.2.1: directory operations ────────────────────────────────────────── */

int file_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int file_is_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

BoolFileResult file_mkdir(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (mkdir(path, 0755) != 0) {
        r.is_err = 1; r.err = make_err(errno, "mkdir failed"); return r;
    }
    r.ok = 1;
    return r;
}

/*
 * file_mkdir_p — create directory and all missing parent components.
 * Walks the path left-to-right, calling mkdir() on each prefix that does not
 * yet exist.  EEXIST on an existing component is not treated as an error.
 */
BoolFileResult file_mkdir_p(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    memcpy(tmp, path, len + 1);

    /* Walk each path separator, creating components one at a time. */
    for (size_t i = 1; i <= len; i++) {
        if (i == len || tmp[i] == '/') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                int saved_errno = errno;
                free(tmp);
                r.is_err = 1; r.err = make_err(saved_errno, "mkdir_p failed"); return r;
            }
            tmp[i] = saved;
        }
    }
    free(tmp);
    r.ok = 1;
    return r;
}

BoolFileResult file_rmdir(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (rmdir(path) != 0) {
        r.is_err = 1; r.err = make_err(errno, "rmdir failed"); return r;
    }
    r.ok = 1;
    return r;
}

/*
 * file_rmdir_r — recursively remove a directory tree.
 * Walks entries: unlinks files, recurses into subdirectories, then rmdir().
 * Uses a path buffer built on the heap; no system("rm -rf").
 */
BoolFileResult file_rmdir_r(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    DIR *d = opendir(path);
    if (!d) {
        r.is_err = 1; r.err = make_err(errno, "opendir failed"); return r;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Build child path: path + "/" + name */
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        char *child = malloc(plen + 1 + nlen + 1);
        if (!child) {
            closedir(d);
            r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
        }
        memcpy(child, path, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, ent->d_name, nlen + 1);

        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            BoolFileResult sub = file_rmdir_r(child);
            if (sub.is_err) { free(child); closedir(d); return sub; }
        } else {
            if (unlink(child) != 0) {
                int saved_errno = errno;
                free(child); closedir(d);
                r.is_err = 1; r.err = make_err(saved_errno, "unlink failed"); return r;
            }
        }
        free(child);
    }
    closedir(d);

    if (rmdir(path) != 0) {
        r.is_err = 1; r.err = make_err(errno, "rmdir failed"); return r;
    }
    r.ok = 1;
    return r;
}

/* ── 28.2.2: copy, move, and metadata ───────────────────────────────────── */

/*
 * file_copy — copy src to dst in 8 KB chunks.
 * dst is truncated/created.  Cross-device safe (no rename dependency).
 */
BoolFileResult file_copy(const char *src, const char *dst)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    FILE *in = fopen(src, "rb");
    if (!in) {
        r.is_err = 1; r.err = make_err(errno, "open source failed"); return r;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        int saved_errno = errno; fclose(in);
        r.is_err = 1; r.err = make_err(saved_errno, "open dest failed"); return r;
    }

    char buf[8192];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nread, out) != nread) {
            int saved_errno = errno; fclose(in); fclose(out);
            r.is_err = 1; r.err = make_err(saved_errno, "write failed"); return r;
        }
    }
    if (ferror(in)) {
        int saved_errno = errno; fclose(in); fclose(out);
        r.is_err = 1; r.err = make_err(saved_errno, "read failed"); return r;
    }
    fclose(in);
    fclose(out);
    r.ok = 1;
    return r;
}

/*
 * file_move — rename src to dst; fallback to copy+delete on EXDEV.
 */
BoolFileResult file_move(const char *src, const char *dst)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (rename(src, dst) == 0) {
        r.ok = 1; return r;
    }
    if (errno != EXDEV) {
        r.is_err = 1; r.err = make_err(errno, "rename failed"); return r;
    }
    /* Cross-device: copy then delete source. */
    BoolFileResult cp = file_copy(src, dst);
    if (cp.is_err) return cp;
    if (unlink(src) != 0) {
        r.is_err = 1; r.err = make_err(errno, "unlink after copy failed"); return r;
    }
    r.ok = 1;
    return r;
}

U64FileResult file_size(const char *path)
{
    U64FileResult r = {0, 0, {0, NULL}};
    struct stat st;
    if (stat(path, &st) != 0) {
        r.is_err = 1; r.err = make_err(errno, "stat failed"); return r;
    }
    r.ok = (uint64_t)st.st_size;
    return r;
}

U64FileResult file_mtime(const char *path)
{
    U64FileResult r = {0, 0, {0, NULL}};
    struct stat st;
    if (stat(path, &st) != 0) {
        r.is_err = 1; r.err = make_err(errno, "stat failed"); return r;
    }
    r.ok = (uint64_t)st.st_mtime;
    return r;
}

/* ── 28.2.3: path utilities ─────────────────────────────────────────────── */

/*
 * file_join — concatenate two path segments with exactly one '/' between them.
 * "a" + "b" → "a/b", "a/" + "b" → "a/b", "a" + "/b" → "a/b".
 * Returns a heap-allocated string; caller owns it.
 */
const char *file_join(const char *a, const char *b)
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
 * file_basename — return the last path component (everything after the final '/').
 * Special cases:
 *   "/"  → "/"
 *   ""   → ""
 * Returns a heap-allocated string; caller owns it.
 */
const char *file_basename(const char *path)
{
    size_t len = strlen(path);
    if (len == 0) return strdup("");
    if (len == 1 && path[0] == '/') return strdup("/");

    /* Strip trailing slashes (but keep at least one char). */
    size_t end = len;
    while (end > 1 && path[end - 1] == '/') end--;

    /* Find the last '/' before end. */
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;

    /* Copy path[start..end-1]. */
    size_t seg = end - start;
    char *out = malloc(seg + 1);
    if (!out) return NULL;
    memcpy(out, path + start, seg);
    out[seg] = '\0';
    return out;
}

/*
 * file_dirname — return the parent directory (everything before the final '/').
 * Special cases:
 *   "foo"  → "."
 *   "/"    → "/"
 *   "/foo" → "/"
 * Returns a heap-allocated string; caller owns it.
 */
const char *file_dirname(const char *path)
{
    size_t len = strlen(path);
    if (len == 0) return strdup(".");

    /* Strip trailing slashes. */
    size_t end = len;
    while (end > 1 && path[end - 1] == '/') end--;

    /* Find the last '/' within path[0..end-1]. */
    size_t last_slash = end;
    while (last_slash > 0 && path[last_slash - 1] != '/') last_slash--;

    if (last_slash == 0) {
        /* No '/' found — directory is current directory. */
        return strdup(".");
    }

    /* Trim trailing slashes from the directory portion, but keep "/" as-is. */
    size_t dir_end = last_slash;
    while (dir_end > 1 && path[dir_end - 1] == '/') dir_end--;

    /* If we consumed everything it must be root. */
    if (dir_end == 0) return strdup("/");

    char *out = malloc(dir_end + 1);
    if (!out) return NULL;
    memcpy(out, path, dir_end);
    out[dir_end] = '\0';
    return out;
}

/*
 * file_absolute — resolve path to an absolute path via realpath(3).
 * Returns a heap-allocated string on success, NULL if realpath() fails.
 * Caller owns the returned string.
 */
const char *file_absolute(const char *path)
{
    /* realpath() with a NULL buffer allocates the result (POSIX.1-2008). */
    return realpath(path, NULL);
}

/*
 * file_ext — return the file extension of the last path component, including
 * the leading dot (e.g. ".txt"), or "" if there is no extension.
 *
 * Rules:
 *   "foo.tar.gz"  → ".gz"
 *   "foo"         → ""
 *   ".hidden"     → ".hidden"  (entire basename is the extension for dotfiles)
 *   "foo."        → "."
 *
 * Returns a heap-allocated string; caller owns it.
 */
const char *file_ext(const char *path)
{
    /* Work on the basename portion only. */
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;

    /* Empty basename → no extension. */
    if (*base == '\0') return strdup("");

    /* If the basename is exactly a dot-file (starts with '.' and has no
     * further dot), the whole name is the "extension". */
    if (base[0] == '.') {
        /* Check for a second dot anywhere after position 0. */
        const char *dot2 = strchr(base + 1, '.');
        if (dot2 == NULL) {
            /* Pure dotfile like ".hidden" — return entire basename. */
            return strdup(base);
        }
        /* e.g. ".foo.bar" — fall through and find the last dot. */
    }

    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        /* No dot, or dot is the very first char (already handled above). */
        return strdup("");
    }
    return strdup(dot);
}

/*
 * file_readlines — read the file at path and split into lines.
 * Lines are split on '\n'; '\r' is stripped.  A trailing newline does NOT
 * produce an extra empty entry.  Empty file → StrArray with len=0.
 * Returns StrArrayFileResult; caller owns all strings and the data array.
 */
StrArrayFileResult file_readlines(const char *path)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};

    StrFileResult fr = file_read(path);
    if (fr.is_err) {
        r.is_err = 1; r.err = fr.err; return r;
    }

    char *content = (char *)fr.ok; /* heap-allocated by file_read */
    size_t total  = strlen(content);

    /* Count lines (number of '\n' in content, adjusted for trailing newline). */
    size_t cap   = 16;
    size_t count = 0;
    const char **lines = malloc(cap * sizeof(const char *));
    if (!lines) {
        free(content);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }

    char *p   = content;
    char *end = content + total;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *seg_end = (nl != NULL) ? nl : end;

        /* Strip trailing '\r'. */
        size_t seg_len = (size_t)(seg_end - p);
        if (seg_len > 0 && p[seg_len - 1] == '\r') seg_len--;

        /* Skip empty trailing entry produced by a final '\n'. */
        if (nl == NULL || nl + 1 < end || seg_len > 0) {
            if (count == cap) {
                cap *= 2;
                const char **tmp = realloc(lines, cap * sizeof(const char *));
                if (!tmp) {
                    for (size_t i = 0; i < count; i++) free((void *)lines[i]);
                    free(lines); free(content);
                    r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
                }
                lines = tmp;
            }
            char *line = malloc(seg_len + 1);
            if (!line) {
                for (size_t i = 0; i < count; i++) free((void *)lines[i]);
                free(lines); free(content);
                r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
            }
            memcpy(line, p, seg_len);
            line[seg_len] = '\0';
            lines[count++] = line;
        }

        if (nl == NULL) break;
        p = nl + 1;
    }

    free(content);
    r.ok.data = lines;
    r.ok.len  = (uint64_t)count;
    return r;
}

/*
 * file_glob — expand a POSIX glob pattern and return matching paths.
 * Uses glob(3) from <glob.h>.  Returns StrArrayFileResult; caller owns all
 * strings and the data array.  No matches → ok.len == 0 (not an error).
 */
StrArrayFileResult file_glob(const char *pattern)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};

    glob_t g;
    int rc = glob(pattern, 0, NULL, &g);

    if (rc == GLOB_NOMATCH) {
        /* No matches — return empty array. */
        r.ok.data = NULL;
        r.ok.len  = 0;
        return r;
    }
    if (rc != 0) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "glob failed"; return r;
    }

    size_t n = g.gl_pathc;
    const char **out = malloc(n * sizeof(const char *));
    if (!out) {
        globfree(&g);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = strdup(g.gl_pathv[i]);
        if (!out[i]) {
            for (size_t j = 0; j < i; j++) free((void *)out[j]);
            free(out); globfree(&g);
            r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
        }
    }
    globfree(&g);
    r.ok.data = out;
    r.ok.len  = (uint64_t)n;
    return r;
}
