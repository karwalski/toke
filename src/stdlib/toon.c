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
#include <stdlib.h>
#include <string.h>
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
 * Parse schema header: name[count]{field1,field2,...}:
 * or:                  {field1,field2,...}:
 *
 * Returns field names via fields/nfields, row count via rowcount.
 * Advances *pp past the colon + newline.
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
        if (*p == ']') p++;
    }

    /* Required {field1,field2,...} */
    if (*p != '{') return 0;
    p++;

    /* Count commas to determine field count */
    int count = 1;
    const char *scan = p;
    while (*scan && *scan != '}') {
        if (*scan == ',') count++;
        scan++;
    }
    if (*scan != '}') return 0;

    char **flds = (char **)malloc((size_t)count * sizeof(char *));
    const char *start = p;
    int idx = 0;
    while (*p && *p != '}') {
        if (*p == ',' || *(p + 1) == '}' || *p == '}') {
            /* handle last field */
            if (*p == ',') {
                size_t len = (size_t)(p - start);
                flds[idx] = (char *)malloc(len + 1);
                memcpy(flds[idx], start, len);
                flds[idx][len] = '\0';
                idx++;
                start = p + 1;
            }
            if (*p == '}') break;
        }
        p++;
    }
    /* Last field (between last comma/start and '}') */
    if (idx < count && *p == '}') {
        size_t len = (size_t)(p - start);
        flds[idx] = (char *)malloc(len + 1);
        memcpy(flds[idx], start, len);
        flds[idx][len] = '\0';
        idx++;
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
        if (strcmp(fields[i], key) == 0) {
            field_idx = i;
        }
        free(fields[i]);
    }
    free(fields);

    if (field_idx < 0) return 0;

    /* Navigate to the correct pipe-delimited field in the first data row */
    int cur = 0;
    const char *vs = p;
    while (cur < field_idx && *vs && *vs != '\n') {
        if (*vs == '|') cur++;
        vs++;
    }
    const char *ve = vs;
    while (*ve && *ve != '|' && *ve != '\n' && *ve != '\0') ve++;

    *val_start = vs;
    *val_end = ve;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* toon_enc — encode a value to TOON string representation */
const char *toon_enc(const char *v) {
    if (!v) return "";
    /* For simple values, TOON is just the raw value (no quotes needed) */
    size_t n = strlen(v);
    char *buf = (char *)malloc(n + 1);
    if (!buf) return "";
    memcpy(buf, v, n);
    buf[n] = '\0';
    return buf;
}

/* toon_dec — validate and store raw TOON string */
ToonResult toon_dec(const char *s) {
    ToonResult r;
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

/* toon_str — extract a string value for key */
StrToonResult toon_str(Toon t, const char *key) {
    StrToonResult r;
    const char *vs, *ve;
    if (!find_toon_field(t.raw, key, &vs, &ve)) {
        r.is_err = 1; r.err = make_err(TOON_ERR_MISSING, key); return r;
    }
    size_t len = (size_t)(ve - vs);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "oom"); return r; }
    memcpy(buf, vs, len);
    buf[len] = '\0';
    r.is_err = 0; r.ok = buf; return r;
}

/* toon_i64 — extract an integer value for key */
I64ToonResult toon_i64(Toon t, const char *key) {
    I64ToonResult r;
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
    F64ToonResult r;
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
    BoolToonResult r;
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
    ToonArrayResult r;
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
        free(fields[i]);
    }
    free(fields);

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

    if (rowcount == 0) rowcount = actual_rows;

    Toon *arr = (Toon *)malloc((size_t)rowcount * sizeof(Toon));
    if (!arr && rowcount > 0) {
        r.is_err = 1; r.err = make_err(TOON_ERR_PARSE, "oom"); return r;
    }

    /* Extract field from each row */
    rp = p;
    for (int row = 0; row < rowcount && *rp; row++) {
        const char *line_start = rp;
        while (*rp && *rp != '\n') rp++;
        /* Find field_idx-th pipe-delimited value */
        const char *fp = line_start;
        int cur = 0;
        while (cur < field_idx && fp < rp) {
            if (*fp == '|') cur++;
            fp++;
        }
        const char *fe = fp;
        while (fe < rp && *fe != '|') fe++;
        size_t len = (size_t)(fe - fp);
        char *val = (char *)malloc(len + 1);
        memcpy(val, fp, len);
        val[len] = '\0';
        arr[row].raw = val;
        if (*rp == '\n') rp++;
    }

    r.is_err = 0;
    r.ok.data = arr;
    r.ok.len = (uint64_t)rowcount;
    return r;
}

