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

int64_t tk_toon_i64_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return 0; }
    Toon *t = (Toon *)(intptr_t)v;
    I64ToonResult r = toon_i64(*t, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : r.ok;
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
    if (!v || !key) { tk_current_error = 1; return 0; }
    Toon *t = (Toon *)(intptr_t)v;
    BoolToonResult r = toon_bool(*t, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : (int64_t)r.ok;
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
    if (!v || !key) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    Toon *t = (Toon *)(intptr_t)v;
    ToonArrayResult r = toon_arr(*t, (const char *)(intptr_t)key);
    if (r.is_err) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    tk_current_error = 0;

    int64_t n = (int64_t)r.ok.len;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        Toon *elem = (Toon *)malloc(sizeof(Toon));
        if (!elem) { tk_arr_setlen(h, i); tk_current_error = 1; return h; }
        *elem = r.ok.data[i];
        slots[i] = (int64_t)(intptr_t)elem;
    }
    free(r.ok.data);
    return h;
}

int64_t tk_toon_f64_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return f64_to_i64(0.0); }
    Toon *t = (Toon *)(intptr_t)v;
    F64ToonResult r = toon_f64(*t, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return f64_to_i64(r.is_err ? 0.0 : r.ok);
}

int64_t tk_toon_str_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return 0; }
    Toon *t = (Toon *)(intptr_t)v;
    StrToonResult r = toon_str(*t, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
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
