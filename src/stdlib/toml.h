/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_TOML_H
#define TK_STDLIB_TOML_H

/*
 * toml.h — C interface for the std.toml standard library module.
 *
 * The ok field of TomlResult is a toml_table_t * (opaque to callers).
 * Use toml_get_* functions to extract values.
 * Call toml_free_table() (or toml_free_root()) when done with a top-level
 * TomlResult; on a sub-table view either is a no-op (113.B.4).
 *
 * Story: 55.6.2  Branch: feature/stdlib-55.6-toml
 */

#include <stdint.h>

typedef struct { void       *ok; int is_err; const char *err_msg; } TomlResult;
typedef struct { const char *ok; int is_err; const char *err_msg; } TomlStrResult;
typedef struct { int64_t     ok; int is_err; const char *err_msg; } TomlI64Result;
typedef struct { int         ok; int is_err; const char *err_msg; } TomlBoolResult;
typedef struct { double      ok; int is_err; const char *err_msg; } TomlF64Result;   /* 121.68 */
/* 113.B.4: an array's elements as i64 slots (see toml_get_arr) */
typedef struct { int64_t *data; int64_t len; int is_err; const char *err_msg; } TomlArrResult;

TomlResult     toml_load(const char *src);
TomlResult     toml_load_file(const char *path);
void           toml_free_table(void *tab);

TomlStrResult  toml_get_str(void *tab, const char *key);
TomlI64Result  toml_get_i64(void *tab, const char *key);
TomlBoolResult toml_get_bool(void *tab, const char *key);
TomlResult     toml_get_section(void *tab, const char *key);
TomlF64Result  toml_get_f64(void *tab, const char *key);   /* 121.68 */
int            toml_has(void *tab, const char *key);       /* 121.68: 1 present, 0 absent */

/* 113.B.4 */
int            toml_free_root(void *tab);   /* 1 freed a document, 0 nothing to free */
TomlArrResult  toml_get_arr(void *tab, const char *key, char kind);  /* s i d b t */
int64_t        toml_count(void *tab);
const char    *toml_key_at(void *tab, int64_t idx);

#endif /* TK_STDLIB_TOML_H */
