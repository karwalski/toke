/*
 * json.c — Implementation of std.json for the toke compiler runtime.
 *
 * Minimal recursive-descent key extractor for flat JSON objects.
 * No external JSON library; all parsing is hand-rolled.
 *
 * Story: 1.3.4, 35.1.3  Branch: feature/stdlib-json
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

/* ------------------------------------------------------------------ */
/* Object inspection and manipulation — Story 29.1.1                  */
/* ------------------------------------------------------------------ */

/*
 * json_keys — return heap-allocated StrArray of top-level key names.
 * Iterates the object once to count, then again to fill.
 */
StrArrayJsonResult json_keys(Json j) {
    StrArrayJsonResult r;
    const char *p = skip_ws(j.raw);
    if (!p || *p != '{') {
        r.is_err = 1;
        r.err = make_err(JSON_ERR_TYPE, "not an object");
        return r;
    }
    p++;

    /* first pass: count keys */
    uint64_t count = 0;
    const char *q = skip_ws(p);
    if (*q != '}') {
        while (*q) {
            q = skip_ws(q);
            if (*q != '"') { r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "bad object"); return r; }
            const char *ke = skip_string(q);
            if (!ke) { r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "bad key"); return r; }
            q = skip_ws(ke);
            if (*q != ':') { r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "bad object"); return r; }
            q = skip_value(skip_ws(q + 1));
            if (!q) { r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "bad value"); return r; }
            count++;
            q = skip_ws(q);
            if (*q == '}') break;
            if (*q == ',') { q++; continue; }
            r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "bad object"); return r;
        }
    }

    /* allocate array of pointers */
    const char **keys = NULL;
    if (count > 0) {
        keys = malloc(count * sizeof(const char *));
        if (!keys) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "oom"); return r; }
    }

    /* second pass: extract key strings */
    q = skip_ws(p);
    for (uint64_t i = 0; i < count; i++) {
        q = skip_ws(q);
        /* q points at opening quote of key */
        const char *ks = q + 1;
        const char *ke = skip_string(q);
        /* ke points past closing quote */
        size_t klen = (size_t)((ke - 1) - ks);
        char *buf = malloc(klen + 1);
        if (!buf) {
            /* free already-allocated keys */
            for (uint64_t x = 0; x < i; x++) free((void *)keys[x]);
            free(keys);
            r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "oom"); return r;
        }
        /* basic unescape */
        const char *src = ks;
        char *dst = buf;
        while (src < ke - 1) {
            if (*src == '\\') { src++; *dst++ = *src++; }
            else               { *dst++ = *src++; }
        }
        *dst = '\0';
        keys[i] = buf;
        /* advance past value and comma */
        ke = skip_ws(ke); /* skip past ':' */
        if (*ke != ':') break;
        q = skip_value(skip_ws(ke + 1));
        q = skip_ws(q);
        if (*q == ',') q++;
    }

    r.is_err = 0;
    r.ok.data = keys;
    r.ok.len  = count;
    return r;
}

/*
 * json_has — return 1 if key exists in top-level object, 0 otherwise.
 * Scans for "key": without performing full value extraction.
 */
int json_has(Json j, const char *key) {
    const char *vs, *ve;
    return find_json_key(j.raw, key, &vs, &ve);
}

/*
 * json_len — count elements in array or keys in object.
 */
