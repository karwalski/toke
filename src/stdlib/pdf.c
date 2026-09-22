/*
 * pdf.c — object model, file index, filters and page tree for std.pdf.
 *
 * The text extraction itself lives in pdftext.c; this file is everything
 * needed to hand it a decoded content stream and a /Resources dictionary.
 *
 * ── HOW OBJECTS ARE FOUND, AND WHY NOT THROUGH THE XREF ─────────────────
 *
 * The obvious route is to read the startxref pointer, parse the
 * cross-reference table (or, since PDF 1.5, the cross-reference STREAM),
 * follow the /Prev chain, and look each object up by offset.  This file
 * does something else: it walks the file from the front, parses every
 * `N G obj ... endobj` it meets, and keeps the LAST definition of each
 * object number.  The trailer dictionaries and /Type /XRef stream dicts are
 * still read, but only for /Root and /Encrypt.
 *
 * That is a deliberate trade and it is worth being explicit about:
 *
 *   + It reads files whose xref is wrong, and there are a lot of them —
 *     every PDF truncated by a mail gateway, every one written by a
 *     producer that miscounted an offset.  An xref-driven reader returns
 *     "not a PDF" for a file every viewer opens, which is a wrong answer,
 *     not a safe one.
 *   + It handles classic tables and xref streams identically, because it
 *     never looks at either.
 *   + The sequential walk never scans INSIDE stream data — it parses each
 *     object and steps over the stream body — so a `12 0 obj` that happens
 *     to occur inside a compressed image cannot invent a phantom object.
 *     A naive "grep for N G obj" would; that is the trap this shape avoids.
 *   - It is O(file) at open where an xref lookup is O(1) per object.  For
 *     the documents this module exists for — statements, invoices, reports —
 *     that is milliseconds, and it is paid once.
 *   - In a file with incremental updates it takes the LAST definition in
 *     file order.  That is correct for real incremental updates, which
 *     append; it is wrong for a file whose xref deliberately points back at
 *     an earlier generation, which is rare and is usually a sign of
 *     tampering rather than of an honest document.
 *
 * Objects packed into an /ObjStm are not visible to that walk — they are
 * inside a compressed stream by construction — so every /Type /ObjStm found
 * is inflated and its contents indexed as well.  Skipping that step is how
 * a reader comes to report "0 pages" for every file a modern producer
 * writes, since the page tree itself is routinely packed.
 *
 * Story: 135.3
 */

#include "pdf.h"
#include "pdfobj.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

/* ── Error state ─────────────────────────────────────────────────────────
 *
 * Two fields, not one.  The message is prose for a human; the kind is a
 * closed set for a program.  Story 135.3 wants "encrypted" to be a distinct
 * answer, and distinguishing it by substring match on prose is exactly the
 * fragility this module exists to remove.
 */
static char s_errmsg[512];
static const char *s_errkind = TK_PDF_E_NONE;

void pdf_fail(const char *kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_errmsg, sizeof s_errmsg, fmt, ap);
    va_end(ap);
    s_errkind = kind;
}

const char *pdf_lasterr(void)      { return s_errmsg; }
const char *pdf_lasterr_kind(void) { return s_errkind; }

static void pdf_clearerr(void) { s_errmsg[0] = '\0'; s_errkind = TK_PDF_E_NONE; }

/* ── Document ───────────────────────────────────────────────────────── */

struct TkPdfDoc {
    uint8_t  *bytes;        /* the whole file; owned                      */
    size_t    len;

    void    **pool;         /* every allocation, freed in one sweep       */
    size_t    npool, cappool;

    PdfObj  **objs;         /* indexed by object number                   */
    uint32_t  nobjs;

    PdfObj   *trailer;      /* merged; carries /Root and /Encrypt         */
    PdfObj   *root;         /* the catalogue                              */

    PdfObj  **pages;        /* flattened page tree, in document order     */
    uint32_t  npages, cappages;
};

void *pdf_alloc(TkPdfDoc *doc, size_t n)
{
    void *p;
    if (doc->npool == doc->cappool) {
        size_t nc = doc->cappool ? doc->cappool * 2 : 256;
        void **np = (void **)realloc(doc->pool, nc * sizeof(void *));
        if (!np) return NULL;
        doc->pool = np;
        doc->cappool = nc;
    }
    p = calloc(1, n ? n : 1);
    if (!p) return NULL;
    doc->pool[doc->npool++] = p;
    return p;
}

PdfObj *pdf_newobj(TkPdfDoc *doc, PdfKind kind)
{
    PdfObj *o = (PdfObj *)pdf_alloc(doc, sizeof(PdfObj));
    if (o) o->kind = kind;
    return o;
}

/* ── Character classes ──────────────────────────────────────────────── */

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0';
}

static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static int is_reg(int c) { return !is_ws(c) && !is_delim(c); }

void pdf_skipws(PdfLex *L)
{
    while (L->i < L->n) {
        uint8_t c = L->p[L->i];
        if (is_ws(c)) { L->i++; continue; }
        if (c == '%') {                       /* comment to end of line */
            while (L->i < L->n && L->p[L->i] != '\n' && L->p[L->i] != '\r') L->i++;
            continue;
        }
        break;
    }
}

int pdf_starts_object(const PdfLex *L)
{
    uint8_t c;
    if (L->i >= L->n) return 0;
    c = L->p[L->i];
    return c == '/' || c == '(' || c == '<' || c == '[' ||
           c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9');
}

size_t pdf_token(PdfLex *L, char *buf, size_t bufsz)
{
    size_t k = 0;
    pdf_skipws(L);
    if (L->i >= L->n) { if (bufsz) buf[0] = '\0'; return 0; }
    /* A lone delimiter is its own token, so `]` and `>>` terminate cleanly. */
    if (is_delim(L->p[L->i])) {
        if (bufsz > 2 && L->p[L->i] == '>' && L->i + 1 < L->n && L->p[L->i + 1] == '>') {
            buf[0] = buf[1] = '>'; buf[2] = '\0'; L->i += 2; return 2;
        }
        if (bufsz > 1) { buf[0] = (char)L->p[L->i]; buf[1] = '\0'; }
        L->i++;
        return 1;
    }
    while (L->i < L->n && is_reg(L->p[L->i])) {
        if (k + 1 < bufsz) buf[k] = (char)L->p[L->i];
        k++;
        L->i++;
    }
    if (bufsz) buf[k < bufsz ? k : bufsz - 1] = '\0';
    return k;
}

