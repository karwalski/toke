/*
 * log_glue.c — i64-ABI wrappers for std.log module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.log.
 */

#include "log.h"
#include <stdint.h>
#include <stdlib.h>

/*
 * Decode a toke-format array of strings into a flat const char ** array.
 * Toke array layout:  ptr[-1] = element count (i64)
 *                     ptr[0..count-1] = elements as i64 (char * as integer)
 */
static void decode_log_fields(int64_t fields_map,
                               const char ***out_fields, int *out_count)
{
    *out_fields = NULL;
    *out_count  = 0;
    if (fields_map == 0) return;

    int64_t *ptr = (int64_t *)(intptr_t)fields_map;
    int64_t len  = ptr[-1];
    if (len <= 0) return;

    int count = (int)(len & ~(int64_t)1);
    if (count == 0) return;

    const char **arr = (const char **)malloc((size_t)count * sizeof(const char *));
    if (!arr) return;

    for (int i = 0; i < count; i++) {
        arr[i] = (const char *)(intptr_t)ptr[i];
    }
    *out_fields = arr;
    *out_count  = count;
}

int64_t tk_log_open_access_w(int64_t path_ptr, int64_t max_lines,
                               int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
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
    TkAccessLog *el = tk_error_log_open(path, (int)max_lines,
                                         (int)max_files, (int)max_age_days);
    if (!el) return -1;
    tk_error_log_set_global(el);
    return 0;
}

int64_t tk_log_accessformat_w(int64_t fmt_ptr)
{
    const char *fmt = (const char *)(intptr_t)fmt_ptr;
    tk_access_log_set_format(fmt);
    return 0;
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

int64_t tk_log_setformat_w(int64_t fmt) { (void)fmt; return 0; }
int64_t tk_log_setlevel_w(int64_t lvl) { (void)lvl; return 0; }
