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

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* json.getstr — alias for json.str */
int64_t tk_json_getstr_w(int64_t obj, int64_t key) {
    return tk_json_str_w(obj, key);
}

/* json.geti64 — alias for json.i64 */
int64_t tk_json_geti64_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}

/* json.geti32 — same as geti64 (all ints are i64 at ABI level) */
int64_t tk_json_geti32_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}

/* json.getf32 / json.getfloat — alias for json.f64 */
int64_t tk_json_getf32_w(int64_t obj, int64_t key) {
    return tk_json_f64_w(obj, key);
}
int64_t tk_json_getfloat_w(int64_t obj, int64_t key) {
    return tk_json_f64_w(obj, key);
}

/* json.getobj(obj, key) — extract a nested JSON object by key */
int64_t tk_json_getobj_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    JsonResult r = json_at(*jp, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    Json *heap = (Json *)malloc(sizeof(Json));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

/* json.getarr(obj, key) — extract a JSON array by key, return as toke array of Json* */
int64_t tk_json_getarr_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    JsonArrayResult r = json_arr(*jp, (const char *)(intptr_t)key);
    if (r.is_err || r.ok.len == 0) return 0;
    uint64_t count = r.ok.len;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        Json *elem = (Json *)malloc(sizeof(Json));
        if (!elem) { block[0] = (int64_t)i; break; }
        *elem = r.ok.data[i];
        block[i + 1] = (int64_t)(intptr_t)elem;
    }
    free(r.ok.data);
    return (int64_t)(intptr_t)(block + 1);
}

/* json.getarray — alias for getarr */
int64_t tk_json_getarray_w(int64_t obj, int64_t key) {
    return tk_json_getarr_w(obj, key);
}

/* json.getopt(obj, key) — get string value, return 0 (none) if missing */
int64_t tk_json_getopt_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    if (json_is_null(*jp, (const char *)(intptr_t)key)) return 0;
    StrJsonResult r = json_str(*jp, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

/* json.getval(obj, key) — get raw JSON value at key as string */
int64_t tk_json_getval_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    JsonResult r = json_at(*jp, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return r.ok.raw ? (int64_t)(intptr_t)r.ok.raw : 0;
}

/* json.isnullkey(obj, key) — return 1 if key is null or missing */
int64_t tk_json_isnullkey_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 1;
    Json *jp = (Json *)(intptr_t)obj;
    return (int64_t)json_is_null(*jp, (const char *)(intptr_t)key);
}

/* json.encstr(s) — JSON-escape a string (with quotes) */
int64_t tk_json_encstr_w(int64_t s) {
    if (!s) return (int64_t)(intptr_t)"\"\"";
    return (int64_t)(intptr_t)json_enc((const char *)(intptr_t)s);
}

/* json.encode(val) — alias for json.enc */
int64_t tk_json_encode_w(int64_t val) {
    return tk_json_enc_w(val);
}

/* json.decode(s) — alias for json.dec */
int64_t tk_json_decode_w(int64_t s) {
    return tk_json_dec_w(s);
}

/* json.quote(s) — JSON-escape a string value (same as encstr) */
int64_t tk_json_quote_w(int64_t s) {
    return tk_json_encstr_w(s);
}

/* json.obj(raw) — wrap a raw JSON string as a Json* object */
int64_t tk_json_obj_w(int64_t raw) {
    if (!raw) return 0;
    const char *s = (const char *)(intptr_t)raw;
    Json *jp = (Json *)malloc(sizeof(Json));
    if (!jp) return 0;
    /* Duplicate the string so the Json owns its memory */
    jp->raw = strdup(s);
    return (int64_t)(intptr_t)jp;
}

/* json.arr(raw) — parse a JSON array string into a toke array of Json* */
int64_t tk_json_arr_w(int64_t raw) {
    if (!raw) return 0;
    /* Parse as a top-level JSON, then get its length and index elements */
    Json j = { (const char *)(intptr_t)raw };
    U64JsonResult lenr = json_len(j);
    if (lenr.is_err || lenr.ok == 0) return 0;
    uint64_t count = lenr.ok;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        JsonResult r = json_index(j, i);
        if (r.is_err) { block[0] = (int64_t)i; break; }
        Json *elem = (Json *)malloc(sizeof(Json));
        if (!elem) { block[0] = (int64_t)i; break; }
        *elem = r.ok;
        block[i + 1] = (int64_t)(intptr_t)elem;
    }
    return (int64_t)(intptr_t)(block + 1);
}

/* json.arrget(arr, idx) — get element at index from a toke array of Json* */
int64_t tk_json_arrget_w(int64_t arr, int64_t idx) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return 0;
    return ptr[idx];
}

/* json.arrlen(arr) — length of a toke array */
int64_t tk_json_arrlen_w(int64_t arr) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    return ptr[-1];
}

