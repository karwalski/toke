/* SPDX-License-Identifier: MIT */
/*
 * yaml.c — Implementation of std.yaml for the toke compiler runtime.
 *
 * Minimal YAML parser/emitter for toke. Handles:
 *   - Flat key-value mappings (key: value)
 *   - Simple sequences (- item)
 *   - Scalar types: strings, integers, floats, booleans, null
 *
 * Does NOT handle:
 *   - Anchors/aliases (&, *)
 *   - Multi-document streams (---)
 *   - Flow collections ({}, [])
 *   - Complex keys
 *   - Tags (!!)
 *
 * Story: 6.3.3
 */

#include "yaml.h"
#include "tk_strhdr.h"  /* 127.295: sealed strings, tk_strlen */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

/*
 * 121.4 (PAR-01) replaced an overflowing `pos += snprintf(out+pos, cap-pos,…)`
 * with a clamp, which traded the overflow for silent truncation at a cap of
 * 2*strlen(input)+256 (an int, so it also overflowed past 1 GiB of input).
 * 122.2: both converters now append to a TkStrBuf, which grows instead.
 */
#include "strbuf.h"

/*
 * 127.295 (S1 step 5, fourteenth tranche): every string yaml.c returns to a
 * caller -- yaml_enc, yaml_str, yaml_from_json, yaml_to_json -- is sealed
 * (tks_alloc/tks_dup + tks_seal), so tk_strlen reads its length in O(1).
 * No C caller frees one: yaml_glue.c hands them to toke, and the tests and
 * fuzz-yaml never freed them (each producer has literal fallbacks -- "null",
 * "\"\"", "", "{}" -- so a free was never allowed).  A sealed string is never
 * freed, by anyone.  If the sealed copy cannot be made the converters hand
 * over their malloc'd buffer instead (read correctly, only with strlen).
 *
 * Stays malloc'd, on purpose: the element raws yaml_arr builds (internal
 * Yaml handle storage, never a str toke reads; yaml_arr frees them itself on
 * failure), and the TkStrBuf the converters build in.
 */

/* A sealed copy of b's contents past `skip` bytes, b freed; lit when the
 * buffer hit OOM (a partial string is never handed out). */
static const char *seal_buf(TkStrBuf *b, size_t skip, const char *lit) {
    size_t n = b->len;
    char *out = strbuf_take(b);
    if (!out) return lit;
    if (skip > n) skip = n;
    char *s = tks_dup(out + skip, n - skip);
    if (!s) {
        if (skip) memmove(out, out + skip, n - skip + 1);
        return out;
    }
    free(out);
    return s;
}

/* Bound converter recursion depth (121.6 companion) — indent grows by 2/level. */
#define YAML_MAX_INDENT 512

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static YamlErr make_err(YamlErrKind kind, const char *msg) {
    YamlErr e; e.kind = kind; e.msg = msg; return e;
}

/* Skip spaces and tabs (not newlines) */
static const char *skip_inline_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Find the end of the current line */
static const char *find_eol(const char *p) {
    while (*p && *p != '\n' && *p != '\r') p++;
    return p;
}

/* Advance past newline */
static const char *skip_newline(const char *p) {
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    return p;
}

/*
 * Find a key in YAML mapping.
 * Scans for "key: value" lines at the top indentation level.
 * Sets *val_start to the first char of value, *val_end past it.
 */
