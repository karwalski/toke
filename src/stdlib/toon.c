/* SPDX-License-Identifier: MIT */
/*
 * toon.c — Implementation of std.toon for the toke compiler runtime.
 *
 * TOON (Token-Oriented Object Notation) parser/emitter.
 * Format: schema declares field names once, then pipe-delimited value rows.
 *
 *   users[3]{id,name,active}:
 *   1|Alice|true
 *   2|Bob|false
 *   3|Carol|true
 *
 * For single objects (non-tabular):
 *   {id,name,active}:
 *   1|Alice|true
 *
 * Story: 6.3.2
 */

#include "toon.h"
#include "strbuf.h"
#include "tk_strhdr.h"  /* 127.295: sealed strings, tk_strlen */
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static ToonErr make_err(ToonErrKind kind, const char *msg) {
    ToonErr e; e.kind = kind; e.msg = msg; return e;
}

/* Skip whitespace including newlines */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

/*
 * TOON escaping (121.40).
 *
 * TOON text has one escape character, '\'.  Inside a value, \\ \| \n \r
 * stand for the byte they name, so a value can hold the column separator
 * and the row separator; inside a field name \, \{ \} do too, so a name can
 * hold the schema's own punctuation.  \t is accepted on input.  A '\'
 * before any other byte is kept, with that byte, as written -- TOON written
 * before this rule (a Windows path, say) reads back unchanged.
 *
 * Before 121.40 nothing was escaped in either direction: toon_to_json wrote
 * names and values between quotes raw, so the value x","admin":true,"y
 * became three JSON members, and toon_from_json copied a '|' or '{' in a
 * JSON string straight into TOON syntax.
 */
static void toon_esc(TkStrBuf *b, const char *s, size_t n, int is_name) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
        case '\\': strbuf_appendn(b, "\\\\", 2); break;
        case '|':  strbuf_appendn(b, "\\|", 2);  break;
        case '\n': strbuf_appendn(b, "\\n", 2);  break;
        case '\r': strbuf_appendn(b, "\\r", 2);  break;
        case ',': case '{': case '}':
            if (is_name) strbuf_putc(b, '\\');
            strbuf_putc(b, c);
            break;
        default:   strbuf_putc(b, c);            break;
        }
    }
}

/* Decode the TOON-escaped bytes [s, e) into b. */
static void toon_unesc(TkStrBuf *b, const char *s, const char *e) {
    while (s < e) {
        if (*s == '\\' && s + 1 < e) {
            char c = s[1];
            switch (c) {
            case 'n': strbuf_putc(b, '\n'); break;
            case 'r': strbuf_putc(b, '\r'); break;
            case 't': strbuf_putc(b, '\t'); break;
            case '\\': case '|': case ',': case '{': case '}':
                strbuf_putc(b, c); break;
            default:  strbuf_putc(b, '\\'); strbuf_putc(b, c); break;
            }
            s += 2;
        } else {
            strbuf_putc(b, *s++);
        }
    }
}

/* The end of the value starting at p: the next unescaped '|', or the end of
 * the line (the first raw '\n', or the NUL).  An escaped byte never ends a
 * value; an escape cannot swallow the line's own '\n'. */
static const char *toon_val_end(const char *p) {
    while (*p && *p != '|' && *p != '\n') {
        if (*p == '\\' && p[1] && p[1] != '\n') p++;
        p++;
    }
    return p;
}

static void free_fields(char **f, int n) {
    for (int i = 0; i < n; i++) free(f[i]);
    free(f);
}

/*
 * Parse schema header: name[count]{field1,field2,...}:
 * or:                  {field1,field2,...}:
 *
 * Returns the DECODED field names via fields/nfields, row count via rowcount.
 * Advances *pp past the colon + newline.  A '\' in the field list escapes
 * the next byte, so \, and \} do not split or close it.  Returns 0 on a
 * malformed header or allocation failure (nothing to free then).
 */
