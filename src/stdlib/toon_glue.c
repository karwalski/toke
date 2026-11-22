/* SPDX-License-Identifier: MIT */
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
#include "tk_array.h"
#include "capabilities.h"   /* 121.42 (5): toon.load is an fs.read sink */
static int64_t f64_to_i64(double d){int64_t i;memcpy(&i,&d,sizeof(i));return i;}

/*
 * Story 136.16 — the five typed accessors take (Toon; key).
 *
 * stdlib/toon.tki, docs/stdlib/toon.md and toon.h have always declared
 * toon.str / toon.i64 / toon.f64 / toon.bool / toon.arr as (Toon, str), and
 * toon.c implements all five that way.  The wrappers below took only the
 * handle and never called them: str returned t->raw whole, i64 ran strtoll
 * over the entire document, f64 strtod, bool strcmp'd the document against
 * "true", and arr took NO arguments at all and returned a freshly calloc'd
 * empty ToonArray — structurally incapable of returning data.  A caller who
 * asked for one field got the whole document or nothing, silently.
 *
 * Errors are reported the way 114.53/114.54 and 127.67 established: set
 * tk_current_error and still return the real value, because 0 and false are
 * legitimate results that the 0 sentinel cannot distinguish from failure.
 */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

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

/* 121.42 (5): reads the file named by the program -- fs.read, like file.read. */
int64_t tk_toon_load_w(int64_t path) {
    TK_REQUIRE_PATH(TK_CAP_FS_READ, (const char *)(intptr_t)path);   /* 138.19 */
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

/* toon.dec(s) -> Toon!ToonErr.  127.241: a failure stores a {tag, msg} box
 * of ToonErr's sum layout — the tag is ToonErrKind itself ($parse 0, $type 1,
 * $missing 2; toon.h) — and a success clears the slot, so the $err arm binds
 * the variant; until 127.241 it returned a bare 0 and bound nil.  toon.c's
 * messages are string literals, so the box never dangles.  (tk_err_box is
 * declared by hand: this file spells the slot itself rather than including
 * tk_runtime.h, see above.) */
void *tk_err_box(int64_t nbytes);
static int64_t toon_raise(ToonErrKind kind, const char *msg) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)kind;
    box[1] = (int64_t)(intptr_t)(msg && *msg ? msg : "toon: parse error");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* 127.246 (2): the typed accessors raise a real box too.  Until 2026-09-30
 * toon.str/i64/f64/bool/arr stored the payload-less flag 1, so `$err:e` bound
 * nil and a match on $parse/$type/$missing could not be written usefully.
 * The message names the call and the key, and is copied through tk_err_msg's
 * per-thread ring (tk_runtime.c) because the key is the caller's string. */
const char *tk_err_msg(const char *fmt, ...);
static int64_t toon_raise_at(const char *op, ToonErr e, int64_t key) {
    const char *why = e.kind == TOON_ERR_MISSING ? "missing key"
                    : (e.msg && *e.msg ? e.msg : "parse error");
    return toon_raise(e.kind, tk_err_msg("%s: %s: %s", op, why,
                          key ? (const char *)(intptr_t)key : ""));
}
static int64_t toon_raise_null(const char *op) {
    return toon_raise(TOON_ERR_PARSE, tk_err_msg("%s: null toon or key", op));
}

int64_t tk_toon_dec_w(int64_t s) {
    if (!s) return toon_raise(TOON_ERR_PARSE, "toon.dec: null input");
    ToonResult r = toon_dec((const char *)(intptr_t)s);
    if (r.is_err) return toon_raise(r.err.kind, r.err.msg);
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return toon_raise(TOON_ERR_PARSE, "toon.dec: out of memory");
    *heap = r.ok;
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return (int64_t)(intptr_t)heap;
}

/* 121.43 (1): toon.tojson(t:$str):$str -- stdlib/toon.tki, docs/stdlib/toon.md
 * and both documented examples pass TOON *text*, the string toon.fromjson
 * returns.  This wrapper read its argument as a `Toon *` handle and loaded
 * t->raw from the first eight bytes of the string, so the documented call
 * `toon.tojson("data[2]{id,name}:...")` segfaulted.  It takes the string, as
 * declared -- the same shape as its twin tk_toon_fromjson_w below. */