/* ── Parsing ────────────────────────────────────────────────────────── */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* /Name#20with#20escapes -> "Name with escapes" */
static PdfObj *parse_name(TkPdfDoc *doc, PdfLex *L)
{
    size_t start, k = 0;
    char *out;
    PdfObj *o;
    L->i++;                                  /* the '/' */
    start = L->i;
    while (L->i < L->n && is_reg(L->p[L->i])) L->i++;
    out = (char *)pdf_alloc(doc, (L->i - start) + 1);
    if (!out) return NULL;
    for (size_t j = start; j < L->i; j++) {
        if (L->p[j] == '#' && j + 2 < L->i) {
            int h = hexval(L->p[j + 1]), l = hexval(L->p[j + 2]);
            if (h >= 0 && l >= 0) { out[k++] = (char)(h * 16 + l); j += 2; continue; }
        }
        out[k++] = (char)L->p[j];
    }
    out[k] = '\0';
    o = pdf_newobj(doc, PDF_NAME);
    if (o) o->u.name = out;
    return o;
}

/* (literal string) — balanced parens, backslash escapes, \ooo octal. */
static PdfObj *parse_lstring(TkPdfDoc *doc, PdfLex *L)
{
    size_t start = ++L->i, k = 0;
    int depth = 1;
    char *out;
    PdfObj *o;
    /* Upper bound on the decoded length is the raw length. */
    size_t j = start;
    while (j < L->n && depth > 0) {
        if (L->p[j] == '\\') { j += 2; continue; }
        if (L->p[j] == '(') depth++;
        else if (L->p[j] == ')') depth--;
        j++;
    }
    out = (char *)pdf_alloc(doc, (j - start) + 1);
    if (!out) return NULL;
    depth = 1;
    while (L->i < L->n) {
        uint8_t c = L->p[L->i++];
        if (c == '\\') {
            if (L->i >= L->n) break;
            c = L->p[L->i++];
            switch (c) {
            case 'n': out[k++] = '\n'; break;
            case 'r': out[k++] = '\r'; break;
            case 't': out[k++] = '\t'; break;
            case 'b': out[k++] = '\b'; break;
            case 'f': out[k++] = '\f'; break;
            case '\r':                        /* line continuation */
                if (L->i < L->n && L->p[L->i] == '\n') L->i++;
                break;
            case '\n': break;
            default:
                if (c >= '0' && c <= '7') {
                    int v = c - '0', d = 1;
                    while (d < 3 && L->i < L->n && L->p[L->i] >= '0' && L->p[L->i] <= '7') {
                        v = v * 8 + (L->p[L->i++] - '0');
                        d++;
                    }
                    out[k++] = (char)(v & 0xFF);
                } else {
                    out[k++] = (char)c;       /* \( \) \\ and anything else */
                }
            }
            continue;
        }
        if (c == '(') { depth++; out[k++] = '('; continue; }
        if (c == ')') { if (--depth == 0) break; out[k++] = ')'; continue; }
        out[k++] = (char)c;
    }
    out[k] = '\0';
    o = pdf_newobj(doc, PDF_STR);
    if (o) { o->u.str.p = out; o->u.str.n = k; }
    return o;
}

/* <48656C6C6F> — an odd final digit is padded with 0, as the spec says. */
static PdfObj *parse_hstring(TkPdfDoc *doc, PdfLex *L)
{
    size_t start = ++L->i, k = 0;
    char *out;
    PdfObj *o;
    size_t j = start;
    while (j < L->n && L->p[j] != '>') j++;
    out = (char *)pdf_alloc(doc, (j - start) / 2 + 2);
    if (!out) return NULL;
    {
        int hi = -1;
        while (L->i < L->n && L->p[L->i] != '>') {
            int v = hexval(L->p[L->i++]);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { out[k++] = (char)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) out[k++] = (char)(hi * 16);
    }
    if (L->i < L->n) L->i++;                  /* the '>' */
    out[k] = '\0';
    o = pdf_newobj(doc, PDF_STR);
    if (o) { o->u.str.p = out; o->u.str.n = k; }
    return o;
}

static int arr_push(TkPdfDoc *doc, PdfObj *a, PdfObj *v)
{
    if (a->u.arr.n == a->u.arr.cap) {
        uint32_t nc = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
        PdfObj **nv = (PdfObj **)pdf_alloc(doc, nc * sizeof(PdfObj *));
        if (!nv) return -1;
        if (a->u.arr.v) memcpy(nv, a->u.arr.v, a->u.arr.n * sizeof(PdfObj *));
        a->u.arr.v = nv;
        a->u.arr.cap = nc;
    }
    a->u.arr.v[a->u.arr.n++] = v;
    return 0;
}

static int dict_put(TkPdfDoc *doc, PdfObj *d, char *key, PdfObj *v)
{
    if (d->u.dict.n == d->u.dict.cap) {
        uint32_t nc = d->u.dict.cap ? d->u.dict.cap * 2 : 8;
        char  **nk = (char **)pdf_alloc(doc, nc * sizeof(char *));
        PdfObj **nv = (PdfObj **)pdf_alloc(doc, nc * sizeof(PdfObj *));
        if (!nk || !nv) return -1;
        if (d->u.dict.k) {
            memcpy(nk, d->u.dict.k, d->u.dict.n * sizeof(char *));
            memcpy(nv, d->u.dict.v, d->u.dict.n * sizeof(PdfObj *));
        }
        d->u.dict.k = nk;
        d->u.dict.v = nv;
        d->u.dict.cap = nc;
    }
    d->u.dict.k[d->u.dict.n] = key;
    d->u.dict.v[d->u.dict.n] = v;
    d->u.dict.n++;
    return 0;
}

/*
 * A number, or an indirect reference.
 *
 * `12 0 R` is three tokens and there is no way to know the first is part of
 * a reference until the third has been read, so this speculates and rewinds.
 * Getting it wrong the other way — treating `12 0 R` as the number 12 —
 * turns every /Length, /Root and /Kids entry into a small integer, which is
 * the kind of failure that produces an empty document rather than an error.
 */
static PdfObj *parse_number_or_ref(TkPdfDoc *doc, PdfLex *L)
{
    char tok[64];
    size_t save = L->i;
    double v;
    PdfObj *o;
    pdf_token(L, tok, sizeof tok);
    v = atof(tok);

    if (strchr(tok, '.') == NULL && tok[0] != '+' && tok[0] != '-' && v >= 0) {
        size_t after_first = L->i;
        char t2[64], t3[64];
        size_t n2 = pdf_token(L, t2, sizeof t2);
        if (n2 && t2[0] >= '0' && t2[0] <= '9' && !strchr(t2, '.')) {
            size_t n3 = pdf_token(L, t3, sizeof t3);
            if (n3 == 1 && t3[0] == 'R') {
                o = pdf_newobj(doc, PDF_REF);
                if (o) { o->u.ref.num = (int32_t)atol(tok); o->u.ref.gen = (int32_t)atol(t2); }
                return o;
            }
        }
        L->i = after_first;
    }
    o = pdf_newobj(doc, PDF_NUM);
    if (o) o->u.num = v;
    (void)save;
    return o;
}

PdfObj *pdf_parse(TkPdfDoc *doc, PdfLex *L, unsigned depth)
{
    uint8_t c;
    pdf_skipws(L);
    if (L->i >= L->n) return NULL;
    if (depth > TK_PDF_MAX_DEPTH) {
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: object nesting deeper than %u",
                 TK_PDF_MAX_DEPTH);
        return NULL;
    }
    c = L->p[L->i];

    if (c == '/') return parse_name(doc, L);
    if (c == '(') return parse_lstring(doc, L);
    if (c == '<') {
        if (L->i + 1 < L->n && L->p[L->i + 1] == '<') {
            PdfObj *d = pdf_newobj(doc, PDF_DICT);
            if (!d) return NULL;
            L->i += 2;
            for (;;) {
                pdf_skipws(L);
                if (L->i >= L->n) break;
                if (L->p[L->i] == '>') {
                    L->i++;
                    if (L->i < L->n && L->p[L->i] == '>') L->i++;
                    break;
                }
                if (L->p[L->i] != '/') {      /* junk key: resynchronise */
                    PdfObj *skip = pdf_parse(doc, L, depth + 1);
                    if (!skip) { char t[64]; if (!pdf_token(L, t, sizeof t)) break; }
                    continue;
                }
                {
                    PdfObj *k = parse_name(doc, L);
                    PdfObj *v = pdf_parse(doc, L, depth + 1);
                    if (!k) return NULL;
                    if (!v) { v = pdf_newobj(doc, PDF_NULL); if (!v) return NULL; }
                    if (dict_put(doc, d, k->u.name, v) != 0) return NULL;
                }
            }
            return d;
        }
        return parse_hstring(doc, L);
    }
    if (c == '[') {
        PdfObj *a = pdf_newobj(doc, PDF_ARR);
        if (!a) return NULL;
        L->i++;
        for (;;) {
            pdf_skipws(L);
            if (L->i >= L->n) break;
            if (L->p[L->i] == ']') { L->i++; break; }
            {
                PdfObj *v = pdf_parse(doc, L, depth + 1);
                if (!v) {
                    char t[64];
                    if (!pdf_token(L, t, sizeof t)) break;   /* keyword inside
                                                              * an array: skip */
                    continue;
                }
                if (arr_push(doc, a, v) != 0) return NULL;
            }
        }
        return a;
    }
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9'))
        return parse_number_or_ref(doc, L);

    /* true / false / null are objects; everything else is a keyword and is
     * the caller's to read. */
    if (c == 't' || c == 'f' || c == 'n') {
        size_t save = L->i;
        char tok[16];
        pdf_token(L, tok, sizeof tok);
        if (!strcmp(tok, "true") || !strcmp(tok, "false")) {
            PdfObj *o = pdf_newobj(doc, PDF_BOOL);
            if (o) o->u.b = tok[0] == 't';
            return o;
        }
        if (!strcmp(tok, "null")) return pdf_newobj(doc, PDF_NULL);
        L->i = save;
    }
    return NULL;
}