/* ------------------------------------------------------------------ */
/* JSON <-> TOON conversion                                             */
/* ------------------------------------------------------------------ */

/*
 * toon_from_json — convert a JSON array of objects to TOON format.
 *
 * Input:  [{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}]
 * Output: data[2]{id,name}:\n1|Alice\n2|Bob\n
 */
const char *toon_from_json(const char *json) {
    if (!json) return "";
    const char *p = json;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p != '[') {
        /* Single object: wrap as one-row TOON */
        if (*p != '{') return "";
        /* Extract keys from the object */
        size_t cap = 4096;
        char *out = (char *)malloc(cap);
        if (!out) return "";
        int pos = 0;

        /* Collect keys */
        const char *kp = p + 1;
        pos += snprintf(out + pos, cap - (size_t)pos, "{");
        int first_key = 1;
        while (*kp && *kp != '}') {
            while (*kp == ' ' || *kp == '\t' || *kp == '\n' || *kp == '\r') kp++;
            if (*kp != '"') break;
            kp++; /* skip opening quote */
            const char *ks = kp;
            while (*kp && *kp != '"') kp++;
            if (!first_key) pos += snprintf(out + pos, cap - (size_t)pos, ",");
            pos += snprintf(out + pos, cap - (size_t)pos, "%.*s", (int)(kp - ks), ks);
            first_key = 0;
            if (*kp == '"') kp++;
            while (*kp == ' ' || *kp == ':') kp++;
            /* skip value */
            if (*kp == '"') {
                kp++;
                while (*kp && !(*kp == '"' && *(kp - 1) != '\\')) kp++;
                if (*kp == '"') kp++;
            } else {
                while (*kp && *kp != ',' && *kp != '}') kp++;
            }
            if (*kp == ',') kp++;
        }
        pos += snprintf(out + pos, cap - (size_t)pos, "}:\n");

        /* Collect values */
        kp = p + 1;
        int first_val = 1;
        while (*kp && *kp != '}') {
            while (*kp == ' ' || *kp == '\t' || *kp == '\n' || *kp == '\r') kp++;
            if (*kp != '"') break;
            kp++;
            while (*kp && *kp != '"') kp++;
            if (*kp == '"') kp++;
            while (*kp == ' ' || *kp == ':') kp++;
            if (!first_val) pos += snprintf(out + pos, cap - (size_t)pos, "|");
            first_val = 0;
            if (*kp == '"') {
                kp++;
                const char *vs = kp;
                while (*kp && !(*kp == '"' && *(kp - 1) != '\\')) kp++;
                pos += snprintf(out + pos, cap - (size_t)pos, "%.*s", (int)(kp - vs), vs);
                if (*kp == '"') kp++;
            } else {
                const char *vs = kp;
                while (*kp && *kp != ',' && *kp != '}') kp++;
                /* trim trailing whitespace */
                const char *ve = kp;
                while (ve > vs && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) ve--;
                pos += snprintf(out + pos, cap - (size_t)pos, "%.*s", (int)(ve - vs), vs);
            }
            if (*kp == ',') kp++;
        }
        pos += snprintf(out + pos, cap - (size_t)pos, "\n");
        out[pos] = '\0';
        return out;
    }

    /* Array of objects */
    size_t cap = 8192;
    char *out = (char *)malloc(cap);
    if (!out) return "";
    int pos = 0;

    /* Count elements and extract keys from first object */
    p++; /* skip '[' */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '{') { free(out); return ""; }

    /* Extract keys from first object */
    char *keys[64];
    int nkeys = 0;
    const char *kp = p + 1;
    while (*kp && *kp != '}' && nkeys < 64) {
        while (*kp == ' ' || *kp == '\t' || *kp == '\n' || *kp == '\r') kp++;
        if (*kp != '"') break;
        kp++;
        const char *ks = kp;
        while (*kp && *kp != '"') kp++;
        size_t klen = (size_t)(kp - ks);
        keys[nkeys] = (char *)malloc(klen + 1);
        memcpy(keys[nkeys], ks, klen);
        keys[nkeys][klen] = '\0';
        nkeys++;
        if (*kp == '"') kp++;
        while (*kp == ' ' || *kp == ':') kp++;
        /* skip value */
        if (*kp == '"') {
            kp++;
            while (*kp && !(*kp == '"' && *(kp - 1) != '\\')) kp++;
            if (*kp == '"') kp++;
        } else {
            while (*kp && *kp != ',' && *kp != '}') kp++;
        }
        if (*kp == ',') kp++;
    }

    /* Count objects */
    int obj_count = 0;
    const char *cp = p;
    while (*cp) {
        if (*cp == '{') obj_count++;
        cp++;
    }

    /* Schema line */
    pos += snprintf(out + pos, cap - (size_t)pos, "data[%d]{", obj_count);
    for (int i = 0; i < nkeys; i++) {
        if (i > 0) pos += snprintf(out + pos, cap - (size_t)pos, ",");
        pos += snprintf(out + pos, cap - (size_t)pos, "%s", keys[i]);
    }
    pos += snprintf(out + pos, cap - (size_t)pos, "}:\n");

    /* Extract values from each object */
    const char *op = p;
    while (*op) {
        if (*op != '{') { op++; continue; }
        op++; /* skip '{' */
        int first = 1;
        for (int ki = 0; ki < nkeys; ki++) {
            /* Find this key in the object */
            const char *search = op;
            int found = 0;
            /* Simple linear key search in current object */
            (void)0; /* values extracted inline */
            while (*search && *search != '}') {
                while (*search == ' ' || *search == '\t' || *search == '\n' || *search == '\r') search++;
                if (*search != '"') break;
                search++;
                const char *ks = search;
                while (*search && *search != '"') search++;
                int match = ((int)(search - ks) == (int)strlen(keys[ki]) &&
                             strncmp(ks, keys[ki], strlen(keys[ki])) == 0);
                if (*search == '"') search++;
                while (*search == ' ' || *search == ':') search++;

                const char *vs = search;
                if (*search == '"') {
                    search++;
                    vs = search;
                    while (*search && !(*search == '"' && *(search - 1) != '\\')) search++;
                    if (match) {
                        if (!first) pos += snprintf(out + pos, cap - (size_t)pos, "|");
                        first = 0;
                        /* Grow buffer if needed */
                        size_t need = (size_t)(search - vs) + 16;
                        if ((size_t)pos + need >= cap) {
                            cap *= 2;
                            out = (char *)realloc(out, cap);
                        }
                        pos += snprintf(out + pos, cap - (size_t)pos, "%.*s",
                                        (int)(search - vs), vs);
                        found = 1;
                    }
                    if (*search == '"') search++;
                } else {
                    while (*search && *search != ',' && *search != '}') search++;
                    if (match) {
                        if (!first) pos += snprintf(out + pos, cap - (size_t)pos, "|");
                        first = 0;
                        const char *ve = search;
                        while (ve > vs && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) ve--;
                        size_t need = (size_t)(ve - vs) + 16;
                        if ((size_t)pos + need >= cap) {
                            cap *= 2;
                            out = (char *)realloc(out, cap);
                        }
                        pos += snprintf(out + pos, cap - (size_t)pos, "%.*s",
                                        (int)(ve - vs), vs);
                        found = 1;
                    }
                }
                if (found) break;
                if (*search == ',') search++;
            }
            if (!found) {
                if (!first) pos += snprintf(out + pos, cap - (size_t)pos, "|");
                first = 0;
            }
        }
        pos += snprintf(out + pos, cap - (size_t)pos, "\n");
        /* Advance past '}' */
        while (*op && *op != '}') op++;
        if (*op == '}') op++;
    }

    for (int i = 0; i < nkeys; i++) free(keys[i]);
    out[pos] = '\0';
    return out;
}

