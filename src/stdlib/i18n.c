/*
 * i18n.c — Implementation of std.i18n for the toke compiler runtime.
 *
 * Provides string externalisation and internationalisation.
 * Loads string bundles from TOON, YAML, or JSON files, resolves keys,
 * and substitutes {placeholder} tokens with provided arguments.
 *
 * Story: 6.3.5
 */

#include "i18n.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static I18nErr make_err(I18nErrKind kind, const char *msg) {
    I18nErr e; e.kind = kind; e.msg = msg; return e;
}

/* Read entire file into malloc'd string. Returns NULL on failure. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read] = '\0';
    return buf;
}

/* Try to open a file with the given extension. */
static char *try_load(const char *base, const char *locale, const char *ext) {
    char path[512];
    char *data;

    /* Try locale-specific first */
    if (locale && locale[0]) {
        snprintf(path, sizeof path, "%s.%s.%s", base, locale, ext);
        data = read_file(path);
        if (data) return data;
    }

    /* Fallback to non-locale */
    snprintf(path, sizeof path, "%s.%s", base, ext);
    data = read_file(path);
    return data;
}

/*
 * Detect format and find key=value.
 * Supports:
 *   TOON:  schema header + pipe-delimited rows with key|value
 *   YAML:  key: value lines
 *   JSON:  {"key":"value",...}
 */
static const char *find_string(const char *data, const char *key) {
    if (!data || !key) return NULL;
    size_t klen = strlen(key);

    /* Detect JSON */
    const char *p = data;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '{') {
        /* JSON: scan for "key":"value" */
        p++;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p != '"') break;
            p++;
            const char *ks = p;
            while (*p && *p != '"') p++;
            int match = ((size_t)(p - ks) == klen && strncmp(ks, key, klen) == 0);
            if (*p == '"') p++;
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                p++;
                const char *vs = p;
                while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
                if (match) {
                    size_t vlen = (size_t)(p - vs);
                    char *val = (char *)malloc(vlen + 1);
                    memcpy(val, vs, vlen);
                    val[vlen] = '\0';
                    return val;
                }
                if (*p == '"') p++;
            } else {
                /* Non-string value */
                const char *vs = p;
                while (*p && *p != ',' && *p != '}') p++;
                if (match) {
                    while (p > vs && (*(p - 1) == ' ' || *(p - 1) == '\t')) p--;
                    size_t vlen = (size_t)(p - vs);
                    char *val = (char *)malloc(vlen + 1);
                    memcpy(val, vs, vlen);
                    val[vlen] = '\0';
                    return val;
                }
            }
            if (*p == ',') p++;
        }
        return NULL;
    }

    /* Detect TOON: has '{' in first line and '}:' */
    const char *brace = strchr(data, '{');
    const char *colon = brace ? strchr(brace, ':') : NULL;
    if (brace && colon && *(colon - 1) == '}') {
        /* TOON format: find key/value field indices in schema */
        const char *sp = brace + 1;
        int key_idx = -1, val_idx = -1, fi = 0;
        while (*sp && *sp != '}') {
            const char *fs = sp;
            while (*sp && *sp != ',' && *sp != '}') sp++;
            size_t flen = (size_t)(sp - fs);
            if (flen == 3 && strncmp(fs, "key", 3) == 0) key_idx = fi;
            if (flen == 5 && strncmp(fs, "value", 5) == 0) val_idx = fi;
            fi++;
            if (*sp == ',') sp++;
        }
        if (key_idx < 0 || val_idx < 0) {
            /* No key/value fields — treat as YAML fallback */
            goto try_yaml;
        }
        /* Skip past schema line */
        p = colon + 1;
        if (*p == '\n') p++;
        /* Scan rows */
        while (*p) {
            const char *line = p;
            while (*p && *p != '\n') p++;
            /* Extract fields */
            const char *fp = line;
            const char *fields[16];
            const char *field_ends[16];
            int nf = 0;
            while (fp <= p && nf < 16) {  /* changed < to <= for last field */
                fields[nf] = fp;
                while (fp < p && *fp != '|') fp++;
                field_ends[nf] = fp;
                nf++;
                if (fp < p && *fp == '|') fp++;
            }
            if (key_idx < nf && val_idx < nf) {
                size_t kflen = (size_t)(field_ends[key_idx] - fields[key_idx]);
                if (kflen == klen && strncmp(fields[key_idx], key, klen) == 0) {
                    size_t vlen = (size_t)(field_ends[val_idx] - fields[val_idx]);
                    char *val = (char *)malloc(vlen + 1);
                    memcpy(val, fields[val_idx], vlen);
                    val[vlen] = '\0';
                    return val;
                }
            }
            if (*p == '\n') p++;
        }
        return NULL;
    }

