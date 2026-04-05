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
