/*
 * template.c — Implementation of the std.template standard library module.
 *
 * Implements {{IDENT}} slot-based string templating.  Each compiled template
 * is stored as an array of segments; each segment is either a literal string
 * or a slot name.  Rendering walks the segment list, copies literals verbatim,
 * and looks up slot names in the caller-supplied variable bindings.
 *
 * Slot syntax: {{IDENT}} where IDENT matches [A-Za-z_][A-Za-z0-9_]*
 * Any {{ not followed by a valid IDENT and closing }} is treated as a literal.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary.
 * Callers own returned strings.
 *
 * Story: 15.1.4
 */

#include "template.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal segment representation
 * ----------------------------------------------------------------------- */

typedef enum {
    SEG_LITERAL,   /* fixed text to copy verbatim */
    SEG_SLOT       /* {{IDENT}} variable slot */
} SegKind;

typedef struct {
    SegKind  kind;
    char    *text;  /* heap-allocated: literal text or slot name */
} Segment;

struct TkTmpl {
    Segment  *segs;
    uint64_t  nseg;
};

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Return non-zero if c is valid as the first character of an IDENT. */
static int ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/* Return non-zero if c is valid as a continuation character of an IDENT. */
static int ident_cont(char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

/*
 * try_parse_slot — attempt to parse a {{IDENT}} slot starting at src[0].
 * src must already point past the opening "{{".
 * On success writes the slot name (NUL-terminated) into *out_name (caller
 * must free) and returns the number of characters consumed from src
 * (including the closing "}}").
 * On failure returns 0 and sets *out_name to NULL.
 */
static size_t try_parse_slot(const char *src, char **out_name)
{
    *out_name = NULL;
    if (!ident_start(src[0])) return 0;

    size_t n = 1;
    while (src[n] && ident_cont(src[n])) n++;

    /* must be followed by }} */
    if (src[n] != '}' || src[n + 1] != '}') return 0;

    char *name = (char *)malloc(n + 1);
    if (!name) return 0;
    memcpy(name, src, n);
    name[n] = '\0';
    *out_name = name;
    return n + 2; /* consume IDENT + }} */
}

/* -----------------------------------------------------------------------
 * tmpl_compile
 * ----------------------------------------------------------------------- */

TkTmpl *tmpl_compile(const char *source)
{
    if (!source) source = "";

    /* ---- first pass: count segments ---- */
    uint64_t nseg = 0;
    const char *p = source;
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            char *name = NULL;
            size_t consumed = try_parse_slot(p + 2, &name);
            if (consumed > 0) {
                free(name);
                nseg++;          /* slot segment */
                p += 2 + consumed;
                continue;
            }
            /* not a valid slot — fall through to literal accumulation */
        }
        /* start of a literal segment: advance until next {{ or end */
        nseg++;
        while (*p) {
            if (p[0] == '{' && p[1] == '{') {
                char *name = NULL;
                size_t consumed = try_parse_slot(p + 2, &name);
                if (consumed > 0) { free(name); break; }
            }
            p++;
        }
    }

    TkTmpl *t = (TkTmpl *)malloc(sizeof(TkTmpl));
    if (!t) return NULL;

    if (nseg == 0) {
        t->segs = NULL;
        t->nseg = 0;
        return t;
    }

    t->segs = (Segment *)malloc(nseg * sizeof(Segment));
    if (!t->segs) { free(t); return NULL; }
    t->nseg = nseg;

    /* ---- second pass: fill segments ---- */
    uint64_t i = 0;
    p = source;
    while (*p && i < nseg) {
        if (p[0] == '{' && p[1] == '{') {
            char *name = NULL;
            size_t consumed = try_parse_slot(p + 2, &name);
            if (consumed > 0) {
                t->segs[i].kind = SEG_SLOT;
                t->segs[i].text = name;
                i++;
                p += 2 + consumed;
                continue;
            }
        }
        /* literal segment */
        const char *lit_start = p;
        while (*p) {
            if (p[0] == '{' && p[1] == '{') {
                char *name = NULL;
                size_t consumed = try_parse_slot(p + 2, &name);
                if (consumed > 0) { free(name); break; }
            }
            p++;
        }
        size_t lit_len = (size_t)(p - lit_start);
        char *lit = (char *)malloc(lit_len + 1);
        if (!lit) {
            /* clean up already-allocated segments then bail */
            for (uint64_t k = 0; k < i; k++) free(t->segs[k].text);
            free(t->segs);
            free(t);
            return NULL;
        }
        memcpy(lit, lit_start, lit_len);
        lit[lit_len] = '\0';
        t->segs[i].kind = SEG_LITERAL;
        t->segs[i].text = lit;
        i++;
    }

    return t;
}

