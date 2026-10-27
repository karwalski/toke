/* SPDX-License-Identifier: MIT */
/*
 * json_glue.c — i64-ABI wrappers for std.json module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.json.
 */

#include "json.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "tk_runtime.h" /* 127.193: tk_current_error + tk_err_box (127.109) */
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
    const char *e = json_enc((const char *)(intptr_t)val);
    return (int64_t)(intptr_t)(e ? e : "\"\"");
}


/*
 * 127.193 — the scalar accessors keep @tk_current_error truthful.
 *
 * They return the value itself, and false, 0 and 0.0 are values.  Under the
 * 0/null sentinel a stored false was indistinguishable from a failure, so
 * `mt json.getbool(o;"off"){$ok:v v;$err:e true}` took the $err arm and
 * answered true — test_json_full.tk's "FAIL json.getbool false".  Every path
 * below now sets the slot: 0 on success, and on failure a {tag, msg} box
 * whose tag is the JsonErr variant index (Parse, Type, Missing — the order of
 * stdlib/json.tki and of JsonErrKind), the same shape file_raise() builds
 * (135.12).  llvm.c's is_stdlib_slot_wrapper() makes the slot authoritative
 * for exactly these wrappers.  The success store is not optional: without it
 * an EARLIER failure left in the slot would turn a success into an error
 * (127.123).
 */
static int64_t json_raise(JsonErrKind kind, const char *msg) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)kind;
    box[1] = (int64_t)(intptr_t)(msg ? msg : "");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/*
 * 127.241 — json.dec and json.str fill the slot too.
 *
 * Both are declared `T!JsonErr` (Json!JsonErr, str!JsonErr), but until this
 * story their glue returned a bare 0 on failure and never touched the slot,
 * so they sat on stdlib_tables.c's bare-glue list and their `$err` arm bound
 * nothing: the parser's own message ("json.dec: unexpected character ...")
 * and the Missing / Type distinction were unreachable from toke.  Now every
 * failure raises the same {JsonErr variant, msg} box as the scalar accessors
 * and every success clears the slot, so is_stdlib_slot_wrapper() derives
 * them as slot wrappers and `mt json.dec(s){$err:e mt e{$parse:m …}}` binds.
 */
int64_t tk_json_dec_w(int64_t s) {
    if (!s) return json_raise(JSON_ERR_PARSE, "json.dec: null input");
    JsonResult res = json_dec((const char *)(intptr_t)s);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    Json *heap_json = (Json *)malloc(sizeof(Json));
    if (!heap_json) return json_raise(JSON_ERR_PARSE, "json.dec: out of memory");
    *heap_json = res.ok;
    tk_current_error = 0;
    return (int64_t)(intptr_t)heap_json;
}

int64_t tk_json_str_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.str: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    StrJsonResult res = json_str(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)res.ok;
}

int64_t tk_json_i64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.i64: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    I64JsonResult res = json_i64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    tk_current_error = 0;
    return (int64_t)res.ok;
}

int64_t tk_json_u64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.u64: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    U64JsonResult res = json_u64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    tk_current_error = 0;
    return (int64_t)res.ok;
}

int64_t tk_json_f64_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.f64: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    F64JsonResult res = json_f64(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    tk_current_error = 0;
    return f64_to_i64(res.ok);
}

int64_t tk_json_bool_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.bool: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    BoolJsonResult res = json_bool(*jp, (const char *)(intptr_t)key);
    if (res.is_err) return json_raise(res.err.kind, res.err.msg);
    tk_current_error = 0;
    return (int64_t)res.ok;
}

/* json.at(doc, path) -> Json!JsonErr — 138.12, the typed nested read.
 *
 * std.json's typed accessors read one key of a flat object, so a nested
 * value (loke T-32: usage.input_tokens) had no typed path at all, and the
 * tempting workaround -- the untyped json.getval, whose i64 result the
 * checker let into json.i64's Json parameter -- segfaulted, because getval
 * hands back the value's raw TEXT, not a Json handle.  json.at walks a dotted
 * path ("usage.input_tokens", "results.0.id": a segment on an array is a
 * decimal index) through json_at and returns a Json the typed accessors take.
 * A key that itself contains '.' cannot be named.  On failure it returns 0
 * (the sentinel this wrapper is decided by) and also sets the error slot. */