/* ── Accessors ──────────────────────────────────────────────────────── */

PdfObj *pdf_resolve(TkPdfDoc *doc, PdfObj *o)
{
    int hops = 0;
    while (o && o->kind == PDF_REF && hops++ < 32) {
        int32_t n = o->u.ref.num;
        if (!doc || n < 0 || (uint32_t)n >= doc->nobjs) return NULL;
        o = doc->objs[n];
    }
    return (o && o->kind == PDF_REF) ? NULL : o;
}

PdfObj *pdf_dget(TkPdfDoc *doc, PdfObj *d, const char *key)
{
    if (!d) return NULL;
    if (d->kind == PDF_STREAM) d = d->u.stream.dict;
    if (!d || d->kind != PDF_DICT) return NULL;
    for (uint32_t i = 0; i < d->u.dict.n; i++)
        if (d->u.dict.k[i] && !strcmp(d->u.dict.k[i], key))
            return pdf_resolve(doc, d->u.dict.v[i]);
    return NULL;
}

double pdf_num(PdfObj *o, double def) { return (o && o->kind == PDF_NUM) ? o->u.num : def; }

const char *pdf_name_of(PdfObj *o) { return (o && o->kind == PDF_NAME) ? o->u.name : NULL; }

int pdf_is_name(PdfObj *o, const char *name)
{
    const char *n = pdf_name_of(o);
    return n && !strcmp(n, name);
}

uint32_t pdf_arr_len(PdfObj *o) { return (o && o->kind == PDF_ARR) ? o->u.arr.n : 0; }

PdfObj *pdf_arr_get(TkPdfDoc *doc, PdfObj *o, uint32_t i)
{
    if (!o || o->kind != PDF_ARR || i >= o->u.arr.n) return NULL;
    return pdf_resolve(doc, o->u.arr.v[i]);
}

/* ── Filters ─────────────────────────────────────────────────────────── */

/*
 * inflate_stream — FlateDecode through zlib.
 *
 * The decoded size is not known in advance, so this grows the output rather
 * than trusting any length in the file.  Z_DATA_ERROR after some output is
 * NOT treated as failure: a truncated or slightly damaged stream is common
 * in the wild and the text decoded before the damage is real text.  A reader
 * that discards it returns an empty page for a document a viewer renders.
 *
 * raw deflate (no zlib header) is retried, because some producers omit it.
 */
static uint8_t *inflate_stream(TkPdfDoc *doc, const uint8_t *in, size_t inlen,
                               size_t *outlen)
{
    for (int window = 0; window < 2; window++) {
        z_stream zs;
        size_t cap = inlen < 4096 ? 16384 : inlen * 4;
        uint8_t *out;
        size_t have = 0;
        int rc;

        if (cap > TK_PDF_MAX_STREAM) cap = TK_PDF_MAX_STREAM;
        out = (uint8_t *)malloc(cap);
        if (!out) return NULL;

        memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, window == 0 ? 15 : -15) != Z_OK) { free(out); return NULL; }
        zs.next_in = (Bytef *)(uintptr_t)in;
        zs.avail_in = (uInt)inlen;

        for (;;) {
            zs.next_out = out + have;
            zs.avail_out = (uInt)(cap - have);
            rc = inflate(&zs, Z_NO_FLUSH);
            have = cap - zs.avail_out;
            if (rc == Z_STREAM_END || rc == Z_BUF_ERROR || rc < 0) break;
            if (zs.avail_out == 0) {
                uint8_t *np;
                size_t nc = cap * 2;
                if (nc > TK_PDF_MAX_STREAM) nc = TK_PDF_MAX_STREAM;
                if (nc == cap) {
                    pdf_fail(TK_PDF_E_TOOLARGE,
                             "pdf: stream inflates past the %u MiB cap",
                             TK_PDF_MAX_STREAM / (1024u * 1024u));
                    inflateEnd(&zs);
                    free(out);
                    return NULL;
                }
                np = (uint8_t *)realloc(out, nc);
                if (!np) { inflateEnd(&zs); free(out); return NULL; }
                out = np;
                cap = nc;
            }
        }
        inflateEnd(&zs);

        if (have > 0) {
            if (doc) {
                if (doc->npool == doc->cappool) {
                    size_t nc2 = doc->cappool ? doc->cappool * 2 : 256;
                    void **np2 = (void **)realloc(doc->pool, nc2 * sizeof(void *));
                    if (!np2) { free(out); return NULL; }
                    doc->pool = np2;
                    doc->cappool = nc2;
                }
                doc->pool[doc->npool++] = out;
            }
            *outlen = have;
            return out;
        }
        free(out);
    }
    return NULL;
}

