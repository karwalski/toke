/*
 * toml_glue.c — i64-ABI wrappers for std.toml module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.toml.
 */

#include "toml.h"
#include <stdint.h>

int64_t tk_toml_load_w(int64_t src) {
    if (!src) return 0;
    TomlResult r = toml_load((const char *)(intptr_t)src);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_section_w(int64_t tab, int64_t key) {
    if (!tab || !key) return tab;
    TomlResult r = toml_get_section((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? tab : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_str_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlStrResult r = toml_get_str((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_i64_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlI64Result r = toml_get_i64((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : r.ok;
}

int64_t tk_toml_bool_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlBoolResult r = toml_get_bool((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)r.ok;
}