int64_t tk_json_at_w(int64_t obj, int64_t path) {
    if (!obj || !path) return json_raise(JSON_ERR_MISSING, "json.at: null object or path");
    Json *jp = (Json *)(intptr_t)obj;
    JsonResult r = json_at(*jp, (const char *)(intptr_t)path);
    if (r.is_err) return json_raise(r.err.kind, r.err.msg);
    Json *heap = (Json *)malloc(sizeof(Json));
    if (!heap) return json_raise(JSON_ERR_PARSE, "json.at: out of memory");
    *heap = r.ok;
    tk_current_error = 0;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_json_parseobj_w(int64_t s) { return tk_json_dec_w(s); }
int64_t tk_json_stringify_w(int64_t val) { return tk_json_enc_w(val); }
/* ── 113.B.5: the typed object surface ──────────────────────────────────
 *
 * json.get / keys / entries / has / haskey were reachable but undeclared
 * ("gen":"abi", i64 in and out), and set / setint built their patch with
 * snprintf("{\"%s\":...}") -- the key pasted in unescaped, so a key holding
 * a quote produced text that is not JSON -- then went through json_merge,
 * which moves a replaced member to the end, and on any failure handed back
 * the input object as if the write had happened.  Each wrapper below is
 * declared `T!jsonerr` in json.tki (so is_stdlib_slot_wrapper() makes the
 * slot authoritative) and keeps the slot truthful on every path: a JsonErr
 * box on failure, 0 on success.
 */

/* json_handle — a heap Json owning `r.ok`, or a raised JsonErr. */
static int64_t json_handle(JsonResult r, const char *who) {
    if (r.is_err) return json_raise(r.err.kind, r.err.msg);
    Json *heap = (Json *)malloc(sizeof(Json));
    if (!heap) { free((void *)r.ok.raw); return json_raise(JSON_ERR_PARSE, who); }
    *heap = r.ok;
    tk_current_error = 0;
    return (int64_t)(intptr_t)heap;
}

/* json.get(doc, key) -> jsonval!jsonerr: the member named exactly `key`, of
 * any JSON type.  json.at is the dotted-path walk; this names any key,
 * including one holding '.', so json.keys + json.get visits every member. */
int64_t tk_json_get_w(int64_t obj, int64_t key) {
    if (!obj || !key) return json_raise(JSON_ERR_MISSING, "json.get: null object or key");
    Json *jp = (Json *)(intptr_t)obj;
    return json_handle(json_member(*jp, (const char *)(intptr_t)key),
                       "json.get: out of memory");
}

/* json_setraw — the shared body of json.set*: member `key` of `obj` set to
 * the JSON text `vtext` (owned by the caller). */
static int64_t json_setraw(const Json *jp, int64_t key, const char *vtext, const char *who) {
    if (!jp) return json_raise(JSON_ERR_TYPE, who);
    if (!key) return json_raise(JSON_ERR_MISSING, who);
    if (!vtext) return json_raise(JSON_ERR_PARSE, who);
    return json_handle(json_setmember(*jp, (const char *)(intptr_t)key, vtext), who);
}

/* json.set(doc, key, v:str) -> jsonval!jsonerr: v as a JSON string. */
int64_t tk_json_set_w(int64_t obj, int64_t key, int64_t val) {
    Json *jp = (Json *)(intptr_t)obj;
    const char *enc = json_enc(val ? (const char *)(intptr_t)val : "");
    int64_t h = json_setraw(jp, key, enc, "json.set: out of memory");
    free((void *)enc);
    return h;
}

/* json.setint(doc, key, v:i64) -> jsonval!jsonerr: v as a JSON number. */
int64_t tk_json_setint_w(int64_t obj, int64_t key, int64_t val) {
    char vbuf[24];
    snprintf(vbuf, sizeof vbuf, "%lld", (long long)val);
    return json_setraw((Json *)(intptr_t)obj, key, vbuf, "json.setint: out of memory");
}

/* json.setf64(doc, key, v:f64) -> jsonval!jsonerr: v (its i64 bit pattern,
 * this glue's float ABI) as a JSON number, shortest round-tripping form.
 * NaN and the infinities have no JSON spelling: $type, nothing written. */
static void j_fmt_f64(char *buf, size_t n, double v);
int64_t tk_json_setf64_w(int64_t obj, int64_t key, int64_t bits) {
    double v = i64_to_f64(bits);
    if (v != v || v - v != 0.0)
        return json_raise(JSON_ERR_TYPE, "json.setf64: not a finite number");
    char vbuf[40];
    j_fmt_f64(vbuf, sizeof vbuf, v);
    return json_setraw((Json *)(intptr_t)obj, key, vbuf, "json.setf64: out of memory");
}

/* json.setbool(doc, key, v:bool) -> jsonval!jsonerr: true / false. */
int64_t tk_json_setbool_w(int64_t obj, int64_t key, int64_t b) {
    return json_setraw((Json *)(intptr_t)obj, key, b ? "true" : "false", "json.setbool: out of memory");
}

/* json.setval(doc, key, v:jsonval) -> jsonval!jsonerr: v's JSON text, so an
 * object or array (json.newobj, json.dec, json.at, json.get) nests.  A
 * $jsonval whose text is not one JSON value is $parse, never pasted in. */
int64_t tk_json_setval_w(int64_t obj, int64_t key, int64_t val) {
    if (!val) return json_raise(JSON_ERR_PARSE, "json.setval: null value");
    const Json *vp = (const Json *)(intptr_t)val;
    if (!vp->raw) return json_raise(JSON_ERR_PARSE, "json.setval: null value");
    return json_setraw((Json *)(intptr_t)obj, key, vp->raw, "json.setval: out of memory");
}

/* json.newobj() -> jsonval: a new empty object, {}. */
int64_t tk_json_newobj_w(void) {
    Json *jp = (Json *)malloc(sizeof(Json));
    char *raw = (char *)malloc(3);
    if (!jp || !raw) { free(jp); free(raw); return 0; }
    memcpy(raw, "{}", 3);
    jp->raw = raw;
    return (int64_t)(intptr_t)jp;
}

/* json.has(doc, key) -> bool: 1 when the object has a member named `key`
 * (escapes decoded, 113.B.5); 0 for a missing key and for a non-object. */
int64_t tk_json_has_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    return (int64_t)json_has(*jp, (const char *)(intptr_t)key);
}

