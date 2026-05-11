/*
 * file_glue.c — i64-ABI wrappers for std.file module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.file.
 */

#include "file.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_file_read_w(int64_t path) {
    if (!path) return 0;
    StrFileResult r = file_read((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_file_write_w(int64_t path, int64_t content) {
    if (!path || !content) return 0;
    BoolFileResult r = file_write((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_isdir_w(int64_t path) {
    if (!path) return 0;
    return (int64_t)file_is_dir((const char *)(intptr_t)path);
}

int64_t tk_file_mkdir_w(int64_t path) {
    if (!path) return 0;
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_copy_w(int64_t src, int64_t dst) {
    if (!src || !dst) return 0;
    BoolFileResult r = file_copy((const char *)(intptr_t)src, (const char *)(intptr_t)dst);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_listall_w(int64_t dir) {
    if (!dir) return 0;
    StrArrayFileResult r = file_listall((const char *)(intptr_t)dir);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_file_exists_w(int64_t path) {
    if (!path) return 0;
    return (int64_t)file_exists((const char *)(intptr_t)path);
}

/* ── wrappers for additional file operations ────────────────────────────── */

int64_t tk_file_list_w(int64_t dir) {
    if (!dir) return 0;
    StrArrayFileResult r = file_list((const char *)(intptr_t)dir);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    free(arr.data);
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_file_append_w(int64_t path, int64_t content, int64_t extra) {
    (void)extra;
    if (!path || !content) return 0;
    BoolFileResult r = file_append((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_delete_w(int64_t path) {
    if (!path) return 0;
    BoolFileResult r = file_delete((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_rename_w(int64_t from, int64_t to) {
    if (!from || !to) return 0;
    BoolFileResult r = file_move((const char *)(intptr_t)from, (const char *)(intptr_t)to);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_readlines_w(int64_t path) {
    if (!path) return 0;
    StrArrayFileResult r = file_readlines((const char *)(intptr_t)path);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    free(arr.data);
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_file_writelines_w(int64_t path, int64_t lines) {
    if (!path || !lines) return 0;
    /* lines points to block+1; block[0] (at lines[-1]) is the length */
    int64_t *arr = (int64_t *)(intptr_t)lines;
    int64_t len = arr[-1];
    if (len <= 0) {
        /* Empty array: write empty string */
        BoolFileResult r = file_write((const char *)(intptr_t)path, "");
        return r.is_err ? 0 : (int64_t)r.ok;
    }
    /* Calculate total size needed */
    size_t total = 0;
    for (int64_t i = 0; i < len; i++) {
        const char *line = (const char *)(intptr_t)arr[i];
        if (line) total += strlen(line);
        total++; /* for '\n' */
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) return 0;
    size_t pos = 0;
    for (int64_t i = 0; i < len; i++) {
        const char *line = (const char *)(intptr_t)arr[i];
        if (line) {
            size_t slen = strlen(line);
            memcpy(buf + pos, line, slen);
            pos += slen;
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    BoolFileResult r = file_write((const char *)(intptr_t)path, buf);
    free(buf);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_stat_w(int64_t path) {
    if (!path) return 0;
    U64FileResult r = file_size((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_err_w(int64_t msg) {
    /* Error accessor: return the message string as-is */
    return msg;
}

int64_t tk_file_listdir_w(int64_t path) {
    return tk_file_list_w(path);
}
