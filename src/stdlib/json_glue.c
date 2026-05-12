/*
 * json_glue.c — i64-ABI wrappers for std.json module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.json.
 */

#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
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
int64_t tk_json_set_w(int64_t obj, int64_t key, int64_t val) {
    if (!obj || !key) return obj;
    /* Build a new JSON object by merging a single key:value pair.
     * Construct {"key":"val"} and merge with obj. */
    Json *jp = (Json *)(intptr_t)obj;
    const char *kstr = (const char *)(intptr_t)key;
    const char *vstr = val ? (const char *)(intptr_t)val : "";
    const char *enc_val = json_enc(vstr);
    /* Build {"key":enc_val} */
    size_t klen = strlen(kstr);
    size_t vlen = strlen(enc_val);
    size_t blen = 1 + klen + 2 + 1 + vlen + 1 + 1;
    char *patch = (char *)malloc(blen);
    if (!patch) return obj;
    snprintf(patch, blen, "{\"%s\":%s}", kstr, enc_val);
    free((void *)enc_val);
    Json j2 = { patch };
    JsonResult merged = json_merge(*jp, j2);
    free(patch);
    if (merged.is_err) return obj;
    Json *result = (Json *)malloc(sizeof(Json));
    if (!result) return obj;
    *result = merged.ok;
    return (int64_t)(intptr_t)result;
}
int64_t tk_json_has_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    return (int64_t)json_has(*jp, (const char *)(intptr_t)key);
}
int64_t tk_json_keys_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    StrArrayJsonResult r = json_keys(*jp);
    if (r.is_err || r.ok.len == 0) return 0;
    /* Return as toke array: [len, elem0, elem1, ...] */
    int64_t *block = (int64_t *)malloc((size_t)(r.ok.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)r.ok.len;
    for (uint64_t i = 0; i < r.ok.len; i++)
        block[i + 1] = (int64_t)(intptr_t)r.ok.data[i];
    free(r.ok.data); /* free the pointer array, strings are now owned by the toke array */
    return (int64_t)(intptr_t)(block + 1);
}
int64_t tk_json_values_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    /* Get keys first, then extract each value */
    StrArrayJsonResult kr = json_keys(*jp);
    if (kr.is_err || kr.ok.len == 0) return 0;
    int64_t *block = (int64_t *)malloc((size_t)(kr.ok.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)kr.ok.len;
    for (uint64_t i = 0; i < kr.ok.len; i++) {
        StrJsonResult vr = json_str(*jp, kr.ok.data[i]);
        if (vr.is_err)
            block[i + 1] = 0;
        else
            block[i + 1] = (int64_t)(intptr_t)vr.ok;
        free((void *)kr.ok.data[i]);
    }
    free(kr.ok.data);
    return (int64_t)(intptr_t)(block + 1);
}

/* json extras */
int64_t tk_json_getint_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}
int64_t tk_json_err_w(int64_t msg) {
    /* Encode an error as a JSON object: {"error":"msg"} */
    const char *mstr = msg ? (const char *)(intptr_t)msg : "error";
    const char *enc = json_enc(mstr);
    size_t elen = strlen(enc);
    size_t blen = 10 + elen + 2;
    char *buf = (char *)malloc(blen);
    if (!buf) return 0;
    snprintf(buf, blen, "{\"error\":%s}", enc);
    free((void *)enc);
    Json *jp = (Json *)malloc(sizeof(Json));
    if (!jp) { free(buf); return 0; }
    jp->raw = buf;
    return (int64_t)(intptr_t)jp;
}
int64_t tk_json_ok_w(int64_t val) {
    /* Encode an ok result as a JSON object: {"ok":"val"} */
    const char *vstr = val ? (const char *)(intptr_t)val : "";
    const char *enc = json_enc(vstr);
    size_t elen = strlen(enc);
    size_t blen = 7 + elen + 2;
    char *buf = (char *)malloc(blen);
    if (!buf) return 0;
    snprintf(buf, blen, "{\"ok\":%s}", enc);
    free((void *)enc);
    Json *jp = (Json *)malloc(sizeof(Json));
    if (!jp) { free(buf); return 0; }
    jp->raw = buf;
    return (int64_t)(intptr_t)jp;
}
int64_t tk_json_serialize_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    /* Return the raw JSON string */
    return jp->raw ? (int64_t)(intptr_t)jp->raw : 0;
}
int64_t tk_json_setint_w(int64_t obj, int64_t key, int64_t val) {
    if (!obj || !key) return obj;
    Json *jp = (Json *)(intptr_t)obj;
    const char *kstr = (const char *)(intptr_t)key;
    /* Build {"key":val} with integer value */
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "%lld", (long long)val);
    size_t klen = strlen(kstr);
    size_t blen = 1 + klen + 2 + 1 + strlen(vbuf) + 1 + 1;
    char *patch = (char *)malloc(blen);
    if (!patch) return obj;
    snprintf(patch, blen, "{\"%s\":%s}", kstr, vbuf);
    Json j2 = { patch };
    JsonResult merged = json_merge(*jp, j2);
    free(patch);
    if (merged.is_err) return obj;
    Json *result = (Json *)malloc(sizeof(Json));
    if (!result) return obj;
    *result = merged.ok;
    return (int64_t)(intptr_t)result;
}
int64_t tk_json_haskey_w(int64_t obj, int64_t key) {
    return tk_json_has_w(obj, key);
}
int64_t tk_json_len_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    U64JsonResult res = json_len(*jp);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}
int64_t tk_json_getbool_w(int64_t obj, int64_t key) {
    return tk_json_bool_w(obj, key);
}
int64_t tk_json_getstring_w(int64_t obj, int64_t key) {
    return tk_json_str_w(obj, key);
}