static int find_yaml_key(const char *raw, const char *key,
                         const char **val_start, const char **val_end) {
    size_t klen = tk_strlen(key);
    const char *p = raw;

    while (*p) {
        const char *line = skip_inline_ws(p);
        const char *eol = find_eol(line);

        /* Check if this line starts with "key:" */
        if ((size_t)(eol - line) >= klen + 1 &&
            strncmp(line, key, klen) == 0 &&
            line[klen] == ':') {

            const char *vs = skip_inline_ws(line + klen + 1);

            /* Handle inline value */
            if (vs < eol && *vs != '\0' && *vs != '\n') {
                /* Strip optional trailing comment */
                const char *ve = eol;
                /* Trim trailing whitespace */
                while (ve > vs && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) ve--;
                /* Strip surrounding quotes if present */
                if (ve - vs >= 2 &&
                    ((*vs == '"' && *(ve - 1) == '"') ||
                     (*vs == '\'' && *(ve - 1) == '\''))) {
                    vs++;
                    ve--;
                }
                *val_start = vs;
                *val_end = ve;
                return 1;
            }

            /* Block scalar or sequence on next lines — return everything
               at deeper indentation */
            int base_indent = (int)(line - p);
            const char *block_start = skip_newline(eol);
            const char *block_end = block_start;
            while (*block_end) {
                const char *next_line = block_end;
                const char *content = skip_inline_ws(next_line);
                int indent = (int)(content - next_line);
                if (*content == '\n' || *content == '\0') {
                    block_end = skip_newline(content);
                    continue;
                }
                if (indent <= base_indent) break;
                block_end = skip_newline(find_eol(content));
            }
            *val_start = block_start;
            *val_end = block_end;
            return 1;
        }

        p = skip_newline(eol);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* yaml_enc — encode a value to YAML string */
const char *yaml_enc(const char *v) {
    if (!v) return "null";
    /* Check if value needs quoting */
    int needs_quote = 0;
    const char *p = v;
    if (*p == '\0') needs_quote = 1;
    while (*p && !needs_quote) {
        if (*p == ':' || *p == '#' || *p == '[' || *p == ']' ||
            *p == '{' || *p == '}' || *p == ',' || *p == '&' ||
            *p == '*' || *p == '!' || *p == '|' || *p == '>' ||
            *p == '\'' || *p == '"' || *p == '%' || *p == '@' ||
            *p == '\n') {
            needs_quote = 1;
        }
        p++;
    }
    size_t n = tk_strlen(v);
    if (needs_quote) {
        char *buf = tks_alloc(n + 2);
        if (!buf) return "\"\"";
        buf[0] = '"';
        memcpy(buf + 1, v, n);
        buf[n + 1] = '"';
        buf[n + 2] = '\0';
        return tks_seal(buf, n + 2);
    }
    char *buf = tks_dup(v, n);
    return buf ? buf : "";
}

/* yaml_dec — validate and store raw YAML string */
YamlResult yaml_dec(const char *s) {
    YamlResult r = {0};
    if (!s || !*s) {
        r.is_err = 1;
        r.err = make_err(YAML_ERR_PARSE, "empty input");
        return r;
    }
    r.is_err = 0;
    r.ok.raw = s;
    return r;
}

/* yaml_str — extract a string value for key */
StrYamlResult yaml_str(Yaml y, const char *key) {
    StrYamlResult r = {0};
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }
    size_t len = (size_t)(ve - vs);
    char *buf = tks_dup(vs, len);
    if (!buf) { r.is_err = 1; r.err = make_err(YAML_ERR_PARSE, "oom"); return r; }
    r.is_err = 0; r.ok = buf; return r;
}

/* yaml_i64 — extract an integer value for key */
I64YamlResult yaml_i64(Yaml y, const char *key) {
    I64YamlResult r = {0};
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    int64_t v = (int64_t)strtoll(vs, &end, 10);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(YAML_ERR_TYPE, "not an i64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* yaml_f64 — extract a float value for key */
F64YamlResult yaml_f64(Yaml y, const char *key) {
    F64YamlResult r = {0};
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }
    char *end; errno = 0;
    double v = strtod(vs, &end);
    if (end == vs || errno) {
        r.is_err = 1; r.err = make_err(YAML_ERR_TYPE, "not an f64"); return r;
    }
    r.is_err = 0; r.ok = v; return r;
}

/* yaml_bool — extract a boolean value for key */
BoolYamlResult yaml_bool(Yaml y, const char *key) {
    BoolYamlResult r = {0};
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }
    size_t len = (size_t)(ve - vs);
    if (len == 4 && strncmp(vs, "true", 4) == 0) {
        r.is_err = 0; r.ok = 1; return r;
    }
    if (len == 5 && strncmp(vs, "false", 5) == 0) {
        r.is_err = 0; r.ok = 0; return r;
    }
    /* YAML also accepts yes/no, on/off */
    if (len == 3 && strncmp(vs, "yes", 3) == 0) {
        r.is_err = 0; r.ok = 1; return r;
    }
    if (len == 2 && strncmp(vs, "no", 2) == 0) {
        r.is_err = 0; r.ok = 0; return r;
    }
    r.is_err = 1; r.err = make_err(YAML_ERR_TYPE, "not a bool"); return r;
}

/* 121.43 (5): one predicate for "this line is a sequence item", used by
 * both passes of yaml_arr.  The counting pass took only "- " and "-\n"; the
 * extraction pass took ANY line starting with '-'.  So a block such as
 *     -5
 *     - a
 *     - b
 * was counted as two items and extracted as "5" and "a" -- a value that was
 * never an item, and the last real item dropped.  A '-' starts an item only
 * when followed by a space, a tab, a line end or the end of the block (YAML
 * 1.2 8.2.1); "-5" and "--x" are scalars and are not items. */
static int yaml_is_seq_item(const char *line, const char *end) {
    if (line >= end || *line != '-') return 0;
    if (line + 1 >= end) return 1;
    char c = line[1];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0';
}

/* yaml_arr — extract a YAML sequence as YamlArray */
YamlArrayResult yaml_arr(Yaml y, const char *key) {
    YamlArrayResult r = {0};
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }

    /* Count sequence items */
    uint64_t count = 0;
    const char *p = vs;
    while (p < ve) {
        const char *line = skip_inline_ws(p);
        if (yaml_is_seq_item(line, ve)) count++;
        const char *eol = find_eol(line);
        p = skip_newline(eol);
    }

    Yaml *arr = (Yaml *)malloc(count * sizeof(Yaml));
    if (!arr && count > 0) {
        r.is_err = 1; r.err = make_err(YAML_ERR_PARSE, "oom"); return r;
    }

    /* Extract items -- the same predicate, so idx reaches exactly count */
    p = vs;
    uint64_t idx = 0;
    while (p < ve && idx < count) {
        const char *line = skip_inline_ws(p);
        if (yaml_is_seq_item(line, ve)) {
            const char *item = skip_inline_ws(line + 1);
            const char *eol = find_eol(item);
            if (eol > ve) eol = ve;
            /* Trim trailing whitespace */
            while (eol > item && (*(eol - 1) == ' ' || *(eol - 1) == '\t')) eol--;
            size_t len = eol > item ? (size_t)(eol - item) : 0;
            char *val = (char *)malloc(len + 1);
            if (!val) {
                /* 121.41: this was unchecked -- memcpy through NULL. */
                for (uint64_t i = 0; i < idx; i++) free((char *)arr[i].raw);
                free(arr);
                r.is_err = 1; r.err = make_err(YAML_ERR_PARSE, "oom"); return r;
            }
            memcpy(val, item, len);
            val[len] = '\0';
            /* Strip quotes */
            if (len >= 2 && ((val[0] == '"' && val[len - 1] == '"') ||
                             (val[0] == '\'' && val[len - 1] == '\''))) {
                memmove(val, val + 1, len - 2);
                val[len - 2] = '\0';
            }
            arr[idx].raw = val;
            idx++;
        }
        const char *eol = find_eol(line);
        p = skip_newline(eol);
    }

    r.is_err = 0;
    r.ok.data = arr;
    r.ok.len = idx;
    return r;
}

