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
#include <stdarg.h>   /* 135.10: formatted last-error messages */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <glob.h>
#include <ftw.h>
#include <fcntl.h>   /* AMB-07: O_NOFOLLOW */

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

/*
 * AMB-07: open with O_NOFOLLOW so a symlink on the final path component is NOT
 * followed — prevents write-through-symlink and TOCTOU redirection. Behavioural
 * change: opening a symlinked path fails with ELOOP. Covers the fopen modes
 * file.c uses ("rb"/"wb"/"ab"/"w"/"a"/"r").
 */
static FILE *fopen_nofollow(const char *path, const char *mode)
{
    int flags;
    if      (mode[0] == 'r') flags = O_RDONLY;
    else if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else return fopen(path, mode);   /* unknown mode: fall back */
    int fd = open(path, flags | O_NOFOLLOW, 0644);
    if (fd < 0) return NULL;          /* errno set (ELOOP on symlink) */
    FILE *f = fdopen(fd, mode);
    if (!f) { int e = errno; close(fd); errno = e; }
    return f;
}

StrFileResult file_read(const char *path)
{
    StrFileResult r = {NULL, 0, {0, NULL}};
    FILE *f = fopen_nofollow(path, "rb");
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
    FILE *f = fopen_nofollow(path, "w");
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
    FILE *f = fopen_nofollow(path, "a");
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

        /* AMB-05: lstat (not stat) so a symlink to an external directory is
         * classified as a non-dir and removed as a link (unlink), never recursed
         * into — a recursive delete must not follow links outside the tree. */
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
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
    FILE *in = fopen_nofollow(src, "rb");
    if (!in) {
        r.is_err = 1; r.err = make_err(errno, "open source failed"); return r;
    }
    FILE *out = fopen_nofollow(dst, "wb");
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

/* ── 55.3.4: recursive directory listing ─ */

/* thread-local accumulator for file_listall */
static struct { const char **data; uint64_t len; uint64_t cap; const char *base_dir; size_t base_len; } _listall_acc;

static int _listall_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_F) {
        /* store path relative to base_dir, skip leading slash */
        const char *rel = fpath + _listall_acc.base_len;
        while (*rel == '/') rel++;
        if (_listall_acc.len == _listall_acc.cap) {
            _listall_acc.cap *= 2;
            const char **tmp = realloc(_listall_acc.data, _listall_acc.cap * sizeof(const char *));
            if (!tmp) return -1;
            _listall_acc.data = tmp;
        }
        _listall_acc.data[_listall_acc.len++] = strdup(rel);
    }
    return 0;
}

StrArrayFileResult file_listall(const char *dir) {
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};
    _listall_acc.cap = 64;
    _listall_acc.len = 0;
    _listall_acc.base_dir = dir;
    _listall_acc.base_len = strlen(dir);
    _listall_acc.data = malloc(_listall_acc.cap * sizeof(const char *));
    if (!_listall_acc.data) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    if (nftw(dir, _listall_cb, 64, FTW_PHYS) != 0) {
        for (uint64_t i = 0; i < _listall_acc.len; i++) free((void *)_listall_acc.data[i]);
        free(_listall_acc.data);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "nftw failed"; return r;
    }
    r.ok.data = _listall_acc.data;
    r.ok.len  = _listall_acc.len;
    return r;
}

/* ══ 135.10 — binary file access ═════════════════════════════════════════
 *
 * file_read() above returns a NUL-terminated char*.  For text that is fine;
 * for a zip, a PDF, a PNG or an xlsx it silently returns the bytes before the
 * first zero, and a caller has no way to notice.  std.zip had to open its own
 * file descriptor to work around it (135.1); 135.3 and 135.4 would each have
 * invented the same workaround.
 *
 * THE RULE THESE TWO CALLS ARE BUILT AROUND: an empty result must never stand
 * for a failure.  A zero-length `@(byte)` means the file was read and is
 * empty; the error arm is a bare 0 (the compiled T!E ABI carries no payload —
 * 127.97), and file_lasterrkind() says which of the nine outcomes occurred.
 */

/* Last-error channel.  Messages are formatted (they name the path and the
 * numbers), so unlike zip's literal-only channel this needs storage; the
 * buffer is static and overwritten by the next failure, which is exactly the
 * lifetime the accessor documents. */
static char        g_file_err_msg[1024] = "";
static const char *g_file_err_kind      = "ok";