/* -----------------------------------------------------------------------
 * tmpl_free
 * ----------------------------------------------------------------------- */

void tmpl_free(TkTmpl *t)
{
    if (!t) return;
    for (uint64_t i = 0; i < t->nseg; i++) free(t->segs[i].text);
    free(t->segs);
    free(t);
}

/* -----------------------------------------------------------------------
 * tmpl_escape
 * ----------------------------------------------------------------------- */

const char *tmpl_escape(const char *s)
{
    if (!s) s = "";

    /* calculate required length */
    size_t len = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&':  len += 5; break; /* &amp; */
            case '<':  len += 4; break; /* &lt;  */
            case '>':  len += 4; break; /* &gt;  */
            case '"':  len += 6; break; /* &quot; */
            case '\'': len += 5; break; /* &#39; */
            default:   len += 1; break;
        }
    }

    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;

    char *q = out;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&':  memcpy(q, "&amp;",  5); q += 5; break;
            case '<':  memcpy(q, "&lt;",   4); q += 4; break;
            case '>':  memcpy(q, "&gt;",   4); q += 4; break;
            case '"':  memcpy(q, "&quot;", 6); q += 6; break;
            case '\'': memcpy(q, "&#39;",  5); q += 5; break;
            default:   *q++ = *p;              break;
        }
    }
    *q = '\0';
    return out;
}

/* -----------------------------------------------------------------------
 * Internal render helper
 * ----------------------------------------------------------------------- */

/*
 * lookup_var — linear scan of vars[0..nvar-1] for key.
 * Returns the matching value, or "" if not found.
 */
static const char *lookup_var(const TkTmplVar *vars, uint64_t nvar,
                               const char *key)
{
    if (!vars || !key) return "";
    for (uint64_t i = 0; i < nvar; i++) {
        if (vars[i].key && strcmp(vars[i].key, key) == 0)
            return vars[i].value ? vars[i].value : "";
    }
    return "";
}

/*
 * render_internal — shared render loop.
 * escape_values: if non-zero, HTML-escape slot values before appending.
 */
static const char *render_internal(TkTmpl *t, const TkTmplVar *vars,
                                    uint64_t nvar, int escape_values)
{
    if (!t) return NULL;

    /* ---- calculate output length ---- */
    size_t total = 0;
    for (uint64_t i = 0; i < t->nseg; i++) {
        Segment *s = &t->segs[i];
        if (s->kind == SEG_LITERAL) {
            total += strlen(s->text);
        } else {
            const char *val = lookup_var(vars, nvar, s->text);
            if (escape_values) {
                /* count escaped length */
                for (const char *p = val; *p; p++) {
                    switch (*p) {
                        case '&':  total += 5; break;
                        case '<':  total += 4; break;
                        case '>':  total += 4; break;
                        case '"':  total += 6; break;
                        case '\'': total += 5; break;
                        default:   total += 1; break;
                    }
                }
            } else {
                total += strlen(val);
            }
        }
    }

    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;

    char *q = out;
    for (uint64_t i = 0; i < t->nseg; i++) {
        Segment *s = &t->segs[i];
        if (s->kind == SEG_LITERAL) {
            size_t len = strlen(s->text);
            memcpy(q, s->text, len);
            q += len;
        } else {
            const char *val = lookup_var(vars, nvar, s->text);
            if (escape_values) {
                for (const char *p = val; *p; p++) {
                    switch (*p) {
                        case '&':  memcpy(q, "&amp;",  5); q += 5; break;
                        case '<':  memcpy(q, "&lt;",   4); q += 4; break;
                        case '>':  memcpy(q, "&gt;",   4); q += 4; break;
                        case '"':  memcpy(q, "&quot;", 6); q += 6; break;
                        case '\'': memcpy(q, "&#39;",  5); q += 5; break;
                        default:   *q++ = *p;              break;
                    }
                }
            } else {
                size_t len = strlen(val);
                memcpy(q, val, len);
                q += len;
            }
        }
    }
    *q = '\0';
    return out;
}

/* -----------------------------------------------------------------------
 * tmpl_render / tmpl_renderhtml
 * ----------------------------------------------------------------------- */

const char *tmpl_render(TkTmpl *t, const TkTmplVar *vars, uint64_t nvar)
{
    return render_internal(t, vars, nvar, 0);
}