/* json.keys(doc) -> [str]!jsonerr: the member names in document order,
 * escapes decoded.  {} is an empty array (a real handle of length 0); a
 * non-object is $type.  Both used to return a bare 0. */
int64_t tk_json_keys_w(int64_t obj) {
    if (!obj) return json_raise(JSON_ERR_TYPE, "json.keys: null object");
    Json *jp = (Json *)(intptr_t)obj;
    StrArrayJsonResult r = json_keys(*jp);
    if (r.is_err) return json_raise(r.err.kind, r.err.msg);
    int64_t h = tk_arr_alloc((int64_t)r.ok.len, (int64_t)r.ok.len);
    if (!h) {
        for (uint64_t i = 0; i < r.ok.len; i++) free((void *)r.ok.data[i]);
        free(r.ok.data);
        return json_raise(JSON_ERR_PARSE, "json.keys: out of memory");
    }
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < r.ok.len; i++)
        block[i] = (int64_t)(intptr_t)r.ok.data[i];
    free(r.ok.data); /* the strings are owned by the toke array now */
    tk_current_error = 0;
    return h;
}
int64_t tk_json_values_w(int64_t obj) {
    if (!obj) return 0;
    Json *jp = (Json *)(intptr_t)obj;
    /* Get keys first, then extract each value */
    StrArrayJsonResult kr = json_keys(*jp);
    if (kr.is_err || kr.ok.len == 0) return 0;
    int64_t h = tk_arr_alloc((int64_t)kr.ok.len, (int64_t)kr.ok.len);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < kr.ok.len; i++) {
        StrJsonResult vr = json_str(*jp, kr.ok.data[i]);
        if (vr.is_err)
            block[i] = 0;
        else
            block[i] = (int64_t)(intptr_t)vr.ok;
        free((void *)kr.ok.data[i]);
    }
    free(kr.ok.data);
    return h;
}