/* ASCIIHexDecode — whitespace-tolerant, terminated by '>'. */
static uint8_t *ahx_decode(TkPdfDoc *doc, const uint8_t *in, size_t n, size_t *outlen)
{
    uint8_t *out = (uint8_t *)pdf_alloc(doc, n / 2 + 2);
    size_t k = 0;
    int hi = -1;
    if (!out) return NULL;
    for (size_t i = 0; i < n && in[i] != '>'; i++) {
        int v = hexval(in[i]);
        if (v < 0) continue;
        if (hi < 0) hi = v; else { out[k++] = (uint8_t)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) out[k++] = (uint8_t)(hi * 16);
    *outlen = k;
    return out;
}

/* ASCII85Decode — 'z' shorthand for four zero bytes, terminated by "~>". */
static uint8_t *a85_decode(TkPdfDoc *doc, const uint8_t *in, size_t n, size_t *outlen)
{
    uint8_t *out = (uint8_t *)pdf_alloc(doc, n * 4 / 5 + 8);
    size_t k = 0, grp = 0;
    uint32_t acc = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == '~') break;
        if (is_ws(c)) continue;
        if (c == 'z' && grp == 0) { out[k++] = 0; out[k++] = 0; out[k++] = 0; out[k++] = 0; continue; }
        if (c < '!' || c > 'u') continue;
        acc = acc * 85 + (uint32_t)(c - '!');
        if (++grp == 5) {
            out[k++] = (uint8_t)(acc >> 24); out[k++] = (uint8_t)(acc >> 16);
            out[k++] = (uint8_t)(acc >> 8);  out[k++] = (uint8_t)acc;
            acc = 0; grp = 0;
        }
    }
    if (grp > 1) {
        for (size_t i = grp; i < 5; i++) acc = acc * 85 + 84;
        for (size_t i = 0; i < grp - 1; i++) out[k++] = (uint8_t)(acc >> (24 - 8 * i));
    }
    *outlen = k;
    return out;
}

/* RunLengthDecode. */
static uint8_t *rl_decode(TkPdfDoc *doc, const uint8_t *in, size_t n, size_t *outlen)
{
    size_t cap = n * 2 + 16, k = 0;
    uint8_t *out;
    if (cap > TK_PDF_MAX_STREAM) return NULL;
    out = (uint8_t *)pdf_alloc(doc, cap);
    if (!out) return NULL;
    for (size_t i = 0; i < n; ) {
        uint8_t l = in[i++];
        if (l == 128) break;
        if (l < 128) {
            size_t run = (size_t)l + 1;
            if (i + run > n || k + run > cap) break;
            memcpy(out + k, in + i, run);
            k += run; i += run;
        } else {
            size_t run = 257 - (size_t)l;
            if (i >= n || k + run > cap) break;
            memset(out + k, in[i++], run);
            k += run;
        }
    }
    *outlen = k;
    return out;
}

/*
 * apply_predictor — undo the PNG/TIFF predictor a /DecodeParms asks for.
 *
 * Needed because xref streams and /ObjStm are routinely written with
 * Predictor 12, and a reader that ignores the predictor gets bytes that
 * inflate cleanly and then decode to nonsense — no error anywhere.
 */
static uint8_t *apply_predictor(TkPdfDoc *doc, uint8_t *data, size_t len,
                                int pred, int colors, int bpc, int columns,
                                size_t *outlen)
{
    size_t bpp, rowlen, nrows, k = 0;
    uint8_t *out, *prev;

    if (pred < 2) { *outlen = len; return data; }
    if (colors <= 0) colors = 1;
    if (bpc <= 0) bpc = 8;
    if (columns <= 0) columns = 1;
    bpp = ((size_t)colors * (size_t)bpc + 7) / 8;
    rowlen = ((size_t)columns * (size_t)colors * (size_t)bpc + 7) / 8;
    if (rowlen == 0) { *outlen = len; return data; }

    if (pred == 2) {                          /* TIFF predictor */
        if (bpc != 8) { *outlen = len; return data; }
        for (size_t r = 0; r + rowlen <= len; r += rowlen)
            for (size_t i = bpp; i < rowlen; i++)
                data[r + i] = (uint8_t)(data[r + i] + data[r + i - bpp]);
        *outlen = len;
        return data;
    }

    nrows = len / (rowlen + 1);               /* PNG: one filter byte per row */
    out = (uint8_t *)pdf_alloc(doc, nrows * rowlen + 1);
    prev = (uint8_t *)pdf_alloc(doc, rowlen);
    if (!out || !prev) return NULL;

    for (size_t r = 0; r < nrows; r++) {
        const uint8_t *src = data + r * (rowlen + 1);
        uint8_t ft = src[0];
        uint8_t *dst = out + k;
        src++;
        for (size_t i = 0; i < rowlen; i++) {
            int a = i >= bpp ? dst[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            int x = src[i];
            switch (ft) {
            case 0: dst[i] = (uint8_t)x; break;
            case 1: dst[i] = (uint8_t)(x + a); break;
            case 2: dst[i] = (uint8_t)(x + b); break;
            case 3: dst[i] = (uint8_t)(x + ((a + b) >> 1)); break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                dst[i] = (uint8_t)(x + pr);
                break;
            }
            default: dst[i] = (uint8_t)x; break;
            }
        }
        memcpy(prev, dst, rowlen);
        k += rowlen;
    }
    *outlen = k;
    return out;
}

