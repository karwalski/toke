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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

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
    size_t klen = strlen(key);
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
    if (needs_quote) {
        size_t n = strlen(v);
        char *buf = (char *)malloc(n + 3);
        if (!buf) return "\"\"";
        buf[0] = '"';
        memcpy(buf + 1, v, n);
        buf[n + 1] = '"';
        buf[n + 2] = '\0';
        return buf;
    }
    size_t n = strlen(v);
    char *buf = (char *)malloc(n + 1);
    if (!buf) return "";
    memcpy(buf, v, n);
    buf[n] = '\0';
    return buf;
}

/* yaml_dec — validate and store raw YAML string */
YamlResult yaml_dec(const char *s) {
    YamlResult r;
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
    StrYamlResult r;
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }
    size_t len = (size_t)(ve - vs);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { r.is_err = 1; r.err = make_err(YAML_ERR_PARSE, "oom"); return r; }
    memcpy(buf, vs, len);
    buf[len] = '\0';
    r.is_err = 0; r.ok = buf; return r;
}

/* yaml_i64 — extract an integer value for key */
I64YamlResult yaml_i64(Yaml y, const char *key) {
    I64YamlResult r;
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
    F64YamlResult r;
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
    BoolYamlResult r;
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

/* yaml_arr — extract a YAML sequence as YamlArray */
YamlArrayResult yaml_arr(Yaml y, const char *key) {
    YamlArrayResult r;
    const char *vs, *ve;
    if (!find_yaml_key(y.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(YAML_ERR_MISSING, key); return r;
    }

    /* Count sequence items (lines starting with "- ") */
    uint64_t count = 0;
    const char *p = vs;
    while (p < ve) {
        const char *line = skip_inline_ws(p);
        if (line < ve && *line == '-' && (*(line + 1) == ' ' || *(line + 1) == '\n')) {
            count++;
        }
        const char *eol = find_eol(line);
        p = skip_newline(eol);
    }

    Yaml *arr = (Yaml *)malloc(count * sizeof(Yaml));
    if (!arr && count > 0) {
        r.is_err = 1; r.err = make_err(YAML_ERR_PARSE, "oom"); return r;
    }

    /* Extract items */
    p = vs;
    uint64_t idx = 0;
    while (p < ve && idx < count) {
        const char *line = skip_inline_ws(p);
        if (line < ve && *line == '-') {
            const char *item = skip_inline_ws(line + 1);
            if (*item == ' ') item++;
            const char *eol = find_eol(item);
            /* Trim trailing whitespace */
            while (eol > item && (*(eol - 1) == ' ' || *(eol - 1) == '\t')) eol--;
            size_t len = (size_t)(eol - item);
            char *val = (char *)malloc(len + 1);
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
    r.ok.len = count;
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
        if (*p == '\\') { p += 2; continue; }
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

static const char *json_skip_value(const char *p) {
    p = json_skip_ws(p);
    if (*p == '"') return json_skip_string(p);
    if (*p == '{') return json_skip_object(p);
    if (*p == '[') return json_skip_array(p);
    if (*p == 't') return p + 4;
    if (*p == 'f') return p + 5;
    if (*p == 'n') return p + 4;
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

/* Recursive JSON-to-YAML with indentation */
static int json_to_yaml_r(const char **pp, char *out, int pos, int cap, int indent) {
    const char *p = json_skip_ws(*pp);

    if (*p == '{') {
        p++;
        p = json_skip_ws(p);
        if (*p == '}') { *pp = p + 1; pos += snprintf(out + pos, (size_t)(cap - pos), "{}"); return pos; }
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
            pos += snprintf(out + pos, (size_t)(cap - pos), "\n%*s%.*s: ",
                            indent, "", (int)(p - ks), ks);
            if (*p == '"') p++;
            p = json_skip_ws(p);
            if (*p == ':') p++;
            p = json_skip_ws(p);
            /* Value */
            pos = json_to_yaml_r(&p, out, pos, cap, indent + 2);
            p = json_skip_ws(p);
            if (*p == ',') p++;
        }
        if (*p == '}') p++;
        *pp = p;
        return pos;
    }

    if (*p == '[') {
        p++;
        p = json_skip_ws(p);
        if (*p == ']') { *pp = p + 1; pos += snprintf(out + pos, (size_t)(cap - pos), "[]"); return pos; }
        while (*p && *p != ']') {
            p = json_skip_ws(p);
            pos += snprintf(out + pos, (size_t)(cap - pos), "\n%*s- ", indent, "");
            pos = json_to_yaml_r(&p, out, pos, cap, indent + 2);
            p = json_skip_ws(p);
            if (*p == ',') p++;
        }
        if (*p == ']') p++;
        *pp = p;
        return pos;
    }

    if (*p == '"') {
        p++;
        const char *vs = p;
        while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
        pos += snprintf(out + pos, (size_t)(cap - pos), "%.*s", (int)(p - vs), vs);
        if (*p == '"') p++;
        *pp = p;
        return pos;
    }

    /* Number, boolean, null — copy literally */
    const char *start = p;
    p = json_skip_value(p);
    pos += snprintf(out + pos, (size_t)(cap - pos), "%.*s", (int)(p - start), start);
    *pp = p;
    return pos;
}

/*
 * yaml_from_json — convert JSON to YAML.
 */
const char *yaml_from_json(const char *json) {
    if (!json) return "";
    int cap = (int)strlen(json) * 2 + 256;
    char *out = (char *)malloc((size_t)cap);
    if (!out) return "";
    const char *p = json;
    int pos = json_to_yaml_r(&p, out, 0, cap, 0);
    /* Trim leading newline */
    if (pos > 0 && out[0] == '\n') {
        memmove(out, out + 1, (size_t)pos);
        pos--;
    }
    out[pos] = '\0';
    return out;
}

/*
 * yaml_to_json — convert simple YAML back to JSON.
 * Handles flat mappings and simple sequences.
 */
const char *yaml_to_json(const char *yaml_str) {
    if (!yaml_str) return "{}";
    const char *p = yaml_str;

    int cap = (int)strlen(yaml_str) * 2 + 256;
    char *out = (char *)malloc((size_t)cap);
    if (!out) return "{}";
    int pos = 0;

    /* Detect if top-level is a sequence or mapping */
    const char *first_line = skip_inline_ws(p);
    if (*first_line == '-') {
        /* Sequence */
        pos += snprintf(out + pos, (size_t)(cap - pos), "[");
        int first = 1;
        while (*p) {
            const char *line = skip_inline_ws(p);
            const char *eol = find_eol(line);
            if (line >= eol) { p = skip_newline(eol); continue; }

            if (*line == '-') {
                if (!first) pos += snprintf(out + pos, (size_t)(cap - pos), ",");
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
                    pos += snprintf(out + pos, (size_t)(cap - pos), "%.*s", (int)ilen, item);
                } else {
                    /* Strip quotes if present */
                    if (ilen >= 2 && ((*item == '"' && *(ie - 1) == '"') ||
                                      (*item == '\'' && *(ie - 1) == '\''))) {
                        item++; ie--; ilen -= 2;
                    }
                    pos += snprintf(out + pos, (size_t)(cap - pos), "\"%.*s\"", (int)ilen, item);
                }
            }
            p = skip_newline(eol);
        }
        pos += snprintf(out + pos, (size_t)(cap - pos), "]");
    } else {
        /* Mapping */
        pos += snprintf(out + pos, (size_t)(cap - pos), "{");
        int first = 1;
        while (*p) {
            const char *line = skip_inline_ws(p);
            const char *eol = find_eol(line);
            if (line >= eol) { p = skip_newline(eol); continue; }

            /* Find colon */
            const char *colon = line;
            while (colon < eol && *colon != ':') colon++;
            if (colon >= eol) { p = skip_newline(eol); continue; }

            if (!first) pos += snprintf(out + pos, (size_t)(cap - pos), ",");
            first = 0;

            /* Key */
            const char *ke = colon;
            while (ke > line && (*(ke - 1) == ' ' || *(ke - 1) == '\t')) ke--;
            pos += snprintf(out + pos, (size_t)(cap - pos), "\"%.*s\":", (int)(ke - line), line);

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
                pos += snprintf(out + pos, (size_t)(cap - pos), "%.*s", (int)vlen, vs);
            } else {
                pos += snprintf(out + pos, (size_t)(cap - pos), "\"%.*s\"", (int)vlen, vs);
            }

            p = skip_newline(eol);
        }
        pos += snprintf(out + pos, (size_t)(cap - pos), "}");
    }

    out[pos] = '\0';
    return out;
}