/* json extras */
int64_t tk_json_getint_w(int64_t obj, int64_t key) {
    return tk_json_i64_w(obj, key);
}
int64_t tk_json_err_w(int64_t msg) {
    /* Encode an error as a JSON object: {"error":"msg"} */
    const char *mstr = msg ? (const char *)(intptr_t)msg : "error";
    const char *enc = json_enc(mstr);
    if (!enc) return 0;
    size_t elen = strlen(enc);
    size_t blen = 10 + elen + 2;
    char *buf = (char *)malloc(blen);
    if (!buf) { free((void *)enc); return 0; }
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
    if (!enc) return 0;
    size_t elen = strlen(enc);
    size_t blen = 7 + elen + 2;
    char *buf = (char *)malloc(blen);
    if (!buf) { free((void *)enc); return 0; }
    snprintf(buf, blen, "{\"ok\":%s}", enc);
    free((void *)enc);
    Json *jp = (Json *)malloc(sizeof(Json));
    if (!jp) { free(buf); return 0; }
    jp->raw = buf;
    return (int64_t)(intptr_t)jp;
}
/* json.serialize(doc) -> str: the document's JSON text -- what a builder
 * (json.newobj + json.set*) hands to http/file/io for output.  113.B.5: ""
 * rather than a null for a null document. */
int64_t tk_json_serialize_w(int64_t obj) {
    if (!obj) return (int64_t)(intptr_t)"";
    Json *jp = (Json *)(intptr_t)obj;
    return (int64_t)(intptr_t)(jp->raw ? jp->raw : "");
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
    int64_t h = tk_arr_alloc((int64_t)count, (int64_t)count);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < count; i++) {
        Json *elem = (Json *)malloc(sizeof(Json));
        if (!elem) { tk_arr_setlen(h, (int64_t)i); break; }
        *elem = r.ok.data[i];
        block[i] = (int64_t)(intptr_t)elem;
    }
    free(r.ok.data);
    return h;
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
/* json.getval(doc, path) -> str — the raw JSON TEXT of the value at path
 * ("" if there is none; a present value's text is never empty).  138.12:
 * declared (Json;str)->str, it was an untyped (i64;i64)->i64, so its result
 * -- a C string -- was accepted where json.i64 wants a Json handle and
 * segfaulted there (loke T-32).  For a Json to read further, use json.at. */
int64_t tk_json_getval_w(int64_t obj, int64_t key) {
    if (!obj || !key) return (int64_t)(intptr_t)"";
    Json *jp = (Json *)(intptr_t)obj;
    JsonResult r = json_at(*jp, (const char *)(intptr_t)key);
    if (r.is_err || !r.ok.raw) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)r.ok.raw;
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
    const char *e = json_enc((const char *)(intptr_t)s);
    return (int64_t)(intptr_t)(e ? e : "\"\"");
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

/* json.arr(doc, key) — [Json]!JsonErr (127.29).
 *
 * `doc` is the Json* handle returned by json.dec (NOT a raw string — the
 * pre-127.29 wrapper took a single raw-string argument while the compiler,
 * per stdlib/json.tki, passes (Json*, key); the Json* was read as a C string,
 * json_len failed on the garbage, and every call collapsed to `$err`).
 *
 *   key == ""  → the document itself must be a JSON array (top-level form)
 *   key != ""  → `doc` must be an object and `key` must hold a JSON array
 *
 * Returns a toke array handle of heap Json* elements (each element's raw is
 * an owned copy of its JSON text, so the sibling accessors json.str/i64/...
 * work on it), or 0 (`$err`) when the value is missing or not an array.
 * An empty array is a valid non-zero handle of length 0, never `$err`.
 */