static int parse_schema(const char **pp, char ***fields, int *nfields, int *rowcount) {
    const char *p = skip_ws(*pp);
    *rowcount = 0;

    /* Optional name before '[' or '{' */
    while (*p && *p != '[' && *p != '{' && *p != '\n') p++;

    /* Optional [count] */
    if (*p == '[') {
        p++;
        *rowcount = (int)strtol(p, (char **)&p, 10);
        if (*rowcount < 0) *rowcount = 0;
        if (*p == ']') p++;
    }

    /* Required {field1,field2,...} */
    if (*p != '{') return 0;
    p++;

    /* Count unescaped commas up to the unescaped '}' */
    int count = 1;
    const char *scan = p;
    while (*scan && *scan != '}') {
        if (*scan == '\\' && scan[1]) scan++;
        else if (*scan == ',') count++;
        scan++;
    }
    if (*scan != '}') return 0;

    char **flds = (char **)calloc((size_t)count, sizeof(char *));
    if (!flds) return 0;
    int idx = 0;
    const char *start = p;
    for (;;) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == ',' || *p == '}') {
            TkStrBuf fb = TK_STRBUF_INIT;
            toon_unesc(&fb, start, p);
            char *f = strbuf_take(&fb);
            if (!f) { free_fields(flds, idx); return 0; }
            flds[idx++] = f;
            if (*p == '}') break;
            start = p + 1;
        }
        p++;
    }
    p++; /* skip '}' */

    if (*p == ':') p++;
    if (*p == '\n') p++;

    *fields = flds;
    *nfields = idx;
    *pp = p;
    return 1;
}

/*
 * Find a field value in TOON data by key name.
 * Searches the schema for the field index, then extracts
 * the corresponding pipe-delimited value from the first data row.
 * The value is returned still escaped; toon_str decodes it.
 */