static const char *file_kind_token(FileErrKind k)
{
    switch (k) {
        case FILE_ERR_NOT_FOUND:   return "notfound";
        case FILE_ERR_PERMISSION:  return "permission";
        case FILE_ERR_IS_DIR:      return "isdir";
        case FILE_ERR_NOT_REGULAR: return "notregular";
        case FILE_ERR_SYMLINK:     return "symlink";
        case FILE_ERR_TOO_LARGE:   return "toolarge";
        case FILE_ERR_NO_MEM:      return "nomem";
        case FILE_ERR_BAD_ARG:     return "badarg";
        case FILE_ERR_INVALID:     return "invalid";
        case FILE_ERR_IO:          return "io";
    }
    return "io";
}

void file_setlasterr(FileErrKind kind, const char *msg)
{
    g_file_err_kind = file_kind_token(kind);
    snprintf(g_file_err_msg, sizeof g_file_err_msg, "%s", msg ? msg : "");
}

const char *file_lasterr(void)     { return g_file_err_msg; }
const char *file_lasterrkind(void) { return g_file_err_kind; }

static void file_clear_lasterr(void)
{
    g_file_err_kind  = "ok";
    g_file_err_msg[0] = '\0';
}

/* Record a failure and build the FileErr the result carries. */
static FileErr file_fail(FileErrKind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_file_err_msg, sizeof g_file_err_msg, fmt, ap);
    va_end(ap);
    g_file_err_kind = file_kind_token(kind);

    FileErr e;
    e.kind = kind;
    e.msg  = g_file_err_msg;
    return e;
}

/*
 * Classify an errno from open(2) on a path that is being read or written.
 * EISDIR and ELOOP are pulled out of the generic I/O bucket deliberately:
 * "it is a directory" and "it is a symlink and std.file does not follow the
 * final component (AMB-07)" are both things a caller can act on, and both
 * would otherwise arrive as an undifferentiated I/O error.
 */
static FileErrKind file_open_kind(int err_no)
{
    switch (err_no) {
        case ENOENT:  return FILE_ERR_NOT_FOUND;
        case ENOTDIR: return FILE_ERR_NOT_FOUND;
        case EACCES:  return FILE_ERR_PERMISSION;
        case EPERM:   return FILE_ERR_PERMISSION;
        case EISDIR:  return FILE_ERR_IS_DIR;
        case ELOOP:   return FILE_ERR_SYMLINK;
        default:      return FILE_ERR_IO;
    }
}

/*
 * file_readbytes — read a whole file as bytes, exactly, or say why not.
 *
 * Nine distinct outcomes, every one of them reportable:
 *
 *   ok          a byte-exact copy; length 0 for an empty file (NOT an error)
 *   notfound    no such path, or a component of it is not a directory
 *   permission  EACCES/EPERM on open
 *   isdir       the path is a directory
 *   notregular  fifo, socket, device — st_size is meaningless, so reading it
 *               as "the whole file" would return a plausible wrong answer
 *   symlink     final component is a symlink; AMB-07 forbids following it
 *   toolarge    over TK_FILE_MAX_BYTES (see the note on the 8x array cost)
 *   nomem       the file fits the cap but the allocation failed
 *   io          read(2) failed, or the file changed size mid-read
 *
 * The size is taken with fstat on the SAME descriptor that is read, so there
 * is no stat/open race, and a short read is an error rather than a quietly
 * truncated buffer — which is the defect this whole story exists to remove.
 */
