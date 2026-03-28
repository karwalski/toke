/*
 * json.c — Implementation of std.json for the toke compiler runtime.
 *
 * Minimal recursive-descent key extractor for flat JSON objects.
 * No external JSON library; all parsing is hand-rolled.
 *
 * Story: 1.3.4  Branch: feature/stdlib-json
 */

#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static JsonErr make_err(JsonErrKind kind, const char *msg) {
    JsonErr e; e.kind = kind; e.msg = msg; return e;
}

/* Skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Skip over a JSON string starting at the opening quote.
 * Returns pointer to the character after the closing quote, or NULL. */
static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    p++;
    while (*p) {
        if (*p == '\\') { p += 2; continue; }
        if (*p == '"')  { return p + 1; }
        p++;
    }
    return NULL;
}

/* Skip over any JSON value.  Returns pointer past the value, or NULL. */
static const char *skip_value(const char *p);

static const char *skip_array(const char *p) {
    if (*p != '[') return NULL;
    p++;
    p = skip_ws(p);
    if (*p == ']') return p + 1;
    while (*p) {
        p = skip_value(skip_ws(p));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ']') return p + 1;
        if (*p == ',') { p++; continue; }
        return NULL;
    }
    return NULL;
}

static const char *skip_object(const char *p) {
    if (*p != '{') return NULL;
    p++;
    p = skip_ws(p);
    if (*p == '}') return p + 1;
    while (*p) {
        p = skip_string(skip_ws(p));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p = skip_value(skip_ws(p + 1));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        return NULL;
    }
    return NULL;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{') return skip_object(p);
    if (*p == '[') return skip_array(p);
    if (*p == 't') return (strncmp(p, "true",  4) == 0) ? p + 4 : NULL;
    if (*p == 'f') return (strncmp(p, "false", 5) == 0) ? p + 5 : NULL;
    if (*p == 'n') return (strncmp(p, "null",  4) == 0) ? p + 4 : NULL;
    /* number: digits, sign, dot, exponent */
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        const char *q = p;
        if (*q == '-') q++;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.') { q++; while (*q >= '0' && *q <= '9') q++; }
        if (*q == 'e' || *q == 'E') {
            q++;
            if (*q == '+' || *q == '-') q++;
            while (*q >= '0' && *q <= '9') q++;
        }
        return (q > p) ? q : NULL;
    }
    return NULL;
}

/*
 * find_json_key — linear scan of a flat JSON object for "key":value.
 * Sets *val_start to the first character of the value, *val_end past it.
 * Returns 1 on success, 0 if not found.
 */
static int find_json_key(const char *raw, const char *key,
                         const char **val_start, const char **val_end) {
    size_t klen = strlen(key);
    const char *p = skip_ws(raw);
    if (*p != '{') return 0;
    p++;
    while (1) {
        p = skip_ws(p);
        if (*p != '"') return 0;
        /* capture key string content */
        const char *ks = p + 1;
        const char *ke = skip_string(p);
        if (!ke) return 0;
        size_t kfound = (size_t)((ke - 1) - ks); /* length without quotes */
        p = skip_ws(ke);
        if (*p != ':') return 0;
        p = skip_ws(p + 1);
        const char *vs = p;
        const char *ve = skip_value(p);
        if (!ve) return 0;
        if (kfound == klen && strncmp(ks, key, klen) == 0) {
            *val_start = vs;
            *val_end   = ve;
            return 1;
        }
        p = skip_ws(ve);
        if (*p == '}') return 0; /* key not in object */
        if (*p == ',') { p++; continue; }
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* json_enc — Profile 1 stub: wrap Str in quotes with minimal escaping */
const char *json_enc(const char *v) {
    if (!v) return "\"\"";
    size_t n = strlen(v);
    /* worst case: every char escaped + 2 quotes + NUL */
    char *buf = malloc(2 * n + 3);
    if (!buf) return "\"\"";
    char *w = buf;
    *w++ = '"';
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '"')       { *w++ = '\\'; *w++ = '"'; }
        else if (v[i] == '\\') { *w++ = '\\'; *w++ = '\\'; }
        else                   { *w++ = v[i]; }
    }
    *w++ = '"';
    *w   = '\0';
    return buf;
}