U64JsonResult json_len(Json j) {
    U64JsonResult r;
    const char *p = skip_ws(j.raw);
    if (!p) { r.is_err = 1; r.err = make_err(JSON_ERR_TYPE, "null json"); return r; }

    if (*p == '[') {
        /* array: count top-level elements */
        p++;
        p = skip_ws(p);
        if (*p == ']') { r.is_err = 0; r.ok = 0; return r; }
        uint64_t count = 0;
        while (*p) {
            const char *end = skip_value(skip_ws(p));
            if (!end) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad array"); return r; }
            count++;
            p = skip_ws(end);
            if (*p == ']') { r.is_err = 0; r.ok = count; return r; }
            if (*p == ',') { p++; continue; }
            r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad array"); return r;
        }
        r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "unterminated array"); return r;
    }

    if (*p == '{') {
        /* object: count top-level keys */
        p++;
        p = skip_ws(p);
        if (*p == '}') { r.is_err = 0; r.ok = 0; return r; }
        uint64_t count = 0;
        while (*p) {
            p = skip_ws(p);
            if (*p != '"') { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad object"); return r; }
            const char *ke = skip_string(p);
            if (!ke) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad key"); return r; }
            p = skip_ws(ke);
            if (*p != ':') { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad object"); return r; }
            p = skip_value(skip_ws(p + 1));
            if (!p) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad value"); return r; }
            count++;
            p = skip_ws(p);
            if (*p == '}') { r.is_err = 0; r.ok = count; return r; }
            if (*p == ',') { p++; continue; }
            r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "bad object"); return r;
        }
        r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "unterminated object"); return r;
    }

    r.is_err = 1;
    r.err = make_err(JSON_ERR_TYPE, "not an array or object");
    return r;
}

/*
 * json_type — return a string literal identifying j's type.
 */
StrJsonResult json_type(Json j) {
    StrJsonResult r;
    const char *p = skip_ws(j.raw);
    if (!p || *p == '\0') {
        r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "empty json"); return r;
    }
    r.is_err = 0;
    if (*p == '{')                         { r.ok = "object"; return r; }
    if (*p == '[')                         { r.ok = "array";  return r; }
    if (*p == '"')                         { r.ok = "string"; return r; }
    if (*p == 't' || *p == 'f')            { r.ok = "bool";   return r; }
    if (*p == 'n')                         { r.ok = "null";   return r; }
    if (*p == '-' || (*p >= '0' && *p <= '9')) { r.ok = "number"; return r; }
    r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "unrecognised json"); return r;
}

/*
 * json_pretty — produce a malloc'd pretty-printed (2-space indent) string.
 * Handles nested objects and arrays; copies strings verbatim.
 */
StrJsonResult json_pretty(Json j) {
    StrJsonResult r;
    if (!j.raw) {
        r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "null json"); return r;
    }
    const char *src = j.raw;
    size_t slen = strlen(src);

    /* worst-case output is substantially larger than input due to indentation;
     * allocate generously: 8 * input length + some headroom */
    size_t cap = slen * 8 + 64;
    char *buf = malloc(cap);
    if (!buf) { r.is_err = 1; r.err = make_err(JSON_ERR_PARSE, "oom"); return r; }

    size_t pos = 0;
    int    depth = 0;
    int    in_str = 0;

#define PRETTY_PUT(c) do { if (pos + 1 < cap) buf[pos++] = (char)(c); } while(0)
#define PRETTY_NEWLINE_INDENT() do { \
    PRETTY_PUT('\n'); \
    for (int _i = 0; _i < depth; _i++) { PRETTY_PUT(' '); PRETTY_PUT(' '); } \
} while(0)

    for (size_t i = 0; i < slen; i++) {
        char c = src[i];

        if (in_str) {
            PRETTY_PUT(c);
            if (c == '\\' && i + 1 < slen) {
                /* escaped char — copy the next byte verbatim */
                i++;
                PRETTY_PUT(src[i]);
            } else if (c == '"') {
                in_str = 0;
            }
            continue;
        }

        /* skip whitespace outside strings */
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;

        switch (c) {
        case '{':
        case '[':
            PRETTY_PUT(c);
            /* peek ahead to see if the container is empty */
            {
                size_t peek = i + 1;
                while (peek < slen &&
                       (src[peek] == ' ' || src[peek] == '\t' ||
                        src[peek] == '\n' || src[peek] == '\r'))
                    peek++;
                char close = (c == '{') ? '}' : ']';
                if (peek < slen && src[peek] == close) {
                    /* empty container — don't indent */
                } else {
                    depth++;
                    PRETTY_NEWLINE_INDENT();
                }
            }
            break;

        case '}':
        case ']':
            /* check if we just opened (depth decreased already via empty check above) */
            {
                /* walk back in output to see if last non-ws char was [ or { */
                int prev_open = 0;
                if (pos > 0) {
                    size_t bp = pos;
                    while (bp > 0) {
                        char prev = buf[bp - 1];
                        if (prev == ' ' || prev == '\n') { bp--; continue; }
                        if (prev == '{' || prev == '[') prev_open = 1;
                        break;
                    }
                }
                if (!prev_open) {
                    depth--;
                    PRETTY_NEWLINE_INDENT();
                }
            }
            PRETTY_PUT(c);
            break;

        case ',':
            PRETTY_PUT(c);
            PRETTY_NEWLINE_INDENT();
            break;

        case ':':
            PRETTY_PUT(c);
            PRETTY_PUT(' ');
            break;

        case '"':
            in_str = 1;
            PRETTY_PUT(c);
            break;

        default:
            PRETTY_PUT(c);
            break;
        }
    }