try_yaml:
    /* YAML: key: value lines */
    p = data;
    while (*p) {
        const char *line = p;
        while (*p == ' ' || *p == '\t') p++;
        const char *content = p;
        while (*p && *p != '\n') p++;
        const char *eol = p;
        /* Check for "key: value" */
        if ((size_t)(eol - content) >= klen + 1 &&
            strncmp(content, key, klen) == 0 &&
            content[klen] == ':') {
            const char *vs = content + klen + 1;
            while (*vs == ' ' || *vs == '\t') vs++;
            const char *ve = eol;
            while (ve > vs && (*(ve - 1) == ' ' || *(ve - 1) == '\t')) ve--;
            /* Strip quotes */
            size_t vlen = (size_t)(ve - vs);
            if (vlen >= 2 && ((*vs == '"' && *(ve - 1) == '"') ||
                              (*vs == '\'' && *(ve - 1) == '\''))) {
                vs++; ve--; vlen -= 2;
            }
            char *val = (char *)malloc(vlen + 1);
            memcpy(val, vs, vlen);
            val[vlen] = '\0';
            return val;
        }
        (void)line;
        if (*p == '\n') p++;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

I18nBundleResult i18n_load(const char *base_path, const char *locale) {
    I18nBundleResult r;
    char *data;

    /* Try TOON first (default format), then YAML, then JSON */
    data = try_load(base_path, locale, "toon");
    if (!data) data = try_load(base_path, locale, "yaml");
    if (!data) data = try_load(base_path, locale, "yml");
    if (!data) data = try_load(base_path, locale, "json");

    if (!data) {
        r.is_err = 1;
        r.err = make_err(I18N_ERR_NOT_FOUND, base_path);
        return r;
    }

    r.is_err = 0;
    r.ok.data = data;
    return r;
}

const char *i18n_get(I18nBundle bundle, const char *key) {
    const char *val = find_string(bundle.data, key);
    if (val) return val;
    /* Safe fallback: return the key itself */
    size_t klen = strlen(key);
    char *copy = (char *)malloc(klen + 1);
    memcpy(copy, key, klen);
    copy[klen] = '\0';
    return copy;
}

const char *i18n_fmt(I18nBundle bundle, const char *key, const char *args) {
    const char *tmpl = find_string(bundle.data, key);
    if (!tmpl) {
        /* Fallback to key */
        size_t klen = strlen(key);
        char *copy = (char *)malloc(klen + 1);
        memcpy(copy, key, klen);
        copy[klen] = '\0';
        return copy;
    }

    /* Parse args: "name=value|name2=value2" */
    typedef struct { const char *name; size_t nlen; const char *val; size_t vlen; } Arg;
    Arg arg_list[32];
    int nargs = 0;

    if (args && *args) {
        const char *ap = args;
        while (*ap && nargs < 32) {
            const char *ns = ap;
            while (*ap && *ap != '=' && *ap != '|') ap++;
            if (*ap != '=') break;
            arg_list[nargs].name = ns;
            arg_list[nargs].nlen = (size_t)(ap - ns);
            ap++; /* skip '=' */
            const char *vs = ap;
            while (*ap && *ap != '|') ap++;
            arg_list[nargs].val = vs;
            arg_list[nargs].vlen = (size_t)(ap - vs);
            nargs++;
            if (*ap == '|') ap++;
        }
    }

    /* Substitute {placeholder} tokens */
    size_t tlen = strlen(tmpl);
    size_t cap = tlen * 2 + 256;
    char *out = (char *)malloc(cap);
    if (!out) return tmpl;
    size_t pos = 0;

    const char *tp = tmpl;
    while (*tp) {
        if (*tp == '{') {
            const char *ps = tp + 1;
            const char *pe = ps;
            while (*pe && *pe != '}') pe++;
            if (*pe == '}') {
                size_t plen = (size_t)(pe - ps);
                int found = 0;
                for (int i = 0; i < nargs; i++) {
                    if (arg_list[i].nlen == plen &&
                        strncmp(arg_list[i].name, ps, plen) == 0) {
                        if (pos + arg_list[i].vlen >= cap) {
                            cap *= 2;
                            out = (char *)realloc(out, cap);
                        }
                        memcpy(out + pos, arg_list[i].val, arg_list[i].vlen);
                        pos += arg_list[i].vlen;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    /* Keep placeholder as-is */
                    size_t chunk = plen + 2;
                    if (pos + chunk >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
                    memcpy(out + pos, tp, chunk);
                    pos += chunk;
                }
                tp = pe + 1;
                continue;
            }
        }
        if (pos + 1 >= cap) { cap *= 2; out = (char *)realloc(out, cap); }
        out[pos++] = *tp++;
    }
    out[pos] = '\0';
    return out;
}

const char *i18n_locale(void) {
    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LANG");
    if (!loc || !*loc) loc = "en";
    /* Extract language code (strip encoding like .UTF-8) */
    size_t len = strlen(loc);
    const char *dot = strchr(loc, '.');
    if (dot) len = (size_t)(dot - loc);
    char *result = (char *)malloc(len + 1);
    memcpy(result, loc, len);
    result[len] = '\0';
    return result;
}
