/*
 * toon_glue.c — i64-ABI wrappers for std.toon (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=toon:std.toon` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "toon.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "file.h"
static int64_t f64_to_i64(double d){int64_t i;memcpy(&i,&d,sizeof(i));return i;}

int64_t tk_toon_parse_w(int64_t s) {
    if (!s) return 0;
    ToonResult r = toon_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_stringify_w(int64_t val) {
    if (!val) return 0;
    const char *s = toon_enc((const char *)(intptr_t)val);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_load_w(int64_t path) {
    if (!path) return 0;
    StrFileResult fr = file_read((const char *)(intptr_t)path);
    if (fr.is_err || !fr.ok) return 0;
    ToonResult r = toon_dec(fr.ok);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_print_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    if (s) printf("%s\n", s);
    return 0;
}

int64_t tk_toon_enc_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_dec_w(int64_t s) {
    if (!s) return 0;
    ToonResult r = toon_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_tojson_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    const char *json = toon_to_json(t->raw);
    return (int64_t)(intptr_t)json;
}

int64_t tk_toon_i64_w(int64_t v) {
    /* v is a Toon handle; extract .raw as an integer string */
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    return (int64_t)strtoll(t->raw, NULL, 10);
}

int64_t tk_toon_getint_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Toon *t = (Toon *)(intptr_t)obj;
    I64ToonResult r = toon_i64(*t, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)r.ok;
}

int64_t tk_toon_getstring_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Toon *t = (Toon *)(intptr_t)obj;
    StrToonResult r = toon_str(*t, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_toon_fromstr_w(int64_t s) {
    /* Alias for toon_dec */
    return tk_toon_dec_w(s);
}

int64_t tk_toon_bool_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    /* Parse raw value as boolean */
    if (strcmp(t->raw, "true") == 0 || strcmp(t->raw, "1") == 0) return 1;
    return 0;
}

int64_t tk_toon_deserialize_w(int64_t s) {
    /* Alias for toon_dec */
    return tk_toon_dec_w(s);
}

int64_t tk_toon_tostr_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_arr_w(void) {
    /* Create an empty ToonArray on the heap and return it as a
     * toke-format array pointer (block[-1]=count, block[0..n-1]=elements).
     * An empty array has count=0, so we allocate a 1-element block where
     * block[0] = count = 0, and return &block[1]. */
    ToonArray *ta = (ToonArray *)calloc(1, sizeof(ToonArray));
    if (!ta) return 0;
    ta->data = NULL;
    ta->len  = 0;
    return (int64_t)(intptr_t)ta;
}

int64_t tk_toon_f64_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    double d = strtod(t->raw, NULL);
    return f64_to_i64(d);
}

int64_t tk_toon_str_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    return (int64_t)(intptr_t)t->raw;
}
