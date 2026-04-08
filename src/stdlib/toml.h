#ifndef TK_STDLIB_TOML_H
#define TK_STDLIB_TOML_H

/*
 * toml.h — C interface for the std.toml standard library module.
 *
 * The ok field of TomlResult is a toml_table_t * (opaque to callers).
 * Use toml_get_* functions to extract values.
 * Call toml_free_table() when done with a top-level TomlResult.
 *
 * Story: 55.6.2  Branch: feature/stdlib-55.6-toml
 */

#include <stdint.h>

typedef struct { void       *ok; int is_err; const char *err_msg; } TomlResult;
typedef struct { const char *ok; int is_err; const char *err_msg; } TomlStrResult;
typedef struct { int64_t     ok; int is_err; const char *err_msg; } TomlI64Result;
typedef struct { int         ok; int is_err; const char *err_msg; } TomlBoolResult;

TomlResult     toml_load(const char *src);
TomlResult     toml_load_file(const char *path);
void           toml_free_table(void *tab);

TomlStrResult  toml_get_str(void *tab, const char *key);
TomlI64Result  toml_get_i64(void *tab, const char *key);
TomlBoolResult toml_get_bool(void *tab, const char *key);
TomlResult     toml_get_section(void *tab, const char *key);

#endif /* TK_STDLIB_TOML_H */
