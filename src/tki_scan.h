/*
 * tki_scan.h — structural iteration over a .tki interface's export records.
 *
 * Story 127.134a.  Seven scanners (llvm.c x4, names.c x2, glue_gen.c x1)
 * found each export record by `strstr(p, "\"kind\"")` and bounded it by the
 * NEXT `"kind"` substring.  A field whose NAME is `kind` — `{"name":"kind"}`
 * — is the string value `"kind"`, so it was taken for the start of the next
 * record and truncated this one: with such a field first in
 * `math.linregresult`, `tkc --check` accepted `.r2` and `tkc --out` rejected
 * it E4025.  The functions here walk the "exports" array brace by brace, so
 * a record's extent and its kind come from the JSON structure, never from a
 * token that happens to appear inside it.
 *
 * The .tki grammar is a small machine-generated JSON subset (no escaped
 * quotes in names or type spellings), so a quote- and depth-aware scan is
 * exact and keeps the frontend free of a JSON dependency (AGENTS.md §6).
 * These were names.c's private helpers (127.66); they live here so every
 * scanner shares one definition.  `static inline` so a translation unit that
 * uses only some of them draws no unused-function warning.
 */
#ifndef TKC_TKI_SCAN_H
#define TKC_TKI_SCAN_H

#include <string.h>

/* The position just past the JSON value starting at p, or NULL. */
static inline const char *tki_scan_value(const char *p, const char *end) {
    if (!p || p >= end) return NULL;
    if (*p == '"') {
        const char *q = (const char *)memchr(p + 1, '"', (size_t)(end - p - 1));
        return q ? q + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *q = (const char *)memchr(p + 1, '"',
                                                     (size_t)(end - p - 1));
                if (!q) return NULL;
                p = q + 1;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* Value position of `key` among the OWN keys of the object at [obj,end), or
 * NULL.  A nested object's keys are never considered. */
static inline const char *tki_obj_key(const char *obj, const char *end,
                                      const char *key) {
    if (!obj || obj >= end || *obj != '{') return NULL;
    size_t klen = strlen(key);
    const char *p = obj + 1;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                           *p == '\r' || *p == ',')) p++;
        if (p >= end || *p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *k1 = p + 1;
        const char *k2 = (const char *)memchr(k1, '"', (size_t)(end - k1));
        if (!k2) return NULL;
        p = k2 + 1;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (p >= end || *p != ':') return NULL;
        p++;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if ((size_t)(k2 - k1) == klen && memcmp(k1, key, klen) == 0) return p;
        p = tki_scan_value(p, end);
        if (!p) return NULL;
    }
    return NULL;
}

/* Position of the opening quote of `key` itself (not its value) among the
 * own keys of the object at [obj,end), or NULL.  For the scanners that read
 * `"key": "value"` onward from the key: a `strstr(p, "\"name\"")` from the
 * record start would land on a nested field's "name" if the record's own
 * key came later. */
static inline char *tki_keypos(const char *obj, const char *end, const char *key) {
    const char *v = tki_obj_key(obj, end, key);
    if (!v) return NULL;
    size_t klen = strlen(key);
    const char *q = v;
    while (q > obj && *q != ':') q--;          /* back over whitespace to ':' */
    while (q > obj && *q != '"') q--;          /* the key's closing quote */
    if ((size_t)(q - obj) < klen + 1) return NULL;
    return (char *)(q - klen - 1);             /* the key's opening quote */
}

/* Copy the quoted string starting at p into out; 1 on success. */
static inline int tki_str_value(const char *p, const char *end, char *out, int outsz) {
    if (!p) return 0;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '"') return 0;
    const char *q1 = p + 1;
    const char *q2 = (const char *)memchr(q1, '"', (size_t)(end - q1));
    if (!q2) return 0;
    int len = (int)(q2 - q1);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, q1, (size_t)len);
    out[len] = '\0';
    return 1;
}

/*
 * tki_next_record — iterate the records of the top-level "exports" array.
 *
 *   const char *cur = NULL, *rec, *rend; char kind[32];
 *   while (tki_next_record(buf, &cur, &rec, &rend, kind, sizeof kind)) ...
 *
 * On each call, sets *rec to the record's '{', *rend to one past its '}',
 * and copies its own "kind" value into kind (empty when it has none).
 * Returns 0 when the array is exhausted or malformed.  `*cur` is the
 * iterator state; start it at NULL.
 */
static inline int tki_next_record(const char *buf, const char **cur,
                                  const char **rec, const char **rend,
                                  char *kind, int kindsz) {
    if (!buf) return 0;
    const char *bufend = buf + strlen(buf);
    const char *p = *cur;
    if (!p) {
        /* The top-level object's own "exports" key, not a substring: a
         * record could legitimately contain the word. */
        const char *top = buf;
        while (top < bufend && *top != '{') top++;
        const char *ex = tki_obj_key(top, bufend, "exports");
        if (!ex || *ex != '[') return 0;
        p = ex + 1;
    }
    while (p < bufend && (*p == ' ' || *p == '\t' || *p == '\n' ||
                          *p == '\r' || *p == ',')) p++;
    if (p >= bufend || *p != '{') { *cur = bufend; return 0; }
    const char *e = tki_scan_value(p, bufend);
    if (!e) { *cur = bufend; return 0; }
    *rec = p;
    *rend = e;
    *cur = e;
    if (kind && kindsz > 0) {
        kind[0] = '\0';
        (void)tki_str_value(tki_obj_key(p, e, "kind"), e, kind, kindsz);
    }
    return 1;
}

#endif /* TKC_TKI_SCAN_H */
