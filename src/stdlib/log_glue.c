/* SPDX-License-Identifier: MIT */
/*
 * log_glue.c — i64-ABI wrappers for std.log module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.log.
 */

#include "log.h"
#include "capabilities.h"   /* 121.22: log.openaccess/openerror are fs.write sinks */
#include "tk_array.h"      /* 127.400: fields is [[str]] */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Decode a log `fields` argument into a flat key/value const char ** array.
 *
 * 127.400: the declared type (stdlib/log.tki, docs/stdlib/log.md) is
 * [[str]] -- an array of two-element (key; value) string arrays, e.g.
 * @(@("host"; "0.0.0.0"); @("port"; "8080")).  This used to read the OUTER
 * array as a flat interleaved key/value list and cast each inner array handle
 * to a char *, so every field printed the bytes of an array block as text.
 * Each element is now read as an array through tk_array.h: slot 0 is the key,
 * slot 1 the value.  An element with fewer than two slots carries no pair and
 * is skipped; slots past the second are ignored.  The flattened result is
 * what tk_log_info/warn/error/debug take (fields[2i] = key, [2i+1] = value).
 */
static void decode_log_fields(int64_t fields_arr,
                               const char ***out_fields, int *out_count)
{
    *out_fields = NULL;
    *out_count  = 0;

    int64_t n = tk_arr_len(fields_arr);
    if (n <= 0 || n > (int64_t)(INT32_MAX / 2)) return;

    const char **arr = (const char **)malloc((size_t)n * 2 * sizeof(const char *));
    if (!arr) return;

    const int64_t *outer = (const int64_t *)(intptr_t)fields_arr;
    int count = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t pair = outer[i];
        if (tk_arr_len(pair) < 2) continue;
        const int64_t *kv = (const int64_t *)(intptr_t)pair;
        arr[count++] = (const char *)(intptr_t)kv[0];
        arr[count++] = (const char *)(intptr_t)kv[1];
    }
    if (count == 0) { free(arr); return; }
    *out_fields = arr;
    *out_count  = count;
}

/*
 * 121.22 (ADR-0010).  log.openaccess and log.openerror create, append to,
 * rotate, gzip and prune files at a path the PROGRAM chooses -- including
 * the directory they sit in (mkdir_single) and every rotated sibling
 * (apply_retention unlinks them).  124.4c gated file.write and os.open; these
 * two were missed, so a binary baked with --cap-enforce and no grants at all
 * wrote /any/path/access.log (measured on main: exit 0, the file and its
 * .lock sidecar created).  The gate is fs.write, taken once here, at
 * acquisition; the rotation that follows works on the file this acquired.
 *
 * The process's own standard streams are not filesystem authority: fds 1
 * and 2 are held before main runs, and "/dev/stderr" is how ooke's default
 * [log] access = "-" reaches them.  Those two paths need no grant.
 */
static int log_path_is_own_stream(const char *path)
{
    return path && (strcmp(path, "/dev/stderr") == 0 ||
                    strcmp(path, "/dev/stdout") == 0);
}

int64_t tk_log_open_access_w(int64_t path_ptr, int64_t max_lines,
                               int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
    if (!log_path_is_own_stream(path)) TK_REQUIRE(TK_CAP_FS_WRITE);
    TkAccessLog *al = tk_access_log_open(path, (int)max_lines,
                                          (int)max_files, (int)max_age_days);
    if (!al) return -1;
    tk_access_log_set_global(al);
    return 0;
}

int64_t tk_log_open_error_w(int64_t path_ptr, int64_t max_lines,
                              int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
    if (!log_path_is_own_stream(path)) TK_REQUIRE(TK_CAP_FS_WRITE);   /* 121.22 */
    TkAccessLog *el = tk_error_log_open(path, (int)max_lines,
                                         (int)max_files, (int)max_age_days);
    if (!el) return -1;
    tk_error_log_set_global(el);
    return 0;
}

/* 136.77: log.closeerror() : bool -- the toke surface of tk_error_log_close.
 * Closing a file this process opened needs no grant (log.openerror took
 * fs.write when it opened it). */
int64_t tk_log_closeerror_w(void)
{
    return (int64_t)tk_error_log_close_global();
}

int64_t tk_log_accessformat_w(int64_t fmt_ptr)
{
    const char *fmt = (const char *)(intptr_t)fmt_ptr;
    /* 127.191: 0 = accepted, -1 = unknown format (message on stderr). */
    return (int64_t)tk_access_log_set_format(fmt);
}

int64_t tk_log_info_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_info((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

int64_t tk_log_error_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_error((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

int64_t tk_log_warn_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_warn((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}
int64_t tk_log_debug_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_debug((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

int64_t tk_log_setformat_w(int64_t fmt) {
    if (!fmt) return 0;
    tk_log_set_format((const char *)(intptr_t)fmt);
    return 0;
}

int64_t tk_log_setlevel_w(int64_t lvl) {
    if (!lvl) return 0;
    tk_log_set_level((const char *)(intptr_t)lvl);
    return 0;
}