/* json.entries(obj) — return array of "key=value" strings */
int64_t tk_json_entries_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    StrArrayJsonResult kr = json_keys(*jp);
    if (kr.is_err || kr.ok.len == 0) return 0;
    uint64_t count = kr.ok.len;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        const char *k = kr.ok.data[i];
        StrJsonResult vr = json_str(*jp, k);
        const char *v = vr.is_err ? "" : vr.ok;
        size_t klen = strlen(k), vlen = strlen(v);
        char *entry = (char *)malloc(klen + 1 + vlen + 1);
        if (entry) {
            memcpy(entry, k, klen);
            entry[klen] = '=';
            memcpy(entry + klen + 1, v, vlen);
            entry[klen + 1 + vlen] = '\0';
        }
        block[i + 1] = (int64_t)(intptr_t)entry;
        free((void *)k);
    }
    free(kr.ok.data);
    return (int64_t)(intptr_t)(block + 1);
}

/* json.kv(key, value) — build {"key":"value"} JSON object */
int64_t tk_json_kv_w(int64_t key, int64_t value) {
    if (!key) return 0;
    const char *k = (const char *)(intptr_t)key;
    const char *v = value ? (const char *)(intptr_t)value : "";
    const char *enc_v = json_enc(v);
    size_t klen = strlen(k), vlen = strlen(enc_v);
    size_t blen = 2 + klen + 2 + vlen + 2;
    char *buf = (char *)malloc(blen);
    if (!buf) { free((void *)enc_v); return 0; }
    snprintf(buf, blen, "{\"%s\":%s}", k, enc_v);
    free((void *)enc_v);
    Json *jp = (Json *)malloc(sizeof(Json));
    if (!jp) { free(buf); return 0; }
    jp->raw = buf;
    return (int64_t)(intptr_t)jp;
}

/* json.parsearray(s) — parse a JSON array string, return toke array of strings */
int64_t tk_json_parsearray_w(int64_t s) {
    if (!s) return 0;
    Json j = { (const char *)(intptr_t)s };
    U64JsonResult lenr = json_len(j);
    if (lenr.is_err || lenr.ok == 0) return 0;
    uint64_t count = lenr.ok;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        JsonResult r = json_index(j, i);
        if (r.is_err) { block[0] = (int64_t)i; break; }
        /* Return the raw JSON text for each element */
        block[i + 1] = r.ok.raw ? (int64_t)(intptr_t)r.ok.raw : 0;
    }
    return (int64_t)(intptr_t)(block + 1);
}

/* json.strarr(arr) — encode a toke string array as JSON array string */
int64_t tk_json_strarr_w(int64_t arr) {
    if (!arr) return (int64_t)(intptr_t)"[]";
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (len <= 0) return (int64_t)(intptr_t)"[]";
    /* Build JSON array: ["s1","s2",...] */
    size_t cap = 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return (int64_t)(intptr_t)"[]";
    buf[0] = '[';
    size_t pos = 1;
    for (int64_t i = 0; i < len; i++) {
        const char *s = (const char *)(intptr_t)ptr[i];
        const char *enc = json_enc(s ? s : "");
        size_t elen = strlen(enc);
        size_t needed = pos + elen + 2; /* comma + bracket */
        if (needed > cap) {
            while (needed > cap) cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); free((void *)enc); return (int64_t)(intptr_t)"[]"; }
            buf = tmp;
        }
        if (i > 0) buf[pos++] = ',';
        memcpy(buf + pos, enc, elen);
        pos += elen;
        free((void *)enc);
    }
    if (pos + 2 > cap) {
        char *tmp = (char *)realloc(buf, pos + 2);
        if (!tmp) { free(buf); return (int64_t)(intptr_t)"[]"; }
        buf = tmp;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    return (int64_t)(intptr_t)buf;
}

/* json.strarray — alias for strarr */
int64_t tk_json_strarray_w(int64_t arr) {
    return tk_json_strarr_w(arr);
}

/* json.trystr(obj, key) — try to get string, return 0 on failure */
int64_t tk_json_trystr_w(int64_t obj, int64_t key) {
    return tk_json_getopt_w(obj, key);
}
int64_t tk_json_trystrkey_w(int64_t obj, int64_t key) {
    return tk_json_getopt_w(obj, key);
}

/* json.tryint(obj, key) — try to get int, return 0 on failure */
int64_t tk_json_tryint_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}
int64_t tk_json_tryintkey_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}

/* json.tryfloat(obj, key) — try to get float, return 0.0 on failure */
int64_t tk_json_tryfloat_w(int64_t obj, int64_t key) {
    return tk_json_f64_w(obj, key);
}
int64_t tk_json_tryfloatkey_w(int64_t obj, int64_t key) {
    return tk_json_f64_w(obj, key);
}

/* json.trybool(obj, key) — try to get bool, return 0 on failure */
int64_t tk_json_trybool_w(int64_t obj, int64_t key) {
    return tk_json_bool_w(obj, key);
}
int64_t tk_json_tryboolkey_w(int64_t obj, int64_t key) {
    return tk_json_bool_w(obj, key);
}

/* json.tryarr(obj, key) — try to get array, return 0 on failure */
int64_t tk_json_tryarr_w(int64_t obj, int64_t key) {
    return tk_json_getarr_w(obj, key);
}

/* json.getregulations — loke-specific, stub returning empty array */
int64_t tk_json_getregulations_w(int64_t obj) {
    (void)obj;
    int64_t *block = (int64_t *)malloc(sizeof(int64_t));
    if (!block) return 0;
    block[0] = 0;
    return (int64_t)(intptr_t)(block + 1);
}