const uint8_t *pdf_stream_data(TkPdfDoc *doc, PdfObj *st, size_t *outlen)
{
    PdfObj *filter, *parms;
    uint8_t *cur;
    size_t curlen;
    uint32_t nfilters;

    if (!st || st->kind != PDF_STREAM) return NULL;
    cur = (uint8_t *)(uintptr_t)st->u.stream.raw;
    curlen = st->u.stream.rawlen;

    filter = pdf_dget(doc, st, "Filter");
    parms  = pdf_dget(doc, st, "DecodeParms");
    if (!parms) parms = pdf_dget(doc, st, "DP");
    nfilters = filter ? (filter->kind == PDF_ARR ? filter->u.arr.n : 1) : 0;

    for (uint32_t f = 0; f < nfilters; f++) {
        PdfObj *fo = filter->kind == PDF_ARR ? pdf_arr_get(doc, filter, f) : filter;
        PdfObj *po = (parms && parms->kind == PDF_ARR) ? pdf_arr_get(doc, parms, f) : parms;
        const char *name = pdf_name_of(fo);
        uint8_t *next = NULL;
        size_t nextlen = 0;
        if (!name) continue;

        if (!strcmp(name, "FlateDecode") || !strcmp(name, "Fl")) {
            next = inflate_stream(doc, cur, curlen, &nextlen);
            if (!next) {
                if (!strcmp(s_errkind, TK_PDF_E_NONE))
                    pdf_fail(TK_PDF_E_BADDOC, "pdf: FlateDecode stream would not inflate");
                return NULL;
            }
        } else if (!strcmp(name, "ASCIIHexDecode") || !strcmp(name, "AHx")) {
            next = ahx_decode(doc, cur, curlen, &nextlen);
        } else if (!strcmp(name, "ASCII85Decode") || !strcmp(name, "A85")) {
            next = a85_decode(doc, cur, curlen, &nextlen);
        } else if (!strcmp(name, "RunLengthDecode") || !strcmp(name, "RL")) {
            next = rl_decode(doc, cur, curlen, &nextlen);
        } else if (!strcmp(name, "Crypt")) {
            continue;                          /* identity crypt filter */
        } else {
            /* DCTDecode, JPXDecode, CCITTFaxDecode, JBIG2Decode: image
             * codecs.  A content stream is never encoded with one, so
             * reaching here means an image, which this module does not
             * extract (story 135.3 item 5, deliberately out of scope). */
            pdf_fail(TK_PDF_E_UNSUPPORTED, "pdf: filter %s is not implemented", name);
            return NULL;
        }
        if (!next) return NULL;
        cur = next;
        curlen = nextlen;

        if (po && po->kind == PDF_DICT) {
            int pred = (int)pdf_num(pdf_dget(doc, po, "Predictor"), 1);
            if (pred > 1) {
                size_t plen = 0;
                uint8_t *pd = apply_predictor(doc, cur, curlen, pred,
                        (int)pdf_num(pdf_dget(doc, po, "Colors"), 1),
                        (int)pdf_num(pdf_dget(doc, po, "BitsPerComponent"), 8),
                        (int)pdf_num(pdf_dget(doc, po, "Columns"), 1), &plen);
                if (!pd) return NULL;
                cur = pd;
                curlen = plen;
            }
        }
    }
    *outlen = curlen;
    return cur;
}

/* ── Object index ────────────────────────────────────────────────────── */

static int index_put(TkPdfDoc *doc, int32_t num, PdfObj *o)
{
    if (num < 0 || (uint32_t)num >= TK_PDF_MAX_OBJECTS) {
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: object number %ld past the %u cap",
                 (long)num, TK_PDF_MAX_OBJECTS);
        return -1;
    }
    if ((uint32_t)num >= doc->nobjs) {
        uint32_t nn = (uint32_t)num + 1;
        PdfObj **np = (PdfObj **)realloc(doc->objs, nn * sizeof(PdfObj *));
        if (!np) return -1;
        memset(np + doc->nobjs, 0, (nn - doc->nobjs) * sizeof(PdfObj *));
        doc->objs = np;
        doc->nobjs = nn;
    }
    doc->objs[num] = o;                       /* last definition wins */
    return 0;
}

/* Find the extent of a stream body starting at L->i (just past `stream`). */
static void stream_extent(TkPdfDoc *doc, PdfLex *L, PdfObj *dict,
                          const uint8_t **raw, size_t *rawlen)
{
    size_t start;
    PdfObj *lenobj;
    /* The EOL after the `stream` keyword is not part of the data. */
    if (L->i < L->n && L->p[L->i] == '\r') L->i++;
    if (L->i < L->n && L->p[L->i] == '\n') L->i++;
    start = L->i;

    /* Trust /Length only when it is a DIRECT integer AND `endstream` really
     * sits where it says.  An indirect /Length cannot be resolved yet — the
     * object it points at may not have been read — and a wrong /Length is
     * common enough that believing it unconditionally truncates real
     * documents. */
    lenobj = NULL;
    for (uint32_t i = 0; dict && dict->kind == PDF_DICT && i < dict->u.dict.n; i++)
        if (!strcmp(dict->u.dict.k[i], "Length")) lenobj = dict->u.dict.v[i];

    if (lenobj && lenobj->kind == PDF_NUM) {
        size_t want = (size_t)lenobj->u.num;
        if (want <= L->n - start) {
            size_t j = start + want;
            while (j < L->n && is_ws(L->p[j])) j++;
            if (j + 9 <= L->n && !memcmp(L->p + j, "endstream", 9)) {
                *raw = L->p + start;
                *rawlen = want;
                L->i = j + 9;
                return;
            }
        }
    }
    /* Fall back to searching for `endstream`. */
    {
        size_t j = start;
        while (j + 9 <= L->n) {
            if (L->p[j] == 'e' && !memcmp(L->p + j, "endstream", 9)) break;
            j++;
        }
        if (j + 9 > L->n) j = L->n;
        {
            size_t e = j;
            while (e > start && (L->p[e - 1] == '\n' || L->p[e - 1] == '\r')) e--;
            *raw = L->p + start;
            *rawlen = e - start;
        }
        L->i = j + 9 <= L->n ? j + 9 : L->n;
    }
    (void)doc;
}

/* Merge `src`'s entries into doc->trailer, later wins. */
static void merge_trailer(TkPdfDoc *doc, PdfObj *src)
{
    if (!src || src->kind != PDF_DICT) return;
    if (!doc->trailer) doc->trailer = pdf_newobj(doc, PDF_DICT);
    if (!doc->trailer) return;
    for (uint32_t i = 0; i < src->u.dict.n; i++) {
        const char *k = src->u.dict.k[i];
        int found = 0;
        for (uint32_t j = 0; j < doc->trailer->u.dict.n; j++) {
            if (!strcmp(doc->trailer->u.dict.k[j], k)) {
                doc->trailer->u.dict.v[j] = src->u.dict.v[i];
                found = 1;
                break;
            }
        }
        if (!found) dict_put(doc, doc->trailer, src->u.dict.k[i], src->u.dict.v[i]);
    }
}

/*
 * scan_objects — the sequential walk described at the top of this file.
 *
 * Steps object by object rather than searching for a pattern, so stream
 * bodies are stepped OVER and can never contribute a phantom `N G obj`.
 */
