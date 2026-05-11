/*
 * json_glue.c — i64-ABI wrappers for std.json module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.json.
 */

#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* f64<->i64 bitcast helpers */
static double i64_to_f64(int64_t i) {
    double d;
    memcpy(&d, &i, sizeof(d));
    return d;
}
static int64_t f64_to_i64(double d) {
    int64_t i;
    memcpy(&i, &d, sizeof(i));
    return i;
}

int64_t tk_json_enc_w(int64_t val) {
    if (!val) return (int64_t)(intptr_t)"\"\"";
    return (int64_t)(intptr_t)json_enc((const char *)(intptr_t)val);
}

int64_t tk_json_dec_w(int64_t s) {
    if (!s) return 0;
    JsonResult res = json_dec((const char *)(intptr_t)s);
    if (res.is_err) return 0;
    Json *heap_json = (Json *)malloc(sizeof(Json));
    if (!heap_json) return 0;
    *heap_json = res.ok;
    return (int64_t)(intptr_t)heap_json;
}

int64_t tk_json_str_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    StrJsonResult res = json_str(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return 0;
    return (int64_t)(intptr_t)res.ok;
}

int64_t tk_json_i64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    I64JsonResult res = json_i64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}

int64_t tk_json_u64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    U64JsonResult res = json_u64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}

int64_t tk_json_f64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    F64JsonResult res = json_f64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return 0;
    return f64_to_i64(res.ok);
}

int64_t tk_json_bool_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    BoolJsonResult res = json_bool(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}

int64_t tk_json_parseobj_w(int64_t s) { return tk_json_dec_w(s); }
int64_t tk_json_stringify_w(int64_t val) { return tk_json_enc_w(val); }
int64_t tk_json_get_w(int64_t obj, int64_t key) { return tk_json_str_w(obj, key); }
int64_t tk_json_set_w(int64_t obj, int64_t key, int64_t val) { (void)obj; (void)key; (void)val; return 0; }
int64_t tk_json_has_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    return (int64_t)json_has(*jp, (const char *)(intptr_t)key);
}
int64_t tk_json_keys_w(int64_t obj) { (void)obj; return 0; }
int64_t tk_json_values_w(int64_t obj) { (void)obj; return 0; }

/* json extras (stubs) */
int64_t tk_json_getint_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_err_w(int64_t msg) { (void)msg; return 0; }
int64_t tk_json_ok_w(int64_t val) { (void)val; return 0; }
int64_t tk_json_serialize_w(int64_t obj) { (void)obj; return 0; }
int64_t tk_json_setint_w(int64_t obj, int64_t key, int64_t val) { (void)obj; (void)key; (void)val; return 0; }
int64_t tk_json_haskey_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_getbool_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_getstring_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