/* ------------------------------------------------------------------ */
/* JSON <-> YAML conversion                                             */
/* ------------------------------------------------------------------ */

/* Forward declarations for JSON parsing helpers */
static const char *json_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *json_skip_string(const char *p) {
    if (*p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\') { p++; if (*p) p++; continue; }   /* 121.5: no OOB on trailing '\' */
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *json_skip_value(const char *p);

static const char *json_skip_array(const char *p) {
    if (*p != '[') return p;
    p++;
    p = json_skip_ws(p);
    if (*p == ']') return p + 1;
    while (*p) {
        p = json_skip_value(json_skip_ws(p));
        p = json_skip_ws(p);
        if (*p == ']') return p + 1;
        if (*p == ',') { p++; continue; }
        return p;
    }
    return p;
}

static const char *json_skip_object(const char *p) {
    if (*p != '{') return p;
    p++;
    p = json_skip_ws(p);
    if (*p == '}') return p + 1;
    while (*p) {
        p = json_skip_string(json_skip_ws(p));
        p = json_skip_ws(p);
        if (*p == ':') p++;
        p = json_skip_value(json_skip_ws(p));
        p = json_skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p == ',') { p++; continue; }
        return p;
    }
    return p;
}

/* 127.544: the literals true/false/null were skipped as a fixed p+4 / p+5,
 * so a document ending inside one (`[t`, `{"a":n`) left p past the string's
 * terminator and every later read was out of bounds (heap-buffer-overflow
 * under ASan; a loop over whatever memory followed).  Skip at most n bytes,
 * never the terminator. */
static const char *json_skip_lit(const char *p, int n) {
    while (n-- > 0 && *p) p++;
    return p;
}

static const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    if (*p == '"') return json_skip_string(p);
    if (*p == '{') return json_skip_object(p);
    if (*p == '[') return json_skip_array(p);
    if (*p == 't') return json_skip_lit(p, 4);
    if (*p == 'f') return json_skip_lit(p, 5);
    if (*p == 'n') return json_skip_lit(p, 4);
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        if (*p == '-') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
        if (*p == 'e' || *p == 'E') {
            p++;
            if (*p == '+' || *p == '-') p++;
            while (*p >= '0' && *p <= '9') p++;
        }
        return p;
    }
    return p;
}