int64_t tk_toon_tojson_w(int64_t v) {
    if (!v) return 0;
    const char *json = toon_to_json((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)json;
}

int64_t tk_toon_i64_w(int64_t v, int64_t key) {
    if (!v || !key) return toon_raise_null("toon.i64");
    Toon *t = (Toon *)(intptr_t)v;
    I64ToonResult r = toon_i64(*t, (const char *)(intptr_t)key);
    if (r.is_err) return toon_raise_at("toon.i64", r.err, key);
    tk_current_error = 0;
    return r.ok;
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

int64_t tk_toon_bool_w(int64_t v, int64_t key) {
    if (!v || !key) return toon_raise_null("toon.bool");
    Toon *t = (Toon *)(intptr_t)v;
    BoolToonResult r = toon_bool(*t, (const char *)(intptr_t)key);
    if (r.is_err) return toon_raise_at("toon.bool", r.err, key);
    tk_current_error = 0;
    return (int64_t)r.ok;
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

/*
 * toon.arr(t; key) : @($toon)!$toonerr — every row's value for one field.
 *
 * Returns a real toke array (tk_array.h layout) whose elements are heap Toon
 * handles of the same shape tk_toon_dec_w hands out, so each element can be
 * fed straight back into toon.str/i64/f64/bool.  The old wrapper took no
 * arguments and returned a bare `ToonArray *` that toke would have read as an
 * array handle — its [-1] length word would have been whatever preceded the
 * calloc block.
 */
int64_t tk_toon_arr_w(int64_t v, int64_t key) {
    if (!v || !key) return toon_raise_null("toon.arr");
    Toon *t = (Toon *)(intptr_t)v;
    ToonArrayResult r = toon_arr(*t, (const char *)(intptr_t)key);
    if (r.is_err) return toon_raise_at("toon.arr", r.err, key);

    int64_t n = (int64_t)r.ok.len;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) {
        free(r.ok.data);
        return toon_raise(TOON_ERR_PARSE, "toon.arr: out of memory");
    }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        Toon *elem = (Toon *)malloc(sizeof(Toon));
        if (!elem) {
            for (int64_t j = 0; j < i; j++) free((void *)(intptr_t)slots[j]);
            free(slots - TK_ARR_HDR);   /* the block tk_arr_alloc returned */
            free(r.ok.data);
            return toon_raise(TOON_ERR_PARSE, "toon.arr: out of memory");
        }
        *elem = r.ok.data[i];
        slots[i] = (int64_t)(intptr_t)elem;
    }
    free(r.ok.data);
    tk_current_error = 0;
    return h;
}

int64_t tk_toon_f64_w(int64_t v, int64_t key) {
    if (!v || !key) { toon_raise_null("toon.f64"); return f64_to_i64(0.0); }
    Toon *t = (Toon *)(intptr_t)v;
    F64ToonResult r = toon_f64(*t, (const char *)(intptr_t)key);
    if (r.is_err) { toon_raise_at("toon.f64", r.err, key); return f64_to_i64(0.0); }
    tk_current_error = 0;
    return f64_to_i64(r.ok);
}

int64_t tk_toon_str_w(int64_t v, int64_t key) {
    if (!v || !key) return toon_raise_null("toon.str");
    Toon *t = (Toon *)(intptr_t)v;
    StrToonResult r = toon_str(*t, (const char *)(intptr_t)key);
    if (r.is_err) return toon_raise_at("toon.str", r.err, key);
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
}

/*
 * Story 136.32 — toon.fromjson and toon.empty.
 *
 * toon.fromjson has been in stdlib/toon.tki and documented on
 * docs/stdlib/toon.md since the module shipped, and toon_from_json() has been
 * in toon.c beside toon_to_json() all along; only the wrapper was missing, so
 * two documented examples could not be built. tk_toon_tojson_w was here; its
 * twin never was.
 *
 * toon.empty() is the other half of the same omission. Three documented
 * examples write `mt toon.dec(...) {$ok:v v; $err:e toon.empty()}` because the
 * $err arm has to produce a $toon and toon.dec — the call that just failed —
 * was the module's only constructor. Declared in stdlib/toon.tki alongside
 * this wrapper. std.yaml carries the identical pair.
 */
int64_t tk_toon_fromjson_w(int64_t json) {
    if (!json) return 0;
    const char *s = toon_from_json((const char *)(intptr_t)json);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_empty_w(void) {
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    heap->raw = "";
    return (int64_t)(intptr_t)heap;
}