#undef PRETTY_PUT
#undef PRETTY_NEWLINE_INDENT

    buf[pos] = '\0';
    r.is_err = 0;
    r.ok = buf;
    return r;
}

/*
 * json_is_null — return 1 if j[key] is null or the key is missing.
 */
int json_is_null(Json j, const char *key) {
    const char *vs, *ve;
    if (!find_json_key(j.raw, key, &vs, &ve)) return 1; /* missing */
    return (strncmp(vs, "null", 4) == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Streaming API — Story 35.1.3                                        */
/* ------------------------------------------------------------------ */

/* --- helpers for the streaming parser --- */

static JsonStreamErr stream_err(JsonStreamErrKind kind, const char *msg) {
    JsonStreamErr e;
    e.kind = kind;
    e.msg  = msg;
    return e;
}

static JsonTokenResult tok_ok(JsonToken t) {
    JsonTokenResult r;
    r.is_err = 0;
    r.ok = t;
    return r;
}

static JsonTokenResult tok_err(JsonStreamErrKind kind, const char *msg) {
    JsonTokenResult r;
    r.is_err = 1;
    r.err = stream_err(kind, msg);
    return r;
}

static JsonToken tok_simple(JsonTokenKind kind) {
    JsonToken t;
    t.kind = kind;
    t.val.u64 = 0;
    return t;
}

static int stream_eof(const JsonStream *s) {
    return s->pos >= s->len;
}

static uint8_t stream_peek(const JsonStream *s) {
    return s->buf[s->pos];
}

static void stream_skip_ws(JsonStream *s) {
    while (s->pos < s->len) {
        uint8_t c = s->buf[s->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            s->pos++;
        else
            break;
    }
}

/*
 * Parse a JSON string starting at s->pos (which must be '"').
 * Returns a malloc'd C-string with escapes resolved, or NULL on error.
 * Advances s->pos past the closing quote.
 */
static char *stream_parse_string(JsonStream *s) {
    if (stream_eof(s) || stream_peek(s) != '"') return NULL;
    s->pos++; /* skip opening quote */

    /* first pass: measure length */
    uint64_t start = s->pos;
    uint64_t slen = 0;
    while (s->pos < s->len) {
        uint8_t c = s->buf[s->pos];
        if (c == '\\') {
            s->pos += 2;
            slen++;
        } else if (c == '"') {
            break;
        } else {
            s->pos++;
            slen++;
        }
    }
    if (s->pos >= s->len) return NULL; /* unterminated */

    /* allocate and second pass: copy with basic unescape */
    char *buf = malloc(slen + 1);
    if (!buf) return NULL;
    uint64_t rp = start;
    char *wp = buf;
    while (rp < s->pos) {
        uint8_t c = s->buf[rp];
        if (c == '\\') {
            rp++;
            uint8_t esc = s->buf[rp];
            switch (esc) {
                case '"':  *wp++ = '"';  break;
                case '\\': *wp++ = '\\'; break;
                case '/':  *wp++ = '/';  break;
                case 'b':  *wp++ = '\b'; break;
                case 'f':  *wp++ = '\f'; break;
                case 'n':  *wp++ = '\n'; break;
                case 'r':  *wp++ = '\r'; break;
                case 't':  *wp++ = '\t'; break;
                default:   *wp++ = (char)esc; break;
            }
            rp++;
        } else {
            *wp++ = (char)c;
            rp++;
        }
    }
    *wp = '\0';
    s->pos++; /* skip closing quote */
    return buf;
}

/*
 * Parse a JSON number starting at s->pos.
 * Determines whether it is integer (u64/i64) or float (f64).
 * Advances s->pos past the number.
 */
static JsonTokenResult stream_parse_number(JsonStream *s) {
    uint64_t start = s->pos;
    int is_neg = 0;
    int is_float = 0;

    if (s->buf[s->pos] == '-') { is_neg = 1; s->pos++; }
    if (s->pos >= s->len || s->buf[s->pos] < '0' || s->buf[s->pos] > '9')
        return tok_err(JSON_STREAM_ERR_INVALID, "bad number");

    while (s->pos < s->len && s->buf[s->pos] >= '0' && s->buf[s->pos] <= '9')
        s->pos++;

    if (s->pos < s->len && s->buf[s->pos] == '.') {
        is_float = 1;
        s->pos++;
        while (s->pos < s->len && s->buf[s->pos] >= '0' && s->buf[s->pos] <= '9')
            s->pos++;
    }
    if (s->pos < s->len && (s->buf[s->pos] == 'e' || s->buf[s->pos] == 'E')) {
        is_float = 1;
        s->pos++;
        if (s->pos < s->len && (s->buf[s->pos] == '+' || s->buf[s->pos] == '-'))
            s->pos++;
        while (s->pos < s->len && s->buf[s->pos] >= '0' && s->buf[s->pos] <= '9')
            s->pos++;
    }

    /* NUL-terminated copy for strto* */
    size_t nlen = (size_t)(s->pos - start);
    char tmp[64];
    if (nlen >= sizeof(tmp))
        return tok_err(JSON_STREAM_ERR_OVERFLOW, "number too long");
    memcpy(tmp, s->buf + start, nlen);
    tmp[nlen] = '\0';

    JsonToken t;
    if (is_float) {
        char *end;
        t.kind = JSON_TOK_F64;
        t.val.f64 = strtod(tmp, &end);
    } else if (is_neg) {
        char *end;
        t.kind = JSON_TOK_I64;
        t.val.i64 = (int64_t)strtoll(tmp, &end, 10);
    } else {
        char *end;
        t.kind = JSON_TOK_U64;
        t.val.u64 = (uint64_t)strtoull(tmp, &end, 10);
    }
    return tok_ok(t);
}

/* --- public streaming parser --- */

JsonStream json_streamparser(const uint8_t *buf, uint64_t len) {
    JsonStream s;
    s.buf   = buf;
    s.len   = len;
    s.pos   = 0;
    s.depth = 0;
    s.stack[0] = JSON_STATE_VALUE;
    return s;
}

JsonTokenResult json_streamnext(JsonStream *s) {
    stream_skip_ws(s);

    if (s->depth == 0 && s->stack[0] == JSON_STATE_DONE) {
        return tok_ok(tok_simple(JSON_TOK_END));
    }

    if (stream_eof(s))
        return tok_err(JSON_STREAM_ERR_TRUNCATED, "unexpected end of input");

    JsonStreamState state = s->stack[s->depth];
    uint8_t c = stream_peek(s);

    switch (state) {
    case JSON_STATE_VALUE:
    case JSON_STATE_OBJ_VALUE:
    case JSON_STATE_ARR_VALUE_OR_END:
    {
        /* In array context, also accept ']' */
        if (state == JSON_STATE_ARR_VALUE_OR_END && c == ']') {
            s->pos++;
            if (s->depth == 0)
                return tok_err(JSON_STREAM_ERR_INVALID, "unexpected ']'");
            s->depth--;
            /* After closing array, container decides next state */
            if (s->depth == 0) {
                s->stack[0] = JSON_STATE_DONE;
            } else {
                JsonStreamState parent = s->stack[s->depth];
                if (parent == JSON_STATE_OBJ_VALUE)
                    s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
                else if (parent == JSON_STATE_ARR_VALUE_OR_END)
                    s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            }
            return tok_ok(tok_simple(JSON_TOK_ARRAY_END));
        }

        /* Parse value */
        if (c == '{') {
            s->pos++;
            if (s->depth > 0) {
                /* transition the current frame to post-value */
                if (state == JSON_STATE_OBJ_VALUE)
                    s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
                else if (state == JSON_STATE_ARR_VALUE_OR_END)
                    s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            }
            s->depth++;
            if (s->depth >= JSON_STREAM_MAX_DEPTH)
                return tok_err(JSON_STREAM_ERR_OVERFLOW, "nesting too deep");
            s->stack[s->depth] = JSON_STATE_OBJ_KEY_OR_END;
            return tok_ok(tok_simple(JSON_TOK_OBJECT_START));
        }
        if (c == '[') {
            s->pos++;
            if (s->depth > 0) {
                if (state == JSON_STATE_OBJ_VALUE)
                    s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
                else if (state == JSON_STATE_ARR_VALUE_OR_END)
                    s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            }
            s->depth++;
            if (s->depth >= JSON_STREAM_MAX_DEPTH)
                return tok_err(JSON_STREAM_ERR_OVERFLOW, "nesting too deep");
            s->stack[s->depth] = JSON_STATE_ARR_VALUE_OR_END;
            return tok_ok(tok_simple(JSON_TOK_ARRAY_START));
        }
        if (c == '"') {
            char *str = stream_parse_string(s);
            if (!str)
                return tok_err(JSON_STREAM_ERR_INVALID, "bad string");
            JsonToken t;
            t.kind = JSON_TOK_STR;
            t.val.str = str;
            /* transition state after scalar */
            if (s->depth == 0) {
                s->stack[0] = JSON_STATE_DONE;
            } else {
                if (state == JSON_STATE_OBJ_VALUE)
                    s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
                else if (state == JSON_STATE_ARR_VALUE_OR_END)
                    s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            }
            return tok_ok(t);
        }
        if (c == 't') {
            if (s->pos + 4 > s->len || memcmp(s->buf + s->pos, "true", 4) != 0)
                return tok_err(JSON_STREAM_ERR_INVALID, "bad token");
            s->pos += 4;
            JsonToken t;
            t.kind = JSON_TOK_BOOL;
            t.val.b = 1;
            if (s->depth == 0) s->stack[0] = JSON_STATE_DONE;
            else if (state == JSON_STATE_OBJ_VALUE)
                s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
            else if (state == JSON_STATE_ARR_VALUE_OR_END)
                s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            return tok_ok(t);
        }
        if (c == 'f') {
            if (s->pos + 5 > s->len || memcmp(s->buf + s->pos, "false", 5) != 0)
                return tok_err(JSON_STREAM_ERR_INVALID, "bad token");
            s->pos += 5;
            JsonToken t;
            t.kind = JSON_TOK_BOOL;
            t.val.b = 0;
            if (s->depth == 0) s->stack[0] = JSON_STATE_DONE;
            else if (state == JSON_STATE_OBJ_VALUE)
                s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
            else if (state == JSON_STATE_ARR_VALUE_OR_END)
                s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            return tok_ok(t);
        }
        if (c == 'n') {
            if (s->pos + 4 > s->len || memcmp(s->buf + s->pos, "null", 4) != 0)
                return tok_err(JSON_STREAM_ERR_INVALID, "bad token");
            s->pos += 4;
            if (s->depth == 0) s->stack[0] = JSON_STATE_DONE;
            else if (state == JSON_STATE_OBJ_VALUE)
                s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
            else if (state == JSON_STATE_ARR_VALUE_OR_END)
                s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            return tok_ok(tok_simple(JSON_TOK_NULL));
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            JsonTokenResult nr = stream_parse_number(s);
            if (!nr.is_err) {
                if (s->depth == 0) s->stack[0] = JSON_STATE_DONE;
                else if (state == JSON_STATE_OBJ_VALUE)
                    s->stack[s->depth] = JSON_STATE_OBJ_COMMA;
                else if (state == JSON_STATE_ARR_VALUE_OR_END)
                    s->stack[s->depth] = JSON_STATE_ARR_COMMA;
            }
            return nr;
        }
        return tok_err(JSON_STREAM_ERR_INVALID, "unexpected character");
    }

    case JSON_STATE_OBJ_KEY_OR_END:
        if (c == '}') {
            s->pos++;
            s->depth--;
            if (s->depth == 0) {
                s->stack[0] = JSON_STATE_DONE;
            }
            return tok_ok(tok_simple(JSON_TOK_OBJECT_END));
        }
        if (c == '"') {
            char *str = stream_parse_string(s);
            if (!str)
                return tok_err(JSON_STREAM_ERR_INVALID, "bad key");
            s->stack[s->depth] = JSON_STATE_OBJ_COLON;
            JsonToken t;
            t.kind = JSON_TOK_KEY;
            t.val.str = str;
            return tok_ok(t);
        }
        return tok_err(JSON_STREAM_ERR_INVALID, "expected key or '}'");

    case JSON_STATE_OBJ_COLON:
        if (c != ':')
            return tok_err(JSON_STREAM_ERR_INVALID, "expected ':'");
        s->pos++;
        s->stack[s->depth] = JSON_STATE_OBJ_VALUE;
        return json_streamnext(s); /* tail-recurse into value */

    case JSON_STATE_OBJ_COMMA:
        if (c == '}') {
            s->pos++;
            s->depth--;
            if (s->depth == 0) {
                s->stack[0] = JSON_STATE_DONE;
            }
            return tok_ok(tok_simple(JSON_TOK_OBJECT_END));
        }
        if (c == ',') {
            s->pos++;
            s->stack[s->depth] = JSON_STATE_OBJ_KEY_OR_END;
            return json_streamnext(s);
        }
        return tok_err(JSON_STREAM_ERR_INVALID, "expected ',' or '}'");

    case JSON_STATE_ARR_COMMA:
        if (c == ']') {
            s->pos++;
            s->depth--;
            if (s->depth == 0) {
                s->stack[0] = JSON_STATE_DONE;
            }
            return tok_ok(tok_simple(JSON_TOK_ARRAY_END));
        }
        if (c == ',') {
            s->pos++;
            s->stack[s->depth] = JSON_STATE_ARR_VALUE_OR_END;
            return json_streamnext(s);
        }
        return tok_err(JSON_STREAM_ERR_INVALID, "expected ',' or ']'");

    case JSON_STATE_DONE:
        return tok_ok(tok_simple(JSON_TOK_END));
    }

    return tok_err(JSON_STREAM_ERR_INVALID, "internal error");
}

/* --- streaming writer --- */

JsonWriter json_newwriter(uint64_t initial_cap) {
    JsonWriter w;
    if (initial_cap < 64) initial_cap = 64;
    w.buf = malloc((size_t)initial_cap);
    w.cap = w.buf ? initial_cap : 0;
    w.pos = 0;
    return w;
}

static int writer_ensure(JsonWriter *w, uint64_t needed) {
    if (w->pos + needed <= w->cap) return 1;
    uint64_t newcap = w->cap * 2;
    while (newcap < w->pos + needed) newcap *= 2;
    uint8_t *nb = realloc(w->buf, (size_t)newcap);
    if (!nb) return 0;
    w->buf = nb;
    w->cap = newcap;
    return 1;
}

static void writer_append(JsonWriter *w, const char *data, size_t len) {
    if (writer_ensure(w, len)) {
        memcpy(w->buf + w->pos, data, len);
        w->pos += len;
    }
}

/*
 * json_streamemit — serialize a Json value (raw JSON text) into the writer.
 *
 * The Json.raw field is already valid JSON, so we copy it directly.
 */
JsonStreamVoidResult json_streamemit(JsonWriter *w, Json j) {
    JsonStreamVoidResult r;
    if (!j.raw) {
        r.is_err = 1;
        r.err = stream_err(JSON_STREAM_ERR_INVALID, "null json");
        return r;
    }
    size_t len = strlen(j.raw);
    if (!writer_ensure(w, len)) {
        r.is_err = 1;
        r.err = stream_err(JSON_STREAM_ERR_OVERFLOW, "writer oom");
        return r;
    }
    writer_append(w, j.raw, len);
    r.is_err = 0;
    return r;
}

JsonBytes json_writerbytes(const JsonWriter *w) {
    JsonBytes b;
    b.data = w->buf;
    b.len  = w->pos;
    return b;
}
