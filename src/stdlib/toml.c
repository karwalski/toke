/*
 * toml.c — Implementation of the std.toml standard library module.
 *
 * FFI wrapper around tomlc99 (Tom Pawlak, MIT licence).
 * tomlc99 is vendored at stdlib/vendor/tomlc99/.
 *
 * Story: 55.6.2  Branch: feature/stdlib-55.6-toml
 */

#include "toml.h"
#include "../../stdlib/vendor/tomlc99/toml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Internal helper — create a TomlErr result.
 */
static TomlResult toml_err(const char *msg)
{
    TomlResult r = {NULL, 1, NULL};
    r.err_msg = msg ? msg : "toml error";
    return r;
}

/*
 * toml_load — parse a TOML string.
 *
 * Returns a TomlResult wrapping a heap-allocated toml_table_t *.
 * On success, caller owns the table and must call toml_free() via toml_free_table().
 * On error, err_msg describes the parse failure.
 */
TomlResult toml_load(const char *src)
{
    TomlResult r = {NULL, 0, NULL};
    if (!src) return toml_err("null input");

    char errbuf[256] = {0};
    toml_table_t *tab = toml_parse((char *)src, errbuf, sizeof(errbuf));
    if (!tab) {
        /* Copy errbuf into heap so caller doesn't need to worry about lifetime. */
        char *msg = malloc(sizeof(errbuf));
        if (msg) memcpy(msg, errbuf, sizeof(errbuf));
        r.is_err = 1;
        r.err_msg = msg ? msg : "parse error";
        return r;
    }
    r.ok = tab;
    return r;
}

/*
 * toml_load_file — read a TOML file and parse it.
 */
TomlResult toml_load_file(const char *path)
{
    if (!path) return toml_err("null path");
    FILE *f = fopen(path, "rb");
    if (!f) return toml_err("file not found");

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return toml_err("ftell failed"); }
    rewind(f);

    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); return toml_err("allocation failed"); }
    size_t nread = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[nread] = '\0';

    TomlResult r = toml_load(src);
    free(src);
    return r;
}

void toml_free_table(void *tab)
{
    if (tab) toml_free((toml_table_t *)tab);
}

/*
 * toml_get_str — get a string value from a table by key.
 */
TomlStrResult toml_get_str(void *tab, const char *key)
{
    TomlStrResult r = {NULL, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_string_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not a string"; return r; }
    r.ok = d.u.s;  /* heap-allocated by tomlc99 */
    return r;
}

/*
 * toml_get_i64 — get an integer value from a table by key.
 */
TomlI64Result toml_get_i64(void *tab, const char *key)
{
    TomlI64Result r = {0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_int_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not an integer"; return r; }
    r.ok = (int64_t)d.u.i;
    return r;
}

/*
 * toml_get_bool — get a boolean value from a table by key.
 */
TomlBoolResult toml_get_bool(void *tab, const char *key)
{
    TomlBoolResult r = {0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_bool_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not a boolean"; return r; }
    r.ok = d.u.b ? 1 : 0;
    return r;
}

/*
 * toml_get_section — get a sub-table by key.
 * Returns a non-owning pointer into the parent table; do not free separately.
 */
TomlResult toml_get_section(void *tab, const char *key)
{
    TomlResult r = {NULL, 0, NULL};
    if (!tab || !key) return toml_err("null argument");
    toml_table_t *sub = toml_table_in((toml_table_t *)tab, key);
    if (!sub) { r.is_err = 1; r.err_msg = "section not found"; return r; }
    r.ok = sub;
    return r;
}
