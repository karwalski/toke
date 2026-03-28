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