BytesFileResult file_readbytes(const char *path)
{
    BytesFileResult r = {{NULL, 0}, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.readbytes: null path");
        return r;
    }

    /* O_NONBLOCK is load-bearing, not defensive: open(2) on a FIFO with no
     * writer BLOCKS FOREVER without it, so a std.file call would hang on a
     * path that this function is going to refuse anyway.  On a regular file
     * it changes nothing — reads never return EAGAIN. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        int saved = errno;
        FileErrKind k = file_open_kind(saved);
        r.is_err = 1;
        r.err = file_fail(k, "file.readbytes: %s: %s",
                          k == FILE_ERR_SYMLINK
                              ? "final path component is a symlink and std.file does not follow it (AMB-07)"
                              : strerror(saved),
                          path);
        return r;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved = errno;
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO, "file.readbytes: fstat failed: %s: %s",
                          strerror(saved), path);
        return r;
    }

    if (S_ISDIR(st.st_mode)) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IS_DIR, "file.readbytes: is a directory: %s", path);
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_NOT_REGULAR,
                          "file.readbytes: not a regular file (fifo, socket or device): %s",
                          path);
        return r;
    }

    uint64_t want = (uint64_t)st.st_size;
    if (want > TK_FILE_MAX_BYTES) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_TOO_LARGE,
                          "file.readbytes: %llu bytes exceeds the %llu-byte limit: %s",
                          (unsigned long long)want,
                          (unsigned long long)TK_FILE_MAX_BYTES, path);
        return r;
    }

    /* +1 so an empty file still gets a non-NULL buffer; the length, not the
     * pointer and never a NUL, is what says how much there is. */
    uint8_t *buf = (uint8_t *)malloc((size_t)want + 1);
    if (!buf) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_NO_MEM,
                          "file.readbytes: allocation of %llu bytes failed: %s",
                          (unsigned long long)want, path);
        return r;
    }

    uint64_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, (size_t)(want - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            close(fd); free(buf);
            r.is_err = 1;
            r.err = file_fail(file_open_kind(saved) == FILE_ERR_PERMISSION
                                  ? FILE_ERR_PERMISSION : FILE_ERR_IO,
                              "file.readbytes: read failed after %llu of %llu bytes: %s: %s",
                              (unsigned long long)got, (unsigned long long)want,
                              strerror(saved), path);
            return r;
        }
        if (n == 0) break;           /* file shrank under us */
        got += (uint64_t)n;
    }
    close(fd);

    if (got != want) {
        free(buf);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO,
                          "file.readbytes: short read, %llu of %llu bytes "
                          "(the file changed size during the read): %s",
                          (unsigned long long)got, (unsigned long long)want, path);
        return r;
    }

    buf[want] = 0;   /* convenience for C callers only; never a terminator */
    file_clear_lasterr();
    r.ok.data = buf;
    r.ok.len  = want;
    return r;
}

/*
 * file_writebytes — write bytes exactly, or say why not.
 *
 * The write side has the same defect as the read side: file_write() takes a
 * char* and calls fputs, so it stops at the first zero.  Without this call
 * there is no way to put a decompressed zip entry (135.1 hands back an exact
 * `@(byte)`) back on disk, and a caller who tried would get a file truncated
 * at the first zero with a successful return value.
 *
 * Outcomes: ok, notfound (a parent directory is missing), permission, isdir,
 * symlink, io (including a short write — ENOSPC), badarg (null path).
 *
 * Semantics match file.write deliberately: create-or-truncate, mode 0644,
 * O_NOFOLLOW, no atomic temp-file-and-rename.  A failure partway through
 * therefore leaves a partially written file, and the message says how many
 * bytes reached the disk; the caller decides whether to delete it.
 */
BoolFileResult file_writebytes(const char *path, const uint8_t *data, uint64_t len)
{
    BoolFileResult r = {0, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.writebytes: null path");
        return r;
    }
    if (!data && len > 0) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG,
                          "file.writebytes: null buffer with length %llu",
                          (unsigned long long)len);
        return r;
    }

    /* O_NONBLOCK for the same reason as the read side: opening a FIFO for
     * writing with no reader blocks, and ENXIO is the non-blocking form of
     * that — reported as "not a regular file", which is what it means. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (fd < 0) {
        int saved = errno;
        FileErrKind k = (saved == ENXIO) ? FILE_ERR_NOT_REGULAR : file_open_kind(saved);
        r.is_err = 1;
        r.err = file_fail(k, "file.writebytes: %s: %s",
                          k == FILE_ERR_SYMLINK
                              ? "final path component is a symlink and std.file does not follow it (AMB-07)"
                              : (k == FILE_ERR_NOT_REGULAR
                                     ? "not a regular file (fifo, socket or device)"
                                     : strerror(saved)),
                          path);
        return r;
    }

    /* The destination must be a regular file.  Writing bytes into a device or
     * a fifo through a call named "write this file" would succeed and mean
     * something entirely different. */
    struct stat wst;
    if (fstat(fd, &wst) == 0 && !S_ISREG(wst.st_mode)) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(S_ISDIR(wst.st_mode) ? FILE_ERR_IS_DIR : FILE_ERR_NOT_REGULAR,
                          "file.writebytes: %s: %s",
                          S_ISDIR(wst.st_mode) ? "is a directory"
                                               : "not a regular file (fifo, socket or device)",
                          path);
        return r;
    }

    uint64_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            close(fd);
            r.is_err = 1;
            r.err = file_fail(FILE_ERR_IO,
                              "file.writebytes: write failed after %llu of %llu bytes "
                              "(file left partially written): %s: %s",
                              (unsigned long long)done, (unsigned long long)len,
                              strerror(saved), path);
            return r;
        }
        done += (uint64_t)n;
    }

    if (close(fd) != 0) {
        int saved = errno;
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO, "file.writebytes: close failed: %s: %s",
                          strerror(saved), path);
        return r;
    }

    file_clear_lasterr();
    r.ok = 1;
    return r;
}