static int scan_objects(TkPdfDoc *doc)
{
    PdfLex L;
    char tok[64];
    L.p = doc->bytes; L.n = doc->len; L.i = 0;

    while (L.i < L.n) {
        size_t save;
        long objnum;
        PdfObj *o;

        pdf_skipws(&L);
        if (L.i >= L.n) break;
        save = L.i;

        if (!pdf_token(&L, tok, sizeof tok)) break;

        if (!strcmp(tok, "trailer")) {
            merge_trailer(doc, pdf_parse(doc, &L, 0));
            continue;
        }
        if (tok[0] < '0' || tok[0] > '9') continue;

        objnum = atol(tok);
        {   /* expect: <gen> obj */
            char g[64], k[64];
            if (!pdf_token(&L, g, sizeof g) || g[0] < '0' || g[0] > '9') { L.i = save + strlen(tok); continue; }
            if (!pdf_token(&L, k, sizeof k) || strcmp(k, "obj") != 0)    { L.i = save + strlen(tok); continue; }
        }

        o = pdf_parse(doc, &L, 0);
        if (!o) { continue; }

        /* A stream is a dictionary followed by the `stream` keyword. */
        {
            size_t after = L.i;
            char k[64];
            if (pdf_token(&L, k, sizeof k) && !strcmp(k, "stream") && o->kind == PDF_DICT) {
                PdfObj *s = pdf_newobj(doc, PDF_STREAM);
                if (!s) return -1;
                s->u.stream.dict = o;
                stream_extent(doc, &L, o, &s->u.stream.raw, &s->u.stream.rawlen);
                o = s;
            } else {
                L.i = after;
            }
        }
        if (index_put(doc, (int32_t)objnum, o) != 0) return -1;
    }
    return 0;
}

/*
 * expand_objstms — index the objects packed inside every /Type /ObjStm.
 *
 * Skipping this is how a reader comes to report "0 pages" for every file a
 * modern producer writes: since PDF 1.5 the catalogue, page tree and font
 * dictionaries are routinely packed into object streams, and none of them is
 * visible to a walk over the file body.
 *
 * An object already defined in the file body WINS over one unpacked here,
 * because a body definition is necessarily part of a later incremental
 * update than the object stream that the first revision packed.
 */
static int expand_objstms(TkPdfDoc *doc)
{
    uint32_t limit = doc->nobjs;
    for (uint32_t i = 0; i < limit; i++) {
        PdfObj *st = doc->objs[i];
        const uint8_t *data;
        size_t dlen = 0;
        uint32_t n, first;
        PdfLex H;

        if (!st || st->kind != PDF_STREAM) continue;
        if (!pdf_is_name(pdf_dget(doc, st, "Type"), "ObjStm")) continue;

        data = pdf_stream_data(doc, st, &dlen);
        if (!data) { pdf_clearerr(); continue; }

        n     = (uint32_t)pdf_num(pdf_dget(doc, st, "N"), 0);
        first = (uint32_t)pdf_num(pdf_dget(doc, st, "First"), 0);
        if (n > TK_PDF_MAX_OBJSTM) {
            pdf_fail(TK_PDF_E_TOOLARGE, "pdf: object stream holds %u objects, cap is %u",
                     n, TK_PDF_MAX_OBJSTM);
            return -1;
        }
        if (first > dlen) continue;

        H.p = data; H.n = first; H.i = 0;     /* the N pairs live in the header */
        for (uint32_t j = 0; j < n; j++) {
            char a[64], b[64];
            long onum, off;
            PdfLex B;
            PdfObj *o;
            if (!pdf_token(&H, a, sizeof a) || !pdf_token(&H, b, sizeof b)) break;
            onum = atol(a);
            off  = atol(b);
            if (off < 0 || (size_t)off + first > dlen) continue;
            if (onum >= 0 && (uint32_t)onum < doc->nobjs && doc->objs[onum]) continue;
            B.p = data; B.n = dlen; B.i = first + (size_t)off;
            o = pdf_parse(doc, &B, 0);
            if (o && index_put(doc, (int32_t)onum, o) != 0) return -1;
        }
    }
    return 0;
}

/* Every /Type /XRef stream dict is also a trailer; that is where /Root
 * lives in a file with no `trailer` keyword at all. */
static void collect_xref_trailers(TkPdfDoc *doc)
{
    for (uint32_t i = 0; i < doc->nobjs; i++) {
        PdfObj *o = doc->objs[i];
        if (o && o->kind == PDF_STREAM &&
            pdf_is_name(pdf_dget(doc, o, "Type"), "XRef"))
            merge_trailer(doc, o->u.stream.dict);
    }
}

/* ── Page tree ───────────────────────────────────────────────────────── */

static int pages_push(TkPdfDoc *doc, PdfObj *page)
{
    if (doc->npages >= TK_PDF_MAX_PAGES) {
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: more than %u pages", TK_PDF_MAX_PAGES);
        return -1;
    }
    if (doc->npages == doc->cappages) {
        uint32_t nc = doc->cappages ? doc->cappages * 2 : 16;
        PdfObj **np = (PdfObj **)realloc(doc->pages, nc * sizeof(PdfObj *));
        if (!np) return -1;
        doc->pages = np;
        doc->cappages = nc;
    }
    doc->pages[doc->npages++] = page;
    return 0;
}

/* Depth- and count-bounded, because /Kids may form a cycle and a cycle in a
 * page tree is a legal byte sequence, not a parse error. */
static int walk_pages(TkPdfDoc *doc, PdfObj *node, unsigned depth)
{
    PdfObj *kids;
    if (!node || depth > TK_PDF_MAX_DEPTH) return 0;
    kids = pdf_dget(doc, node, "Kids");
    if (!kids || kids->kind != PDF_ARR) {
        if (pdf_is_name(pdf_dget(doc, node, "Type"), "Page") ||
            pdf_dget(doc, node, "Contents") || pdf_dget(doc, node, "MediaBox"))
            return pages_push(doc, node);
        return 0;
    }
    for (uint32_t i = 0; i < kids->u.arr.n; i++) {
        PdfObj *kid = pdf_arr_get(doc, kids, i);
        if (!kid) continue;
        if (pdf_is_name(pdf_dget(doc, kid, "Type"), "Page")) {
            if (pages_push(doc, kid) != 0) return -1;
        } else if (walk_pages(doc, kid, depth + 1) != 0) {
            return -1;
        }
        if (doc->npages >= TK_PDF_MAX_PAGES) return -1;
    }
    return 0;
}

PdfObj *pdf_page_dict(TkPdfDoc *doc, uint32_t n)
{
    if (!doc || n == 0 || n > doc->npages) return NULL;
    return doc->pages[n - 1];
}

PdfObj *pdf_page_inherited(TkPdfDoc *doc, PdfObj *page, const char *key)
{
    unsigned hops = 0;
    while (page && hops++ < TK_PDF_MAX_DEPTH) {
        PdfObj *v = pdf_dget(doc, page, key);
        if (v) return v;
        page = pdf_dget(doc, page, "Parent");
    }
    return NULL;
}

/* ── Open / close ────────────────────────────────────────────────────── */

static void doc_free(TkPdfDoc *doc)
{
    if (!doc) return;
    for (size_t i = 0; i < doc->npool; i++) free(doc->pool[i]);
    free(doc->pool);
    free(doc->objs);
    free(doc->pages);
    free(doc->bytes);
    free(doc);
}

void pdf_close(TkPdfDoc *doc) { doc_free(doc); }

