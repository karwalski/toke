/*
 * file_glue.c — i64-ABI wrappers for std.file module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.file.
 */

#include "file.h"
#include <stdint.h>
#include <stdlib.h>

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

/* file stubs */
int64_t tk_file_list_w(int64_t dir) { (void)dir; return 0; }
int64_t tk_file_append_w(int64_t path, int64_t content, int64_t extra) {
    (void)path; (void)content; (void)extra; return 0;
}
int64_t tk_file_delete_w(int64_t path) { (void)path; return 0; }
int64_t tk_file_rename_w(int64_t from, int64_t to) { (void)from; (void)to; return 0; }
int64_t tk_file_readlines_w(int64_t path) { (void)path; return 0; }
int64_t tk_file_writelines_w(int64_t path, int64_t lines) { (void)path; (void)lines; return 0; }
int64_t tk_file_stat_w(int64_t path) { (void)path; return 0; }
int64_t tk_file_err_w(int64_t msg) { (void)msg; return 0; }
int64_t tk_file_listdir_w(int64_t path) { (void)path; return 0; }