int64_t tk_json_arr_w(int64_t obj, int64_t key) {
    /* 127.241: every failure raises a JsonErr box, every success clears the
     * slot -- see tk_json_dec_w. */
    if (!obj) return json_raise(JSON_ERR_MISSING, "json.arr: null object");
    Json *jp = (Json *)(intptr_t)obj;
    const char *k = key ? (const char *)(intptr_t)key : "";
    JsonArrayResult r = k[0] ? json_arr(*jp, k) : json_arr_top(*jp);
    if (r.is_err) return json_raise(r.err.kind, r.err.msg);
    uint64_t count = r.ok.len;
    int64_t h = tk_arr_alloc((int64_t)count, (int64_t)count);
    if (!h) { free(r.ok.data); return json_raise(JSON_ERR_PARSE, "json.arr: out of memory"); }
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < count; i++) {
        Json *elem = (Json *)malloc(sizeof(Json));
        if (!elem) { tk_arr_setlen(h, (int64_t)i); break; }
        *elem = r.ok.data[i];
        block[i] = (int64_t)(intptr_t)elem;
    }
    free(r.ok.data);
    tk_current_error = 0;
    return h;
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

/* json.entries(doc) -> [str]!jsonerr — 113.B.5: one "key=value" string per
 * member, in document order: the key with its escapes decoded, then '=',
 * then the member's value as JSON TEXT (a string keeps its quotes and
 * escapes), so no value is lost or confused with another -- before, every
 * non-string value read as "" and the string "1" and the number 1 looked the
 * same.  Split at the FIRST '=' (a key holding '=' cannot be told apart: use
 * json.keys + json.get to read such a member).  {} is an empty array, a
 * non-object $type; both were a bare 0. */
int64_t tk_json_entries_w(int64_t obj) {
    if (!obj) return json_raise(JSON_ERR_TYPE, "json.entries: null object");
    Json *jp = (Json *)(intptr_t)obj;
    StrArrayJsonResult kr = json_keys(*jp);
    if (kr.is_err) return json_raise(kr.err.kind, kr.err.msg);
    uint64_t count = kr.ok.len;
    int64_t h = tk_arr_alloc((int64_t)count, (int64_t)count);
    int oom = !h;
    int64_t *block = h ? (int64_t *)(intptr_t)h : NULL;
    for (uint64_t i = 0; i < count; i++) {
        const char *k = kr.ok.data[i];
        if (!oom) {
            JsonResult vr = json_member(*jp, k);
            const char *v = vr.is_err ? "null" : vr.ok.raw;
            size_t klen = strlen(k), vlen = strlen(v);
            char *entry = (char *)malloc(klen + 1 + vlen + 1);
            if (entry) {
                memcpy(entry, k, klen);
                entry[klen] = '=';
                memcpy(entry + klen + 1, v, vlen + 1);
                block[i] = (int64_t)(intptr_t)entry;
            } else {
                oom = 1;
            }
            if (!vr.is_err) free((void *)vr.ok.raw);
        }
        free((void *)k);
    }
    free(kr.ok.data);
    if (oom) return json_raise(JSON_ERR_PARSE, "json.entries: out of memory");
    tk_current_error = 0;
    return h;
}

/* json.kv(key, value) — build {"key":"value"} JSON object */
int64_t tk_json_kv_w(int64_t key, int64_t value) {
    if (!key) return 0;
    const char *k = (const char *)(intptr_t)key;
    const char *v = value ? (const char *)(intptr_t)value : "";
    const char *enc_v = json_enc(v);
    if (!enc_v) return 0;
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
    int64_t h = tk_arr_alloc((int64_t)count, (int64_t)count);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < count; i++) {
        JsonResult r = json_index(j, i);
        if (r.is_err) { tk_arr_setlen(h, (int64_t)i); break; }
        /* Return the raw JSON text for each element */
        block[i] = r.ok.raw ? (int64_t)(intptr_t)r.ok.raw : 0;
    }
    return h;
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
        if (!enc) { free(buf); return (int64_t)(intptr_t)"[]"; }
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
/* json.getregulations(doc) — the array under "regulations" (json.getarr's
 * shape), or an empty array.  138.11: this was a stub that ignored its
 * argument and always returned an empty array (loke T-12). */