/* The %PDF- header may legally sit up to 1024 bytes into the file. */
static int has_pdf_header(const uint8_t *d, size_t n)
{
    size_t lim = n < 1024 + 8 ? n : 1024 + 8;
    for (size_t i = 0; i + 5 <= lim; i++)
        if (d[i] == '%' && !memcmp(d + i, "%PDF-", 5)) return 1;
    return 0;
}

TkPdfDoc *pdf_open_mem(const uint8_t *data, uint64_t len)
{
    TkPdfDoc *doc;
    PdfObj *root, *pages;

    pdf_clearerr();
    if (!data || len == 0) {
        pdf_fail(TK_PDF_E_BADDOC, "pdf: empty input");
        return NULL;
    }
    if (len > TK_PDF_MAX_FILE) {
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: file is %llu bytes, cap is %u",
                 (unsigned long long)len, TK_PDF_MAX_FILE);
        return NULL;
    }
    if (!has_pdf_header(data, (size_t)len)) {
        pdf_fail(TK_PDF_E_BADDOC, "pdf: no %%PDF- header in the first 1024 bytes");
        return NULL;
    }

    doc = (TkPdfDoc *)calloc(1, sizeof(TkPdfDoc));
    if (!doc) { pdf_fail(TK_PDF_E_IO, "pdf: out of memory"); return NULL; }
    doc->bytes = (uint8_t *)malloc((size_t)len + 1);
    if (!doc->bytes) { doc_free(doc); pdf_fail(TK_PDF_E_IO, "pdf: out of memory"); return NULL; }
    memcpy(doc->bytes, data, (size_t)len);
    doc->bytes[len] = '\0';
    doc->len = (size_t)len;

    if (scan_objects(doc) != 0) { doc_free(doc); return NULL; }
    collect_xref_trailers(doc);
    if (expand_objstms(doc) != 0) { doc_free(doc); return NULL; }
    collect_xref_trailers(doc);               /* an xref stream may itself
                                               * have been packed */

    /*
     * ENCRYPTION, before anything else is attempted.
     *
     * Story 135.3 priority 3: statements are routinely password-protected,
     * and an encrypted document whose strings and streams decode to noise
     * would otherwise yield an empty page — which reads as "this document
     * has no text", a different and wrong answer.  It is refused as its own
     * error kind, and deliberately not decrypted even when the user
     * password is empty: "the file is protected" is the fact the caller
     * needs, and silently opening a protected document is a decision this
     * module should not make on anyone's behalf.
     */
    if (pdf_dget(doc, doc->trailer, "Encrypt")) {
        pdf_fail(TK_PDF_E_ENCRYPTED,
                 "pdf: the document is encrypted (/Encrypt present); "
                 "std.pdf does not decrypt");
        doc_free(doc);
        return NULL;
    }

    root = pdf_dget(doc, doc->trailer, "Root");
    if (!root) {                              /* no usable trailer: find the
                                               * catalogue directly */
        for (uint32_t i = 0; i < doc->nobjs; i++)
            if (doc->objs[i] && pdf_is_name(pdf_dget(doc, doc->objs[i], "Type"), "Catalog")) {
                root = doc->objs[i];
                break;
            }
    }
    doc->root = root;

    pages = root ? pdf_dget(doc, root, "Pages") : NULL;
    if (pages) {
        if (walk_pages(doc, pages, 0) != 0) { doc_free(doc); return NULL; }
    }
    if (doc->npages == 0) {
        /* Last resort: every /Type /Page object, in object-number order.
         * A file whose page tree is damaged still has its pages. */
        for (uint32_t i = 0; i < doc->nobjs; i++)
            if (doc->objs[i] && pdf_is_name(pdf_dget(doc, doc->objs[i], "Type"), "Page"))
                if (pages_push(doc, doc->objs[i]) != 0) { doc_free(doc); return NULL; }
    }
    if (doc->npages == 0) {
        pdf_fail(TK_PDF_E_BADDOC, "pdf: no pages found");
        doc_free(doc);
        return NULL;
    }
    return doc;
}

TkPdfDoc *pdf_open_file(const char *path)
{
    FILE *f;
    long size;
    uint8_t *buf;
    TkPdfDoc *doc;

    pdf_clearerr();
    if (!path) { pdf_fail(TK_PDF_E_IO, "pdf: no path"); return NULL; }
    f = fopen(path, "rb");
    if (!f) { pdf_fail(TK_PDF_E_IO, "pdf: cannot open %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); pdf_fail(TK_PDF_E_IO, "pdf: cannot seek %s", path); return NULL; }
    size = ftell(f);
    if (size < 0) { fclose(f); pdf_fail(TK_PDF_E_IO, "pdf: cannot size %s", path); return NULL; }
    if ((unsigned long)size > TK_PDF_MAX_FILE) {
        fclose(f);
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: %s is %ld bytes, cap is %u", path, size, TK_PDF_MAX_FILE);
        return NULL;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); pdf_fail(TK_PDF_E_IO, "pdf: out of memory"); return NULL; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf); fclose(f);
        pdf_fail(TK_PDF_E_IO, "pdf: short read on %s", path);
        return NULL;
    }
    fclose(f);
    doc = pdf_open_mem(buf, (uint64_t)size);
    free(buf);
    return doc;
}

uint32_t pdf_page_count(const TkPdfDoc *doc) { return doc ? doc->npages : 0; }

/* ── Page geometry ───────────────────────────────────────────────────── */

static int media_box(TkPdfDoc *doc, uint32_t n, double out[4])
{
    PdfObj *page = pdf_page_dict(doc, n), *mb;
    out[0] = out[1] = 0; out[2] = 612; out[3] = 792;   /* US Letter default */
    if (!page) return -1;
    mb = pdf_page_inherited(doc, page, "MediaBox");
    if (!mb || pdf_arr_len(mb) < 4) return 0;
    for (int i = 0; i < 4; i++) out[i] = pdf_num(pdf_arr_get(doc, mb, (uint32_t)i), out[i]);
    /* A MediaBox may be written with its corners in either order. */
    if (out[2] < out[0]) { double t = out[0]; out[0] = out[2]; out[2] = t; }
    if (out[3] < out[1]) { double t = out[1]; out[1] = out[3]; out[3] = t; }
    return 0;
}

double pdf_page_width(TkPdfDoc *doc, uint32_t n)
{
    double b[4];
    if (media_box(doc, n, b) != 0) return 0;
    return b[2] - b[0];
}

double pdf_page_height(TkPdfDoc *doc, uint32_t n)
{
    double b[4];
    if (media_box(doc, n, b) != 0) return 0;
    return b[3] - b[1];
}

/* ── Page content ────────────────────────────────────────────────────── */

struct TkPdfPage {
    TkPdfRun *runs;
    uint32_t  n;
};

/*
 * page_content — /Contents, concatenated.
 *
 * /Contents may be an array of streams, and the split between them may fall
 * anywhere — in the middle of an operator, in the middle of a number.  The
 * spec requires them to be treated as a single stream with a whitespace
 * separator, and a reader that decodes them independently loses whatever
 * straddles a boundary.
 */