/*
 * toon_to_json — convert TOON format back to JSON array of objects.
 *
 * Input:  data[2]{id,name}:\n1|Alice\n2|Bob\n
 * Output: [{"id":1,"name":"Alice"},{"id":2,"name":"Bob"}]
 */
const char *toon_to_json(const char *toon_str) {
    if (!toon_str) return "[]";
    const char *p = toon_str;
    char **fields = NULL;
    int nfields = 0, rowcount = 0;

    if (!parse_schema(&p, &fields, &nfields, &rowcount)) return "[]";

    size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) {
        for (int i = 0; i < nfields; i++) free(fields[i]);
        free(fields);
        return "[]";
    }
    int pos = 0;
    pos += snprintf(out + pos, cap - (size_t)pos, "[");

    int row = 0;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        if (p == line_start) { if (*p == '\n') p++; continue; }

        if (row > 0) pos += snprintf(out + pos, cap - (size_t)pos, ",");
        pos += snprintf(out + pos, cap - (size_t)pos, "{");

        /* Parse pipe-delimited values */
        const char *vp = line_start;
        for (int fi = 0; fi < nfields; fi++) {
            if (fi > 0) pos += snprintf(out + pos, cap - (size_t)pos, ",");

            const char *vs = vp;
            while (vp < p && *vp != '|') vp++;
            const char *ve = vp;

            /* Grow buffer if needed */
            size_t need = (size_t)(ve - vs) + strlen(fields[fi]) + 16;
            if ((size_t)pos + need >= cap) {
                cap *= 2;
                out = (char *)realloc(out, cap);
            }

            /* Detect if value is numeric or boolean */
            int is_num = 1;
            int is_bool = 0;
            const char *check = vs;
            if (check < ve && *check == '-') check++;
            if (check == ve) is_num = 0;
            int has_dot = 0;
            while (check < ve) {
                if (*check == '.') { has_dot = 1; check++; continue; }
                if (!isdigit((unsigned char)*check)) { is_num = 0; break; }
                check++;
            }
            (void)has_dot;
            size_t vlen = (size_t)(ve - vs);
            if (vlen == 4 && strncmp(vs, "true", 4) == 0) is_bool = 1;
            if (vlen == 5 && strncmp(vs, "false", 5) == 0) is_bool = 1;
            if (vlen == 4 && strncmp(vs, "null", 4) == 0) is_bool = 1;

            pos += snprintf(out + pos, cap - (size_t)pos, "\"%s\":", fields[fi]);
            if (is_num || is_bool) {
                pos += snprintf(out + pos, cap - (size_t)pos, "%.*s", (int)(ve - vs), vs);
            } else {
                pos += snprintf(out + pos, cap - (size_t)pos, "\"%.*s\"", (int)(ve - vs), vs);
            }

            if (vp < p && *vp == '|') vp++;
        }

        pos += snprintf(out + pos, cap - (size_t)pos, "}");
        row++;
        if (*p == '\n') p++;
    }

    pos += snprintf(out + pos, cap - (size_t)pos, "]");
    out[pos] = '\0';

    for (int i = 0; i < nfields; i++) free(fields[i]);
    free(fields);
    return out;
}