int64_t tk_json_getregulations_w(int64_t obj) {
    int64_t h = obj ? tk_json_getarr_w(obj, (int64_t)(intptr_t)"regulations") : 0;
    return h ? h : tk_arr_alloc(0, 0);
}

/* ── 127.43: typed JSON number / bool encoding ──────────────────────────
 *
 * `j.enc(x)` and `j.print(x)` reach the untyped i64 ABI, where an integer and
 * a heap pointer are indistinguishable. The pre-127.43 path sent *every*
 * argument through tk_json_enc_w / tk_json_print, which read the i64 as a
 * `char *`: an i64 whose value happens to be a valid-looking address (or, for
 * tk_json_print, any value outside +-1e9) was dereferenced, so
 * `j.print(4294967296)` walked into unmapped memory (SIGSEGV, exit 139) and
 * `j.enc(2147483648)` returned garbage.
 *
 * These wrappers are the typed targets the j.enc/j.print dispatch selects once
 * the argument's checker type is known (i64 / bool / f64). They never
 * dereference the value, so the full i64 range — 2^31, 2^32, 2^63-1 and the
 * negatives — encodes exactly.
 */

/* json.encnum(v) — encode an i64 as a JSON number (no quotes, full range). */
int64_t tk_json_encnum_w(int64_t v) {
    char buf[24];  /* -9223372036854775808 = 20 chars + NUL */
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    if (n < 0) return (int64_t)(intptr_t)"0";
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) return (int64_t)(intptr_t)"0";
    memcpy(out, buf, (size_t)n + 1);
    return (int64_t)(intptr_t)out;
}

/* json.encbool(b) — encode a bool as the JSON literal true/false. */
int64_t tk_json_encbool_w(int64_t b) {
    const char *s = b ? "true" : "false";
    char *out = (char *)malloc(strlen(s) + 1);
    if (!out) return (int64_t)(intptr_t)"false";
    strcpy(out, s);
    return (int64_t)(intptr_t)out;
}

/* json.encf64(bits) — encode a double (passed as its i64 bit pattern, the
 * float ABI used throughout this glue) as a JSON number. Uses the shortest of
 * %.15g/%.16g/%.17g that round-trips, so no precision is lost and ordinary
 * values stay readable (1.5 -> "1.5", not "1.5000000000000000"). */
static void j_fmt_f64(char *buf, size_t n, double v) {
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, n, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) return;
    }
}
int64_t tk_json_encf64_w(int64_t bits) {
    char buf[40];
    j_fmt_f64(buf, sizeof buf, i64_to_f64(bits));
    char *out = (char *)malloc(strlen(buf) + 1);
    if (!out) return (int64_t)(intptr_t)"0";
    strcpy(out, buf);
    return (int64_t)(intptr_t)out;
}

/* ── 127.44 / 127.45: typed JSON array printing and encoding ────────────
 *
 * A toke array reaches the i64 ABI as a bare backing-block handle, so an
 * array returned by a user function or by .map/.filter was indistinguishable
 * from an i64 or a `char *` at the call boundary. tk_json_print's byte
 * heuristic guessed "string" whenever the first element's low byte happened to
 * be printable ASCII — `j.print(f(@(122;5)))` printed "z" — and the flat
 * tk_json_print_arr printed the element words verbatim, so an @@i64 came out
 * as a list of addresses.
 *
 * These wrappers take the shape from the compiler instead of guessing it:
 *
 *   depth  1 = flat array, 2 = array of arrays, ... (recursion per level)
 *   kind   element kind at the innermost level (TK_JKIND_* below)
 *
 * A 0 handle at any level encodes as JSON null. The encoder builds the text in
 * one growable buffer and returns an owned NUL-terminated string; the printer
 * writes the same text plus a newline, matching the other tk_json_print_*.
 */