static uint8_t *page_content(TkPdfDoc *doc, PdfObj *page, size_t *outlen)
{
    PdfObj *c = pdf_dget(doc, page, "Contents");
    uint8_t *buf = NULL;
    size_t total = 0, cap = 0;
    uint32_t n, i;

    if (!c) { *outlen = 0; return NULL; }
    n = (c->kind == PDF_ARR) ? c->u.arr.n : 1;
    for (i = 0; i < n; i++) {
        PdfObj *st = (c->kind == PDF_ARR) ? pdf_arr_get(doc, c, i) : c;
        const uint8_t *d;
        size_t dl = 0;
        if (!st || st->kind != PDF_STREAM) continue;
        d = pdf_stream_data(doc, st, &dl);
        if (!d) { pdf_clearerr(); continue; }
        if (total + dl + 1 > TK_PDF_MAX_CONTENT) {
            pdf_fail(TK_PDF_E_TOOLARGE, "pdf: page content exceeds the %u MiB cap",
                     TK_PDF_MAX_CONTENT / (1024u * 1024u));
            free(buf);
            return NULL;
        }
        if (total + dl + 2 > cap) {
            size_t nc = (total + dl + 2) * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = nc;
        }
        memcpy(buf + total, d, dl);
        total += dl;
        buf[total++] = '\n';                  /* the required separator */
    }
    if (buf) buf[total] = '\0';
    *outlen = total;
    return buf;
}

TkPdfPage *pdf_page_runs(TkPdfDoc *doc, uint32_t n)
{
    PdfObj *page;
    uint8_t *content;
    size_t clen = 0;
    TkPdfRun *runs = NULL;
    uint32_t nruns = 0, cap = 0;
    TkPdfPage *pg;

    pdf_clearerr();
    page = pdf_page_dict(doc, n);
    if (!page) {
        pdf_fail(TK_PDF_E_NOPAGE, "pdf: page %u of %u", n, doc ? doc->npages : 0);
        return NULL;
    }
    content = page_content(doc, page, &clen);
    if (content && clen) {
        PdfObj *res = pdf_page_inherited(doc, page, "Resources");
        if (pdf_extract_runs(doc, res, content, clen, n, &runs, &nruns, &cap) != 0) {
            free(content);
            free(runs);
            return NULL;
        }
    }
    free(content);

    pg = (TkPdfPage *)calloc(1, sizeof(TkPdfPage));
    if (!pg) { free(runs); pdf_fail(TK_PDF_E_IO, "pdf: out of memory"); return NULL; }
    pg->runs = runs;
    pg->n = nruns;
    return pg;
}

void pdf_page_free(TkPdfPage *pg)
{
    if (!pg) return;
    for (uint32_t i = 0; i < pg->n; i++) free(pg->runs[i].text);
    free(pg->runs);
    free(pg);
}

uint32_t pdf_run_count(const TkPdfPage *pg) { return pg ? pg->n : 0; }

const TkPdfRun *pdf_run_at(const TkPdfPage *pg, uint32_t i)
{
    if (!pg || i >= pg->n) return NULL;
    return &pg->runs[i];
}

static int run_has_ink(const TkPdfRun *r)
{
    for (const char *p = r->text; p && *p; p++)
        if (!isspace((unsigned char)*p)) return 1;
    return 0;
}

int pdf_page_has_text(TkPdfDoc *doc, uint32_t n)
{
    uint32_t first = n, last = n;
    if (!doc) return 0;
    if (n == 0) { first = 1; last = doc->npages; }
    for (uint32_t p = first; p <= last; p++) {
        TkPdfPage *pg = pdf_page_runs(doc, p);
        int found = 0;
        if (!pg) continue;
        for (uint32_t i = 0; i < pg->n && !found; i++)
            if (run_has_ink(&pg->runs[i])) found = 1;
        pdf_page_free(pg);
        if (found) return 1;
    }
    return 0;
}

/* ── Reading order ───────────────────────────────────────────────────── */

/*
 * Sort key for reading order: down the page first (y DESCENDING, because
 * PDF's y grows upward), then left to right.
 *
 * Baselines that differ by less than a third of the larger font's size are
 * treated as the SAME line — a superscript, a slightly-raised currency
 * symbol or a differently-sized cell in the same row would otherwise each
 * become their own line, which scrambles a table just as thoroughly as
 * content order does.
 */
static int run_cmp(const void *a, const void *b)
{
    const TkPdfRun *ra = (const TkPdfRun *)a, *rb = (const TkPdfRun *)b;
    double tol = (ra->fontsize > rb->fontsize ? ra->fontsize : rb->fontsize) / 3.0;
    if (tol < 0.5) tol = 0.5;
    if (ra->y - rb->y > tol) return -1;
    if (rb->y - ra->y > tol) return 1;
    if (ra->x < rb->x) return -1;
    if (ra->x > rb->x) return 1;
    return 0;
}

char *pdf_page_text(TkPdfDoc *doc, uint32_t n)
{
    TkPdfPage *pg = pdf_page_runs(doc, n);
    TkPdfRun *sorted;
    char *out;
    size_t cap = 256, len = 0;

    if (!pg) return NULL;
    out = (char *)malloc(cap);
    if (!out) { pdf_page_free(pg); pdf_fail(TK_PDF_E_IO, "pdf: out of memory"); return NULL; }
    out[0] = '\0';

    sorted = pg->n ? (TkPdfRun *)malloc(pg->n * sizeof(TkPdfRun)) : NULL;
    if (pg->n && !sorted) { free(out); pdf_page_free(pg); return NULL; }
    if (sorted) {
        memcpy(sorted, pg->runs, pg->n * sizeof(TkPdfRun));
        qsort(sorted, pg->n, sizeof(TkPdfRun), run_cmp);
    }

    for (uint32_t i = 0; i < pg->n; i++) {
        const TkPdfRun *r = &sorted[i];
        const char *sep = "";
        size_t tl = strlen(r->text), sl;
        if (i > 0) {
            const TkPdfRun *p = &sorted[i - 1];
            double tol = (p->fontsize > r->fontsize ? p->fontsize : r->fontsize) / 3.0;
            if (tol < 0.5) tol = 0.5;
            if (p->y - r->y > tol) sep = "\n";
            else if (r->x - (p->x + p->width) > r->fontsize * 0.25) sep = " ";
        }
        sl = strlen(sep);
        if (len + tl + sl + 1 > cap) {
            size_t nc = (len + tl + sl + 1) * 2;
            char *nb = (char *)realloc(out, nc);
            if (!nb) { free(out); free(sorted); pdf_page_free(pg); return NULL; }
            out = nb;
            cap = nc;
        }
        memcpy(out + len, sep, sl); len += sl;
        memcpy(out + len, r->text, tl); len += tl;
        out[len] = '\0';
    }
    free(sorted);
    pdf_page_free(pg);
    return out;
}