/* Recursive JSON-to-YAML with indentation.
 *
 * 127.542: every loop iteration below must consume input.  An element that
 * is none of { [ " and no literal json_skip_value knows (the fuzz input
 * `{"b":[1,2@]}`: '@'), and any element past the YAML_MAX_INDENT depth cap,
 * comes back with *pp unmoved; the sequence loop then saw the same byte, not
 * ']', and appended "\n- " forever (libFuzzer OOM at 2 GiB).  Each loop now
 * stops when an element made no progress and *stop unwinds every enclosing
 * frame, so no parent goes on to parse the bytes a child left behind.  The
 * work is linear in the input and the output is bounded by it (at most
 * YAML_MAX_INDENT + a few bytes per input byte).  An empty element on a comma
 * (`[1,,2]`, `{"a":,"b":1}`) consumes the comma and converts as before. */
static void json_to_yaml_r(const char **pp, TkStrBuf *ob, int indent,
                           int *stop) {
    if (indent > YAML_MAX_INDENT) return;   /* 121.6: bound recursion depth */
    const char *p = json_skip_ws(*pp);

    if (*p == '{') {
        p++;
        p = json_skip_ws(p);
        if (*p == '}') { *pp = p + 1; strbuf_appendf(ob, "{}"); return; }
        int first = 1;
        while (*p && *p != '}') {
            if (!first) { /* already on new line */ }
            first = 0;
            /* Key */
            p = json_skip_ws(p);
            if (*p != '"') break;
            p++;
            const char *ks = p;
            while (*p && *p != '"') p++;
            strbuf_appendf(ob, "\n%*s%.*s: ",
                            indent, "", (int)(p - ks), ks);
            if (*p == '"') p++;
            p = json_skip_ws(p);
            if (*p == ':') p++;
            p = json_skip_ws(p);
            /* Value */
            const char *before = p;
            json_to_yaml_r(&p, ob, indent + 2, stop);
            /* 127.542: no progress (except on a comma) -- stop everything */
            if (*stop || (p == before && *p != ',')) { *stop = 1; break; }
            p = json_skip_ws(p);
            if (*p == ',') p++;
        }
        if (!*stop && *p == '}') p++;
        *pp = p;
        return;
    }

    if (*p == '[') {
        p++;
        p = json_skip_ws(p);
        if (*p == ']') { *pp = p + 1; strbuf_appendf(ob, "[]"); return; }
        while (*p && *p != ']') {
            p = json_skip_ws(p);
            strbuf_appendf(ob, "\n%*s- ", indent, "");
            const char *before = p;
            json_to_yaml_r(&p, ob, indent + 2, stop);
            /* 127.542: no progress (except on a comma) -- stop everything */
            if (*stop || (p == before && *p != ',')) { *stop = 1; break; }
            p = json_skip_ws(p);
            if (*p == ',') p++;
        }
        if (!*stop && *p == ']') p++;
        *pp = p;
        return;
    }

    if (*p == '"') {
        p++;
        const char *vs = p;
        while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
        strbuf_appendf(ob, "%.*s", (int)(p - vs), vs);
        if (*p == '"') p++;
        *pp = p;
        return;
    }

    /* Number, boolean, null — copy literally */
    const char *start = p;
    p = json_skip_value(p);
    strbuf_appendf(ob, "%.*s", (int)(p - start), start);
    *pp = p;
}

/*
 * yaml_from_json — convert JSON to YAML.
 */
const char *yaml_from_json(const char *json) {
    if (!json) return "";
    TkStrBuf ob = TK_STRBUF_INIT;
    const char *p = json;
    int stop = 0;
    json_to_yaml_r(&p, &ob, 0, &stop);
    /* Trim the leading newline a top-level mapping starts with. */
    size_t skip = (!ob.oom && ob.len && ob.buf[0] == '\n') ? 1 : 0;
    return seal_buf(&ob, skip, "");
}