#define TK_JKIND_I64  0
#define TK_JKIND_STR  1
#define TK_JKIND_F64  2
#define TK_JKIND_BOOL 3

typedef struct { char *p; size_t len, cap; } JBuf;

static int jbuf_more(JBuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + need + 1) cap *= 2;
    char *np = (char *)realloc(b->p, cap);
    if (!np) return 0;
    b->p = np; b->cap = cap;
    return 1;
}
static void jbuf_put(JBuf *b, const char *s, size_t n) {
    if (!jbuf_more(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}
static void jbuf_puts(JBuf *b, const char *s) { jbuf_put(b, s, strlen(s)); }

/* One element at the innermost level, by kind. */
static void jbuf_elem(JBuf *b, int64_t v, int64_t kind) {
    char num[40];
    switch (kind) {
    case TK_JKIND_STR: {
        const char *s = (const char *)(intptr_t)v;
        if (!s) { jbuf_puts(b, "null"); return; }
        /* json_enc returns an owned, quoted, escaped string for non-NULL. */
        const char *enc = json_enc(s);
        if (!enc) { jbuf_puts(b, "null"); return; }
        jbuf_puts(b, enc);
        free((void *)enc);
        return;
    }
    case TK_JKIND_F64:
        j_fmt_f64(num, sizeof num, i64_to_f64(v));
        jbuf_puts(b, num);
        return;
    case TK_JKIND_BOOL:
        jbuf_puts(b, v ? "true" : "false");
        return;
    default:
        snprintf(num, sizeof num, "%lld", (long long)v);
        jbuf_puts(b, num);
        return;
    }
}

static void jbuf_arr(JBuf *b, int64_t h, int64_t depth, int64_t kind) {
    if (!h) { jbuf_puts(b, "null"); return; }
    if (depth <= 1) {
        int64_t len = tk_arr_len(h);
        const int64_t *data = (const int64_t *)(intptr_t)h;
        jbuf_puts(b, "[");
        for (int64_t i = 0; i < len; i++) {
            if (i) jbuf_puts(b, ",");
            jbuf_elem(b, data[i], kind);
        }
        jbuf_puts(b, "]");
        return;
    }
    int64_t len = tk_arr_len(h);
    const int64_t *data = (const int64_t *)(intptr_t)h;
    jbuf_puts(b, "[");
    for (int64_t i = 0; i < len; i++) {
        if (i) jbuf_puts(b, ",");
        jbuf_arr(b, data[i], depth - 1, kind);
    }
    jbuf_puts(b, "]");
}

/* The array's JSON text in a heap buffer, or NULL if allocation failed. */
static char *json_arr_text(int64_t h, int64_t depth, int64_t kind) {
    JBuf b = { NULL, 0, 0 };
    jbuf_arr(&b, h, depth, kind);
    return b.p;
}

/* json.encarr(h, depth, kind) — JSON text of an array, as an owned string. */
int64_t tk_json_encarr_w(int64_t h, int64_t depth, int64_t kind) {
    JBuf b = { json_arr_text(h, depth, kind), 0, 0 };
    if (!b.p) {
        char *empty = (char *)malloc(3);
        if (!empty) return (int64_t)(intptr_t)"[]";
        strcpy(empty, "[]");
        return (int64_t)(intptr_t)empty;
    }
    return (int64_t)(intptr_t)b.p;
}

/* json.printarr(h, depth, kind) — the same text, to stdout, with a newline. */
void tk_json_printarr_w(int64_t h, int64_t depth, int64_t kind) {
    /* 135.3c: this used to call tk_json_encarr_w(), which returns the string
     * LITERAL "[]" when allocation fails, and then free() it -- a free() of a
     * global (clang unix.Malloc).  Take the text before that fallback. */
    char *text = json_arr_text(h, depth, kind);
    printf("%s\n", text ? text : "[]");
    free(text);
}