/* json_dec — validate and store raw JSON string */
JsonResult json_dec(const char *s) {
    JsonResult r;
    const char *p = skip_ws(s);
    if (*p != '{' && *p != '[') {
        r.is_err = 1;
        r.err    = make_err(JSON_ERR_PARSE, "expected '{' or '['");
        return r;
    }
    const char *end = (*p == '{') ? skip_object(p) : skip_array(p);
    if (!end) {
        r.is_err = 1;
        r.err    = make_err(JSON_ERR_PARSE, "malformed JSON");
        return r;
    }
    r.is_err = 0;
    r.ok.raw = s;
    return r;
}

/* json_str — extract a string value for key */
StrJsonResult json_str(Json j, const char *key) {
    StrJsonResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    if (*vs != '"') {
        r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not a string"); return r;
    }
    /* copy string content (without surrounding quotes, basic unescape) */
    size_t cap = (size_t)(ve - vs);
    char *buf = malloc(cap);
    if (!buf) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "oom"); return r; }
    const char *p = vs + 1;
    char *w = buf;
    while (p < ve - 1) {
        if (*p == '\\') { p++; *w++ = *p++; }
        else            { *w++ = *p++; }
    }
    *w = '\0';
    r.is_err = 0; r.ok = buf; return r;
}

/* json_u64 */
U64JsonResult json_u64(Json j, const char *key) {
    U64JsonResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    uint64_t v = (uint64_t)strtoull(vs, &end, 10);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not a u64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* json_i64 */
I64JsonResult json_i64(Json j, const char *key) {
    I64JsonResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    int64_t v = (int64_t)strtoll(vs, &end, 10);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not an i64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* json_f64 */
F64JsonResult json_f64(Json j, const char *key) {
    F64JsonResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    double v = strtod(vs, &end);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not an f64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* json_bool */
BoolJsonResult json_bool(Json j, const char *key) {
    BoolJsonResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    if (strncmp(vs, "true",  4) == 0) { r.is_err = 0; r.ok = 1; return r; }
    if (strncmp(vs, "false", 5) == 0) { r.is_err = 0; r.ok = 0; return r; }
    r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not a bool"); return r;
}

/* json_arr — extract a JSON array as JsonArray of Json elements */
JsonArrayResult json_arr(Json j, const char *key) {
    JsonArrayResult r;
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(JSON_ERR_MISSING, key); return r;
    }
    if (*vs != '[') {
        r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "not an array"); return r;
    }
    /* first pass: count elements */
    uint64_t count = 0;
    const char *p = skip_ws(vs + 1);
    if (*p != ']') {
        while (1) {
            const char *elem_end = skip_value(skip_ws(p));
            if (!elem_end) {
                r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad array"); return r;
            }
            count++;
            p = skip_ws(elem_end);
            if (*p == ']') break;
            if (*p == ',') { p++; continue; }
            r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad array"); return r;
        }
    }
    Json *arr = malloc(count * sizeof(Json));
    if (!arr && count > 0) {
        r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "oom"); return r;
    }
    /* second pass: fill elements */
    p = skip_ws(vs + 1);
    for (uint64_t i = 0; i < count; i++) {
        const char *elem_s = skip_ws(p);
        const char *elem_e = skip_value(elem_s);
        /* copy element substring as raw */
        size_t len = (size_t)(elem_e - elem_s);
        char *buf = malloc(len + 1);
        memcpy(buf, elem_s, len);
        buf[len] = '\0';
        arr[i].raw = buf;
        p = skip_ws(elem_e);
        if (*p == ',') p++;
    }
    r.is_err = 0; r.ok.data = arr; r.ok.len = count; return r;
}
