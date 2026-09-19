/*
 * yaml_glue.c — i64-ABI wrappers for std.yaml (Story 136.32).
 *
 * std.yaml shipped a complete C parser/emitter (yaml.c, 602 lines), a
 * normative interface (stdlib/yaml.tki) and a full reference page
 * (docs/stdlib/yaml.md) — and not one of the nine documented entry points had
 * a wrapper, so every one of the page's ten examples died at link. The module
 * was documented end to end and unreachable from toke.
 *
 * The four wrappers that DID exist (parse/stringify/load/print — none of them
 * documented) lived in tk_web_glue.c, which src/stdlib_deps.c registers under
 * the HTTP module, so even those only linked when a program happened to import
 * std.http. They are moved here verbatim, the same extraction 114.35 did for
 * toon_glue.c, 114.31 for net_glue.c and 136.25 for ml_glue.c.
 *
 * This file is the exact analogue of toon_glue.c: yaml.h and toon.h declare
 * the same seven-function shape over the same result-struct family, so the
 * marshalling below matches toon's line for line, including the error
 * convention 114.53/127.67 established — set tk_current_error and still
 * return the real value, because 0 and false are legitimate results that the
 * 0 sentinel cannot distinguish from failure.
 */
#include "yaml.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "file.h"
#include "str.h"
#include "tk_array.h"

static int64_t f64_to_i64(double d) { int64_t i; memcpy(&i, &d, sizeof(i)); return i; }

extern int64_t tk_current_error;

/* Yaml handles are stored as heap-allocated Yaml structs cast to i64. */

static int64_t yaml_heap(Yaml y) {
    Yaml *heap = (Yaml *)malloc(sizeof(Yaml));
    if (!heap) return 0;
    *heap = y;
    return (int64_t)(intptr_t)heap;
}

/* ── documented surface (stdlib/yaml.tki, docs/stdlib/yaml.md) ─────── */

int64_t tk_yaml_enc_w(int64_t v) {
    if (!v) return 0;
    const char *s = yaml_enc((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

int64_t tk_yaml_dec_w(int64_t s) {
    if (!s) { tk_current_error = 1; return 0; }
    YamlResult r = yaml_dec((const char *)(intptr_t)s);
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return 0;
    return yaml_heap(r.ok);
}

/*
 * yaml.empty() : $yaml — the empty document.
 *
 * Every `mt yaml.dec(...)` in the documentation needs a $yaml for its $err
 * arm, and nothing else in the module can produce one: yaml.dec is the only
 * constructor and it is precisely the call that failed. The page has always
 * been written against `yaml.empty()`; it was simply never declared. Added to
 * stdlib/yaml.tki alongside this wrapper (and to std.toon, which carries the
 * same idiom on three of its pages).
 */
int64_t tk_yaml_empty_w(void) {
    Yaml y; y.raw = "";
    return yaml_heap(y);
}

int64_t tk_yaml_str_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return 0; }
    Yaml *y = (Yaml *)(intptr_t)v;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_yaml_i64_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return 0; }
    Yaml *y = (Yaml *)(intptr_t)v;
    I64YamlResult r = yaml_i64(*y, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : r.ok;
}

int64_t tk_yaml_f64_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return f64_to_i64(0.0); }
    Yaml *y = (Yaml *)(intptr_t)v;
    F64YamlResult r = yaml_f64(*y, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return f64_to_i64(r.is_err ? 0.0 : r.ok);
}

int64_t tk_yaml_bool_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return 0; }
    Yaml *y = (Yaml *)(intptr_t)v;
    BoolYamlResult r = yaml_bool(*y, (const char *)(intptr_t)key);
    tk_current_error = r.is_err ? 1 : 0;
    return r.is_err ? 0 : (int64_t)r.ok;
}

/*
 * yaml.arr(y; key) : @($yaml)!$yamlerr.
 *
 * Returns a real toke array (tk_array.h layout) whose elements are heap Yaml
 * handles of the same shape tk_yaml_dec_w hands out, so each element can be
 * fed straight back into yaml.str/i64/f64/bool. Mirrors tk_toon_arr_w.
 */
int64_t tk_yaml_arr_w(int64_t v, int64_t key) {
    if (!v || !key) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    Yaml *y = (Yaml *)(intptr_t)v;
    YamlArrayResult r = yaml_arr(*y, (const char *)(intptr_t)key);
    if (r.is_err) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    tk_current_error = 0;

    int64_t n = (int64_t)r.ok.len;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) { tk_current_error = 1; return tk_arr_alloc(0, 0); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        int64_t elem = yaml_heap(r.ok.data[i]);
        if (!elem) { tk_arr_setlen(h, i); tk_current_error = 1; return h; }
        slots[i] = elem;
    }
    free(r.ok.data);
    return h;
}

int64_t tk_yaml_fromjson_w(int64_t json) {
    if (!json) return 0;
    const char *s = yaml_from_json((const char *)(intptr_t)json);
    return (int64_t)(intptr_t)s;
}

int64_t tk_yaml_tojson_w(int64_t v) {
    if (!v) return 0;
    const char *s = yaml_to_json((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

/* ── undocumented wrappers moved verbatim from tk_web_glue.c ───────── */

int64_t tk_yaml_parse_w(int64_t s) {
    if (!s) return 0;
    YamlResult r = yaml_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    return yaml_heap(r.ok);
}

int64_t tk_yaml_stringify_w(int64_t val) {
    if (!val) return 0;
    const char *s = yaml_enc((const char *)(intptr_t)val);
    return (int64_t)(intptr_t)s;
}

int64_t tk_yaml_load_w(int64_t path) {
    if (!path) return 0;
    StrFileResult fr = file_read((const char *)(intptr_t)path);
    if (fr.is_err || !fr.ok) return 0;
    YamlResult r = yaml_dec(fr.ok);
    if (r.is_err) return 0;
    return yaml_heap(r.ok);
}

int64_t tk_yaml_print_w(int64_t v) {
    if (!v) return 0;
    const char *s = yaml_enc((const char *)(intptr_t)v);
    if (s) printf("%s\n", s);
    return 0;
}

int64_t tk_yaml_getnestedmap_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    if (r.is_err || !r.ok) return 0;
    /* Treat the string value as raw YAML for a nested map */
    YamlResult nested = yaml_dec(r.ok);
    if (nested.is_err) return 0;
    return yaml_heap(nested.ok);
}

int64_t tk_yaml_getrootmap_w(int64_t doc) {
    return doc;
}

int64_t tk_yaml_getstring_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_yaml_maphaskey_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    return r.is_err ? 0 : 1;
}

int64_t tk_yaml_splitstr_w(int64_t s, int64_t delim) {
    if (!s) return 0;
    const char *str = (const char *)(intptr_t)s;
    const char *d = delim ? (const char *)(intptr_t)delim : ",";
    StrArray parts = str_split(str, d);
    StrArray *heap = (StrArray *)malloc(sizeof(StrArray));
    if (!heap) return 0;
    *heap = parts;
    return (int64_t)(intptr_t)heap;
}