const char *tmpl_renderhtml(TkTmpl *t, const TkTmplVar *vars, uint64_t nvar)
{
    return render_internal(t, vars, nvar, 1);
}

/* -----------------------------------------------------------------------
 * tmpl_vars
 * ----------------------------------------------------------------------- */

TkTmplVar *tmpl_vars(const char *const *keys, const char *const *values,
                      uint64_t nvar)
{
    if (nvar == 0) return NULL;
    if (!keys || !values) return NULL;

    TkTmplVar *out = (TkTmplVar *)malloc(nvar * sizeof(TkTmplVar));
    if (!out) return NULL;

    for (uint64_t i = 0; i < nvar; i++) {
        out[i].key   = keys[i];
        out[i].value = values[i];
    }
    return out;
}

/* -----------------------------------------------------------------------
 * tmpl_html
 * ----------------------------------------------------------------------- */

const char *tmpl_html(const char *tag,
                      const char *const *attr_keys,
                      const char *const *attr_values,
                      uint64_t nattr,
                      const char *const *children,
                      uint64_t nchild)
{
    if (!tag) return NULL;

    size_t tag_len = strlen(tag);

    /* Calculate total output length. */
    /* <tag attr="val" attr="val">children</tag> */
    size_t total = 1 + tag_len; /* opening < + tag */

    for (uint64_t i = 0; i < nattr; i++) {
        if (!attr_keys || !attr_values) break;
        const char *ak = attr_keys[i]   ? attr_keys[i]   : "";
        const char *av = attr_values[i]  ? attr_values[i] : "";
        /* space + key + =" + escaped_value + " */
        total += 1 + strlen(ak) + 2;
        for (const char *p = av; *p; p++) {
            switch (*p) {
                case '&':  total += 5; break;
                case '<':  total += 4; break;
                case '>':  total += 4; break;
                case '"':  total += 6; break;
                case '\'': total += 5; break;
                default:   total += 1; break;
            }
        }
        total += 1; /* closing quote */
    }

    total += 1; /* > after opening tag */

    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i])
            total += strlen(children[i]);
    }

    total += 2 + tag_len + 1; /* </ + tag + > */

    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;

    char *q = out;

    /* opening tag */
    *q++ = '<';
    memcpy(q, tag, tag_len);
    q += tag_len;

    /* attributes */
    for (uint64_t i = 0; i < nattr; i++) {
        if (!attr_keys || !attr_values) break;
        const char *ak = attr_keys[i]   ? attr_keys[i]   : "";
        const char *av = attr_values[i]  ? attr_values[i] : "";
        *q++ = ' ';
        size_t ak_len = strlen(ak);
        memcpy(q, ak, ak_len);
        q += ak_len;
        *q++ = '=';
        *q++ = '"';
        for (const char *p = av; *p; p++) {
            switch (*p) {
                case '&':  memcpy(q, "&amp;",  5); q += 5; break;
                case '<':  memcpy(q, "&lt;",   4); q += 4; break;
                case '>':  memcpy(q, "&gt;",   4); q += 4; break;
                case '"':  memcpy(q, "&quot;", 6); q += 6; break;
                case '\'': memcpy(q, "&#39;",  5); q += 5; break;
                default:   *q++ = *p;              break;
            }
        }
        *q++ = '"';
    }

    *q++ = '>';

    /* children */
    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i]) {
            size_t clen = strlen(children[i]);
            memcpy(q, children[i], clen);
            q += clen;
        }
    }

    /* closing tag */
    *q++ = '<';
    *q++ = '/';
    memcpy(q, tag, tag_len);
    q += tag_len;
    *q++ = '>';
    *q = '\0';

    return out;
}

/* -----------------------------------------------------------------------
 * tmpl_renderfile
 * ----------------------------------------------------------------------- */

const char *tmpl_renderfile(const char *path, const TkTmplVar *vars,
                            uint64_t nvar)
{
    if (!path) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    /* determine file size */
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long fsize = ftell(fp);
    if (fsize < 0) { fclose(fp); return NULL; }
    rewind(fp);

    char *src = (char *)malloc((size_t)fsize + 1);
    if (!src) { fclose(fp); return NULL; }

    size_t nread = fread(src, 1, (size_t)fsize, fp);
    fclose(fp);
    src[nread] = '\0';

    TkTmpl *t = tmpl_compile(src);
    free(src);
    if (!t) return NULL;

    const char *result = tmpl_render(t, vars, nvar);
    tmpl_free(t);
    return result;
}