static int find_toon_field(const char *raw, const char *key,
                           const char **val_start, const char **val_end) {
    const char *p = raw;
    char **fields = NULL;
    int nfields = 0, rowcount = 0;

    if (!parse_schema(&p, &fields, &nfields, &rowcount))
        return 0;

    /* Find field index */
    int field_idx = -1;
    for (int i = 0; i < nfields; i++) {
        if (strcmp(fields[i], key) == 0) field_idx = i;
    }
    free_fields(fields, nfields);

    if (field_idx < 0) return 0;

    /* Navigate to the correct pipe-delimited field in the first data row */
    const char *vs = p;
    for (int cur = 0; cur < field_idx; cur++) {
        vs = toon_val_end(vs);
        if (*vs != '|') break;
        vs++;
    }
    *val_start = vs;
    *val_end = toon_val_end(vs);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* 127.295: the strs this file hands out (toon_enc, toon_str, toon_from_json,
 * toon_to_json) are sealed strings: one copy of the finished buffer, measured
 * as a C string (so a decoded 0 byte ends it where every C reader already saw
 * it end), and the buffer freed.  NULL if any append or the copy hit OOM.  A
 * sealed string is never freed; no C caller frees these (toon_glue.c hands
 * them to toke, and the "" / "[]" fallbacks were never freeable). */
static char *toon_seal(TkStrBuf *b) {
    if (b->oom) { strbuf_free(b); return NULL; }
    const char *s = strbuf_cstr(b);
    const char *z = b->len ? (const char *)memchr(s, 0, b->len) : NULL;
    char *t = tks_dup(s, z ? (size_t)(z - s) : b->len);
    strbuf_free(b);
    return t;
}

/* toon_enc — encode a value to its TOON representation: the value itself,
 * with '\\', '|', newline and CR escaped (121.40) so it stays one value in
 * one row.  A value without those bytes comes back unchanged. */
const char *toon_enc(const char *v) {
    if (!v) return "";
    TkStrBuf b = TK_STRBUF_INIT;
    toon_esc(&b, v, tk_strlen(v), 0);
    char *out = toon_seal(&b);
    return out ? out : "";
}

/* toon_dec — validate and store raw TOON string */
ToonResult toon_dec(const char *s) {
    ToonResult r = {0};
    if (!s || !*s) {
        r.is_err = 1;
        r.err = make_err(TOON_ERR_PARSE, "empty input");
        return r;
    }

    /* Validate: must contain '{' for schema */
    const char *p = s;
    while (*p && *p != '{') p++;
    if (*p != '{') {
        r.is_err = 1;
        r.err = make_err(TOON_ERR_PARSE, "missing schema declaration");
        return r;
    }
    while (*p && *p != '}') p++;
    if (*p != '}') {
        r.is_err = 1;
        r.err = make_err(TOON_ERR_PARSE, "unclosed schema");
        return r;
    }

    r.is_err = 0;
    r.ok.raw = s;
    return r;
}

/* toon_str — extract a string value for key, decoded (121.40) */
StrToonResult toon_str(Toon t, const char *key) {
    StrToonResult r = {0};
    const char *vs, *ve;
    if (!find_toon_field(t.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }
    TkStrBuf b = TK_STRBUF_INIT;
    toon_unesc(&b, vs, ve);
    char *buf = toon_seal(&b);
    if (!buf) { r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "oom"); return r; }
    r.is_err = 0; r.ok = buf; return r;
}

/* toon_i64 — extract an integer value for key */
I64ToonResult toon_i64(Toon t, const char *key) {
    I64ToonResult r = {0};
    const char *vs, *ve;
    if (!find_toon_field(t.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    int64_t v = (int64_t)strtoll(vs, &end, 10);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(TOON_ERR_TYPE, "not an i64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* toon_f64 — extract a float value for key */
F64ToonResult toon_f64(Toon t, const char *key) {
    F64ToonResult r = {0};
    const char *vs, *ve;
    if (!find_toon_field(t.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    double v = strtod(vs, &end);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(TOON_ERR_TYPE, "not an f64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* toon_bool — extract a boolean value for key */
BoolToonResult toon_bool(Toon t, const char *key) {
    BoolToonResult r = {0};
    const char *vs, *ve;
    if (!find_toon_field(t.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }
    size_t len = (size_t)(ve - vs);
    if (len == 4 && strncmp(vs, "true", 4) == 0) {
        r.is_err = 0; r.ok = 1; return r;
    }
    if (len == 5 && strncmp(vs, "false", 5) == 0) {
        r.is_err = 0; r.ok = 0; return r;
    }
    r.is_err = 1; r.err = make_err(TOON_ERR_TYPE, "not a bool"); return r;
}

/* toon_arr — extract array rows as ToonArray */
ToonArrayResult toon_arr(Toon t, const char *key) {
    ToonArrayResult r = {0};
    const char *p = t.raw;
    char **fields = NULL;
    int nfields = 0, rowcount = 0;

    if (!parse_schema(&p, &fields, &nfields, &rowcount)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "bad schema"); return r;
    }

    /* Find the field index for key */
    int field_idx = -1;
    for (int i = 0; i < nfields; i++) {
        if (strcmp(fields[i], key) == 0) field_idx = i;
    }
    free_fields(fields, nfields);

    if (field_idx < 0) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }

    /* Count actual rows */
    int actual_rows = 0;
    const char *rp = p;
    while (*rp) {
        if (*rp == '\n') { actual_rows++; rp++; continue; }
        /* non-empty line = a row */
        while (*rp && *rp != '\n') rp++;
        actual_rows++;
        if (*rp == '\n') rp++;
    }
    /* Recount: lines with content */
    actual_rows = 0;
    rp = p;
    while (*rp) {
        const char *line_start = rp;
        while (*rp && *rp != '\n') rp++;
        if (rp > line_start) actual_rows++;
        if (*rp == '\n') rp++;
    }

    /* 121.16 (PAR-06): the declared [count] is the sender's claim, not a
     * fact.  It was trusted over the rows actually present: `d[100]{id}:\n1\n`
     * returned a length-100 array whose elements 1..99 were uninitialised
     * pointers, a negative count returned a length near 2^64 over a NULL
     * array, and a blank line consumed a row slot as an empty value.  Now a
     * declared count can only shorten the result (a count below the rows
     * present keeps the first N, as before), blank lines are skipped, and
     * `len` is the number of elements actually filled. */
    int rows = actual_rows;
    if (rowcount > 0 && rowcount < actual_rows) rows = rowcount;

    Toon *arr = (Toon *)calloc(rows > 0 ? (size_t)rows : 1, sizeof(Toon));
    if (!arr) {
        r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "oom"); return r;
    }

    /* Extract field from each row.  Splitting is escape-aware (121.40); the
     * element keeps the value's TOON text, escapes and all. */
    int filled = 0;
    rp = p;
    while (filled < rows && *rp) {
        const char *line_start = rp;
        while (*rp && *rp != '\n') rp++;
        if (rp == line_start) { rp++; continue; }   /* blank line */
        const char *fp = line_start;
        for (int cur = 0; cur < field_idx; cur++) {
            fp = toon_val_end(fp);
            if (*fp != '|') break;
            fp++;
        }
        const char *fe = toon_val_end(fp);
        size_t len = (size_t)(fe - fp);
        char *val = (char *)malloc(len + 1);
        if (!val) {                                  /* PAR-10 */
            for (int i = 0; i < filled; i++) free((char *)arr[i].raw);
            free(arr);
            r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "oom"); return r;
        }
        memcpy(val, fp, len);
        val[len] = '\0';
        arr[filled++].raw = val;
        if (*rp == '\n') rp++;
    }
    r.is_err = 0;
    r.ok.data = arr;
    r.ok.len = (uint64_t)filled;
    return r;
}

/* ------------------------------------------------------------------ */
/* JSON <-> TOON conversion                                             */
/* ------------------------------------------------------------------ */

/* JSON scanning for toon_from_json (121.40).  The old scanner treated every
 * '{' in the input as the start of an object and every '"' as the end of a
 * key, so a string value could add rows and a key could end early. */
static const char *jws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at a JSON string's opening quote: return just past the closing quote
 * (or at the NUL). */
static const char *jskip_str(const char *p) {
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p == '"' ? p + 1 : p;
}

/* p at any JSON value: return just past it.  Objects and arrays are skipped
 * whole, strings inside them included; a scalar runs to the next ',', '}',
 * ']' or whitespace. */
static const char *jskip_val(const char *p) {
    if (*p == '"') return jskip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = jskip_str(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

/* One member of a JSON object: its decoded key, and its value's span. */
typedef struct { char *key; const char *vs, *ve; } JMember;

/* p at '{': collect the object's members (at most `max`), return the count,
 * or -1 on allocation failure.  *endp is left just past the object. */
static int jobject(const char *p, JMember *m, int max, const char **endp) {
    int n = 0;
    p = jws(p + 1);
    while (*p == '"') {
        TkStrBuf kb = TK_STRBUF_INIT;
        const char *q = p + 1;
        strbuf_append_json_unescape(&kb, &q);
        p = jws(q);
        if (*p == ':') p = jws(p + 1);
        const char *vs = p;
        p = jskip_val(p);
        if (n < max) {
            char *k = strbuf_take(&kb);
            if (!k) { for (int i = 0; i < n; i++) free(m[i].key); return -1; }
            m[n].key = k; m[n].vs = vs; m[n].ve = p; n++;
        } else {
            strbuf_free(&kb);
        }
        p = jws(p);
        if (*p == ',') p = jws(p + 1);
    }
    /* Anything else before the '}' is malformed: skip to the object's end. */
    while (*p && *p != '}') {
        if (*p == '"') { p = jskip_str(p); continue; }
        if (*p == '{' || *p == '[') { p = jskip_val(p); continue; }
        p++;
    }
    *endp = *p == '}' ? p + 1 : p;
    return n;
}

/* Append a JSON value as one TOON value: a string is decoded and escaped,
 * anything else (number, literal, nested object or array) is escaped as
 * written. */
static void jval_to_toon(TkStrBuf *b, const char *vs, const char *ve) {
    if (vs < ve && *vs == '"') {
        TkStrBuf vb = TK_STRBUF_INIT;
        const char *q = vs + 1;
        strbuf_append_json_unescape(&vb, &q);
        toon_esc(b, strbuf_cstr(&vb), vb.len, 0);
        strbuf_free(&vb);
    } else {
        toon_esc(b, vs, (size_t)(ve - vs), 0);
    }
}

#define TOON_MAX_KEYS 1024

/*
 * toon_from_json — convert a JSON array of objects to TOON format.
 *
 * Input:  [{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}]
 * Output: data[2]{id,name}:\n1|Alice\n2|Bob\n
 *
 * The first object's keys are the schema; each later object contributes the
 * value of each schema key (empty when absent).  Elements that are not
 * objects are skipped.  Keys and values are TOON-escaped (121.40).
 */
const char *toon_from_json(const char *json) {
    if (!json) return "";
    const char *p = jws(json);
    int single = (*p == '{');
    if (!single) {
        if (*p != '[') return "";
        p = jws(p + 1);
        if (*p != '{') return "";
    }

    JMember *keys = (JMember *)malloc(TOON_MAX_KEYS * sizeof(JMember));
    JMember *mem  = (JMember *)malloc(TOON_MAX_KEYS * sizeof(JMember));
    if (!keys || !mem) { free(keys); free(mem); return ""; }
    const char *after;
    int nkeys = jobject(p, keys, TOON_MAX_KEYS, &after);
    if (nkeys < 0) { free(keys); free(mem); return ""; }

    TkStrBuf rows = TK_STRBUF_INIT;
    int nrows = 0;
    while (*p == '{') {
        int nm = jobject(p, mem, TOON_MAX_KEYS, &after);
        if (nm < 0) break;
        for (int ki = 0; ki < nkeys; ki++) {
            if (ki > 0) strbuf_putc(&rows, '|');
            for (int mi = 0; mi < nm; mi++) {
                if (strcmp(mem[mi].key, keys[ki].key) == 0) {
                    jval_to_toon(&rows, mem[mi].vs, mem[mi].ve);
                    break;
                }
            }
        }
        strbuf_putc(&rows, '\n');
        nrows++;
        for (int mi = 0; mi < nm; mi++) free(mem[mi].key);
        if (single) break;
        /* next object element; skip anything that is not an object.
         * 127.543: jskip_val returns a scalar's end, and a stray '}' (or
         * any byte jws does not skip but a scalar stops at) IS its own end,
         * so `[{"a":1}}` never advanced and this loop spun forever.  Every
         * pass now consumes at least one byte. */
        p = jws(after);
        while (*p == ',' || (*p && *p != '{' && *p != ']')) {
            if (*p == ',') { p = jws(p + 1); continue; }
            const char *q = jskip_val(p);
            p = jws(q > p ? q : p + 1);
        }
    }

    TkStrBuf ob = TK_STRBUF_INIT;
    if (!single) strbuf_appendf(&ob, "data[%d]", nrows);
    strbuf_putc(&ob, '{');
    for (int i = 0; i < nkeys; i++) {
        if (i > 0) strbuf_putc(&ob, ',');
        toon_esc(&ob, keys[i].key, tk_strlen(keys[i].key), 1);
        free(keys[i].key);
    }
    strbuf_appendn(&ob, "}:\n", 3);
    strbuf_appendn(&ob, strbuf_cstr(&rows), rows.len);
    if (rows.oom) ob.oom = 1;
    strbuf_free(&rows);
    free(keys);
    free(mem);
    char *out = toon_seal(&ob);
    return out ? out : "";
}

/* 1 when [s, e) is a JSON number (RFC 8259 grammar), 0 otherwise. */
static int is_json_number(const char *s, const char *e) {
    const char *p = s;
    if (p < e && *p == '-') p++;
    if (p >= e) return 0;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (p < e && isdigit((unsigned char)*p)) p++; }
    else return 0;
    if (p < e && *p == '.') {
        p++;
        if (p >= e || !isdigit((unsigned char)*p)) return 0;
        while (p < e && isdigit((unsigned char)*p)) p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || !isdigit((unsigned char)*p)) return 0;
        while (p < e && isdigit((unsigned char)*p)) p++;
    }
    return p == e;
}

/*
 * toon_to_json — convert TOON format back to JSON array of objects.
 *
 * Input:  data[2]{id,name}:\n1|Alice\n2|Bob\n
 * Output: [{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}]
 *
 * Field names and string values are decoded from TOON and JSON-escaped
 * (121.40); a value is written bare only when it is a JSON number, true,
 * false or null.
 */
const char *toon_to_json(const char *toon_str) {
    if (!toon_str) return "[]";
    const char *p = toon_str;
    char **fields = NULL;
    int nfields = 0, rowcount = 0;

    if (!parse_schema(&p, &fields, &nfields, &rowcount)) return "[]";

    /* 122.2: grew by ONE doubling when a value did not fit, so a field
     * name or value longer than the buffer still wrote past it. */
    TkStrBuf ob = TK_STRBUF_INIT;
    TkStrBuf vb = TK_STRBUF_INIT;
    strbuf_putc(&ob, '[');

    int row = 0;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        if (p == line_start) { if (*p == '\n') p++; continue; }

        if (row > 0) strbuf_putc(&ob, ',');
        strbuf_putc(&ob, '{');

        const char *vp = line_start;
        for (int fi = 0; fi < nfields; fi++) {
            if (fi > 0) strbuf_putc(&ob, ',');
            const char *vs = vp;
            const char *ve = toon_val_end(vs);
            if (ve > p) ve = p;
            vp = ve;

            vb.len = 0;
            if (vb.buf) vb.buf[0] = '\0';
            toon_unesc(&vb, vs, ve);
            const char *v = strbuf_cstr(&vb);
            size_t vlen = vb.len;
            int bare = is_json_number(v, v + vlen) ||
                       (vlen == 4 && !strcmp(v, "true")) ||
                       (vlen == 5 && !strcmp(v, "false")) ||
                       (vlen == 4 && !strcmp(v, "null"));

            strbuf_putc(&ob, '"');
            strbuf_append_json(&ob, fields[fi]);
            strbuf_appendn(&ob, "\":", 2);
            if (bare) {
                strbuf_appendn(&ob, v, vlen);
            } else {
                strbuf_putc(&ob, '"');
                strbuf_append_json(&ob, v);
                strbuf_putc(&ob, '"');
            }

            if (vp < p && *vp == '|') vp++;
        }

        strbuf_putc(&ob, '}');
        row++;
        if (*p == '\n') p++;
    }

    strbuf_putc(&ob, ']');
    if (vb.oom) ob.oom = 1;
    strbuf_free(&vb);

    free_fields(fields, nfields);
    char *out = toon_seal(&ob);
    return out ? out : "[]";
}