/*
 * yaml_to_json — convert simple YAML back to JSON.
 * Handles flat mappings and simple sequences.
 */
const char *yaml_to_json(const char *yaml_str) {
    if (!yaml_str) return "{}";
    const char *p = yaml_str;

    TkStrBuf ob = TK_STRBUF_INIT;

    /* Detect if top-level is a sequence or mapping */
    const char *first_line = skip_inline_ws(p);
    if (*first_line == '-') {
        /* Sequence */
        strbuf_appendf(&ob, "[");
        int first = 1;
        while (*p) {
            const char *line = skip_inline_ws(p);
            const char *eol = find_eol(line);
            if (line >= eol) { p = skip_newline(eol); continue; }

            if (*line == '-') {
                if (!first) strbuf_appendf(&ob, ",");
                first = 0;
                const char *item = skip_inline_ws(line + 1);
                if (*item == ' ') item++;
                const char *ie = eol;
                while (ie > item && (*(ie - 1) == ' ' || *(ie - 1) == '\t')) ie--;
                /* Check if numeric/bool/null */
                int is_num = 1;
                const char *check = item;
                if (check < ie && *check == '-') check++;
                if (check == ie) is_num = 0;
                while (check < ie) {
                    if (*check != '.' && !isdigit((unsigned char)*check)) { is_num = 0; break; }
                    check++;
                }
                size_t ilen = (size_t)(ie - item);
                int is_bool = (ilen == 4 && strncmp(item, "true", 4) == 0) ||
                              (ilen == 5 && strncmp(item, "false", 5) == 0) ||
                              (ilen == 4 && strncmp(item, "null", 4) == 0);
                if (is_num || is_bool) {
                    strbuf_appendf(&ob, "%.*s", (int)ilen, item);
                } else {
                    /* Strip quotes if present */
                    if (ilen >= 2 && ((*item == '"' && *(ie - 1) == '"') ||
                                      (*item == '\'' && *(ie - 1) == '\''))) {
                        item++; ie--; ilen -= 2;
                    }
                    strbuf_appendf(&ob, "\"%.*s\"", (int)ilen, item);
                }
            }
            p = skip_newline(eol);
        }
        strbuf_appendf(&ob, "]");
    } else {
        /* Mapping */
        strbuf_appendf(&ob, "{");
        int first = 1;
        while (*p) {
            const char *line = skip_inline_ws(p);
            const char *eol = find_eol(line);
            if (line >= eol) { p = skip_newline(eol); continue; }

            /* Find colon */
            const char *colon = line;
            while (colon < eol && *colon != ':') colon++;
            if (colon >= eol) { p = skip_newline(eol); continue; }

            if (!first) strbuf_appendf(&ob, ",");
            first = 0;

            /* Key */
            const char *ke = colon;
            while (ke > line && (*(ke - 1) == ' ' || *(ke - 1) == '\t')) ke--;
            strbuf_appendf(&ob, "\"%.*s\":", (int)(ke - line), line);

            /* Value */
            const char *vs = skip_inline_ws(colon + 1);
            const char *ve = eol;
            while (ve > vs && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) ve--;
            /* Strip quotes */
            size_t vlen = (size_t)(ve - vs);
            if (vlen >= 2 && ((*vs == '"' && *(ve - 1) == '"') ||
                              (*vs == '\'' && *(ve - 1) == '\''))) {
                vs++; ve--; vlen -= 2;
            }
            int is_num = 1;
            const char *check = vs;
            if (check < ve && *check == '-') check++;
            if (check == ve) is_num = 0;
            while (check < ve) {
                if (*check != '.' && !isdigit((unsigned char)*check)) { is_num = 0; break; }
                check++;
            }
            int is_bool = (vlen == 4 && strncmp(vs, "true", 4) == 0) ||
                          (vlen == 5 && strncmp(vs, "false", 5) == 0) ||
                          (vlen == 4 && strncmp(vs, "null", 4) == 0);
            if (is_num || is_bool) {
                strbuf_appendf(&ob, "%.*s", (int)vlen, vs);
            } else {
                strbuf_appendf(&ob, "\"%.*s\"", (int)vlen, vs);
            }

            p = skip_newline(eol);
        }
        strbuf_appendf(&ob, "}");
    }

    return seal_buf(&ob, 0, "{}");
}
