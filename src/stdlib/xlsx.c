/*
 * xlsx.c — std.xlsx, cell values out of an XLSX workbook (story 135.4).
 *
 * See xlsx.h for what this module is and the three traps it exists to get
 * right.  This file is laid out in the order the format is read:
 *
 *   §1  errors and buffers
 *   §2  the XML scanner            — a pull scanner, not a tree builder
 *   §3  entity decoding
 *   §4  the date mapping           — TRAP 2 lives here
 *   §5  number formats             — which numbers are dates
 *   §6  the shared-string pool     — TRAP 1 lives here
 *   §7  workbook.xml and its rels
 *   §8  worksheets                 — TRAP 3 lives here
 *   §9  open / close
 *
 * WHY A SCANNER AND NOT std.xml.  std.xml is an element BUILDER with a
 * shallow `xml_get(xml, tag)` beside it; it has no notion of document order,
 * of nesting, or of "the fourth <si> in this file".  Every one of those is
 * load-bearing here — a shared-string entry is identified by its POSITION,
 * and `<cellStyleXfs>` and `<cellXfs>` both contain `<xf>` elements that mean
 * different things.  So this is a 200-line pull scanner over the four parts
 * we read, which is smaller than the adapter the other route would need.
 */

#include "xlsx.h"
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 * §1  Errors and buffers
 * ════════════════════════════════════════════════════════════════════════ */

static const char *g_xlsx_err = "";

const char *xlsx_lasterr(void) { return g_xlsx_err; }

static void *xfail(const char *why) { g_xlsx_err = why; return NULL; }

/* A growable byte buffer.  Every accumulation in this file goes through it so
 * that a malformed part costs a failed realloc rather than a stack overrun. */
typedef struct { char *p; size_t n, cap; } Buf;

static int buf_grow(Buf *b, size_t need)
{
    if (b->n + need + 1 <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + need + 1) {
        if (cap > (size_t)1 << 40) return 0;
        cap *= 2;
    }
    char *np = (char *)realloc(b->p, cap);
    if (!np) return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static int buf_add(Buf *b, const char *s, size_t n)
{
    if (!n) return 1;
    if (!buf_grow(b, n)) return 0;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    return 1;
}

static int buf_addc(Buf *b, char c)
{
    if (!buf_grow(b, 1)) return 0;
    b->p[b->n++] = c;
    return 1;
}

/* Hand the bytes over as a NUL-terminated string; the buffer is reset.
 * An EMPTY buffer yields a heap "" rather than NULL, because "" and "failed"
 * must never be the same answer. */
static char *buf_take(Buf *b)
{
    if (!b->p) {
        char *e = (char *)malloc(1);
        if (e) e[0] = '\0';
        return e;
    }
    b->p[b->n] = '\0';
    char *r = b->p;
    b->p = NULL; b->n = 0; b->cap = 0;
    return r;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = 0; b->cap = 0; }

static char *xdup(const char *s)
{
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static char *xdupn(const char *s, size_t n)
{
    char *r = (char *)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* memmem is not C99.  This is the whole of what we need it for. */
static const char *xfind(const char *hay, size_t hn, const char *ndl, size_t nn)
{
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == ndl[0] && !memcmp(hay + i, ndl, nn)) return hay + i;
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════════
 * §2  The XML scanner
 *
 * Non-validating and deliberately so: this reads machine-written parts of a
 * container that a zip check has already accepted, and a validating parser
 * would reject workbooks that Excel itself opens.  What it DOES guarantee is
 * that it never reads outside [p, end) and never loops without advancing —
 * a truncated part ends the scan, it does not hang.
 *
 * Element names are reported LOCAL, with any namespace prefix stripped, so
 * `<x:sheetData>` and `<sheetData>` read the same.  Attribute names are
 * matched by local name too, except that a PREFIXED `id` is how `r:id` is
 * found without hard-coding the prefix a writer happened to pick.
 * ════════════════════════════════════════════════════════════════════════ */

typedef enum { XT_EOF = 0, XT_START, XT_END, XT_TEXT } XmlTokKind;

typedef struct {
    XmlTokKind  kind;
    const char *name;  size_t nlen;   /* START, END: local name              */
    const char *attrs; size_t alen;   /* START: the raw attribute span       */
    int         selfclose;            /* START: <x/>                          */
    const char *text;  size_t tlen;   /* TEXT: raw bytes                      */
    int         cdata;                /* TEXT: from <![CDATA[ ]]>, no entities */
} XmlTok;

typedef struct { const char *p, *end; } XmlScan;

static void xml_scan_init(XmlScan *s, const char *data, size_t len)
{
    s->p = data; s->end = data + len;
}

static int xml_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Strip a namespace prefix: "x:sheetData" -> "sheetData". */
static void xml_local(const char **name, size_t *nlen)
{
    const char *c = (const char *)memchr(*name, ':', *nlen);
    if (c) { *nlen -= (size_t)(c + 1 - *name); *name = c + 1; }
}

static int xml_next(XmlScan *s, XmlTok *t)
{
    memset(t, 0, sizeof *t);

    for (;;) {
        if (s->p >= s->end) { t->kind = XT_EOF; return 0; }

        if (*s->p != '<') {
            const char *st = s->p;
            while (s->p < s->end && *s->p != '<') s->p++;
            t->kind = XT_TEXT; t->text = st; t->tlen = (size_t)(s->p - st);
            return 1;
        }

        size_t left = (size_t)(s->end - s->p);

        if (left >= 9 && !memcmp(s->p, "<![CDATA[", 9)) {
            const char *st = s->p + 9;
            const char *e = xfind(st, (size_t)(s->end - st), "]]>", 3);
            t->kind = XT_TEXT; t->cdata = 1; t->text = st;
            t->tlen = (size_t)((e ? e : s->end) - st);
            s->p = e ? e + 3 : s->end;
            return 1;
        }
        if (left >= 4 && !memcmp(s->p, "<!--", 4)) {
            const char *e = xfind(s->p + 4, left - 4, "-->", 3);
            s->p = e ? e + 3 : s->end;
            continue;                      /* comments carry nothing we want */
        }
        if (left >= 2 && (s->p[1] == '?' || s->p[1] == '!')) {
            const char *e = (const char *)memchr(s->p, '>', left);
            s->p = e ? e + 1 : s->end;
            continue;                      /* <?xml ?>, <!DOCTYPE ...>       */
        }
        if (left >= 2 && s->p[1] == '/') {
            const char *st = s->p + 2;
            const char *e = (const char *)memchr(st, '>', (size_t)(s->end - st));
            const char *stop = e ? e : s->end;
            const char *ne = st;
            while (ne < stop && !xml_isspace(*ne)) ne++;
            t->kind = XT_END; t->name = st; t->nlen = (size_t)(ne - st);
            xml_local(&t->name, &t->nlen);
            s->p = e ? e + 1 : s->end;
            return 1;
        }

        /* A start tag.  Find its '>' while honouring quoted attribute values,
         * so that `<c r="a>b">` does not end the tag early. */
        {
            const char *q = s->p + 1;
            char quote = 0;
            while (q < s->end) {
                char c = *q;
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '"' || c == '\'') quote = c;
                else if (c == '>') break;
                q++;
            }
            const char *gt = (q < s->end) ? q : s->end;

            const char *st = s->p + 1;
            const char *ne = st;
            while (ne < gt && !xml_isspace(*ne) && *ne != '/') ne++;

            t->kind = XT_START;
            t->name = st; t->nlen = (size_t)(ne - st);
            xml_local(&t->name, &t->nlen);

            const char *ae = gt;
            if (ae > ne && ae[-1] == '/') { t->selfclose = 1; ae--; }
            t->attrs = ne; t->alen = (size_t)(ae > ne ? ae - ne : 0);

            s->p = (gt < s->end) ? gt + 1 : s->end;
            return 1;
        }
    }
}

static int xml_is(const XmlTok *t, const char *name)
{
    size_t n = strlen(name);
    return t->nlen == n && !memcmp(t->name, name, n);
}

/*
 * xml_attr — the raw (still entity-encoded) value of one attribute.
 *
 * `want` is matched against the attribute's LOCAL name.  `prefixed` selects
 * which spelling is accepted: 0 = unprefixed only, 1 = prefixed only (this is
 * how `r:id` is found whatever prefix the writer chose), 2 = either.
 */
static int xml_attr(const XmlTok *t, const char *want, int prefixed,
                    const char **val, size_t *vlen)
{
    const char *p = t->attrs, *end = t->attrs + t->alen;
    size_t wn = strlen(want);

    while (p < end) {
        while (p < end && xml_isspace(*p)) p++;
        if (p >= end) return 0;

        const char *ns = p;
        while (p < end && *p != '=' && !xml_isspace(*p)) p++;
        const char *nend = p;

        while (p < end && xml_isspace(*p)) p++;
        if (p >= end || *p != '=') continue;       /* valueless: skip it     */
        p++;
        while (p < end && xml_isspace(*p)) p++;
        if (p >= end) return 0;

        char q = *p;
        if (q != '"' && q != '\'') {               /* unquoted: tolerate it  */
            const char *vs = p;
            while (p < end && !xml_isspace(*p)) p++;
            if (nend - ns == (ptrdiff_t)wn && !memcmp(ns, want, wn) && prefixed != 1) {
                *val = vs; *vlen = (size_t)(p - vs); return 1;
            }
            continue;
        }
        p++;
        const char *vs = p;
        while (p < end && *p != q) p++;
        const char *ve = p;
        if (p < end) p++;

        {
            const char *ln = ns; size_t ll = (size_t)(nend - ns);
            int has_prefix = memchr(ns, ':', ll) != NULL;
            xml_local(&ln, &ll);
            int name_ok = (ll == wn && !memcmp(ln, want, wn));
            int pref_ok = (prefixed == 2) || (prefixed == 1 ? has_prefix : !has_prefix);
            if (name_ok && pref_ok) { *val = vs; *vlen = (size_t)(ve - vs); return 1; }
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * §3  Entity decoding
 * ════════════════════════════════════════════════════════════════════════ */

static int utf8_put(Buf *b, unsigned long cp)
{
    if (cp < 0x80)          return buf_addc(b, (char)cp);
    if (cp < 0x800)         return buf_addc(b, (char)(0xC0 | (cp >> 6)))
                                && buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    if (cp < 0x10000)       return buf_addc(b, (char)(0xE0 | (cp >> 12)))
                                && buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)))
                                && buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    if (cp <= 0x10FFFF)     return buf_addc(b, (char)(0xF0 | (cp >> 18)))
                                && buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)))
                                && buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)))
                                && buf_addc(b, (char)(0x80 | (cp & 0x3F)));
    return 1;                                   /* out of range: drop it     */
}

/* Append `s` decoding XML entities.  An entity this does not recognise is
 * appended VERBATIM rather than dropped: a name we do not know is data we do
 * not understand, and silently deleting it is worse than leaving it. */
static int xml_text_add(Buf *b, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') {
            size_t st = i;
            while (i < n && s[i] != '&') i++;
            if (!buf_add(b, s + st, i - st)) return 0;
            continue;
        }
        const char *semi = (const char *)memchr(s + i, ';', n - i);
        if (!semi || (size_t)(semi - (s + i)) > 12) {
            if (!buf_addc(b, '&')) return 0;
            i++;
            continue;
        }
        const char *e = s + i + 1;
        size_t el = (size_t)(semi - e);

        if (el == 3 && !memcmp(e, "amp", 3))       { if (!buf_addc(b, '&'))  return 0; }
        else if (el == 2 && !memcmp(e, "lt", 2))   { if (!buf_addc(b, '<'))  return 0; }
        else if (el == 2 && !memcmp(e, "gt", 2))   { if (!buf_addc(b, '>'))  return 0; }
        else if (el == 4 && !memcmp(e, "quot", 4)) { if (!buf_addc(b, '"'))  return 0; }
        else if (el == 4 && !memcmp(e, "apos", 4)) { if (!buf_addc(b, '\'')) return 0; }
        else if (el >= 2 && e[0] == '#') {
            unsigned long cp = 0;
            int hex = (e[1] == 'x' || e[1] == 'X');
            const char *d = e + (hex ? 2 : 1);
            int any = 0, bad = 0;
            while (d < semi) {
                int v;
                char c = *d++;
                if (c >= '0' && c <= '9')      v = c - '0';
                else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else { bad = 1; break; }
                cp = cp * (hex ? 16u : 10u) + (unsigned long)v;
                if (cp > 0x10FFFF) { bad = 1; break; }
                any = 1;
            }
            if (bad || !any) { if (!buf_add(b, s + i, (size_t)(semi - (s + i)) + 1)) return 0; }
            else if (!utf8_put(b, cp)) return 0;
        }
        else if (!buf_add(b, s + i, (size_t)(semi - (s + i)) + 1)) return 0;

        i = (size_t)(semi - s) + 1;
    }
    return 1;
}

/* Decode an attribute value into a fresh string. */
static char *xml_attr_dup(const char *v, size_t n)
{
    Buf b = {0};
    if (!xml_text_add(&b, v, n)) { buf_free(&b); return NULL; }
    return buf_take(&b);
}

/* ════════════════════════════════════════════════════════════════════════
 * §4  TRAP 2 — the date mapping, leap-year bug and all
 *
 * An Excel date is a serial number counted from an epoch chosen by the
 * workbook.  In the 1900 system the count is wrong ON PURPOSE: Lotus 1-2-3
 * treated 1900 as a leap year, Excel copied the defect in 1985 so that Lotus
 * files would import unchanged, and forty years of spreadsheets have been
 * written through it since.  Serial 60 IS 1900-02-29, a day that did not
 * happen.
 *
 * So there are two epochs in the 1900 system, not one:
 *
 *      serial  1 .. 59   days after 1899-12-31    (before the phantom day)
 *      serial 60         the phantom day itself
 *      serial 61 ..      days after 1899-12-30    (one fewer, because the
 *                                                  count is one day ahead)
 *
 * "Fixing" it means using the 1899-12-30 epoch throughout, which is right for
 * every modern date and wrong by one day for everything on or before
 * 1900-02-28.  That is the failure mode worth naming: it is not a crash, it
 * is 1900-02-27 appearing where the workbook says 1900-02-28, and nothing
 * anywhere says so.  We are DECODING someone else's encoding, so the encoding
 * is what we follow.
 *
 * The 1904 system (`<workbookPr date1904="1"/>`, the old Mac default) counts
 * from 1904-01-01 == serial 0 and has no such defect; the bug is not applied
 * there, which is itself a thing to get wrong.
 * ════════════════════════════════════════════════════════════════════════ */

/* days_from_civil / civil_from_days — Howard Hinnant's proleptic Gregorian
 * algorithms, era-based.  Integer only: no math.h, so std.xlsx needs no -lm. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = yy + (*m <= 2);
}

/* Parse a decimal serial.  Returns 0 unless the WHOLE string is a number:
 * "45000abc" is not a serial, and accepting its prefix would turn a parse bug
 * into a plausible date. */
static int parse_serial(const char *s, double *out)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

int xlsx_serial_to_text(const char *serial, int date1904, int timeonly,
                        char *out, size_t outsz)
{
    if (out && outsz) out[0] = '\0';
    double v;
    if (!out || outsz < 24 || !parse_serial(serial, &v)) return 0;
    if (v < 0.0) return 0;                  /* Excel has no negative dates   */

    int64_t whole = (int64_t)v;             /* v >= 0, so this is floor()    */
    double  frac  = v - (double)whole;

    /* A day is 86400 seconds and 0.5 has no exact decimal-to-binary partner
     * at this scale, so round rather than truncate: truncating turns 12:00:00
     * into 11:59:59 for a value that came back as 0.499999999999. */
    long secs = (long)(frac * 86400.0 + 0.5);
    if (secs >= 86400) { secs -= 86400; whole += 1; }

    int hh = (int)(secs / 3600), mi = (int)((secs / 60) % 60), ss = (int)(secs % 60);

    if (timeonly || (!date1904 && whole == 0)) {
        /* In the 1900 system serial 0 is Excel's own degenerate "1900-01-00";
         * a value under a day there is a time of day, which is how Excel
         * itself renders it. */
        snprintf(out, outsz, "%02d:%02d:%02d", hh, mi, ss);
        return 1;
    }

    int64_t  y;
    unsigned mo, da;

    if (!date1904 && whole == 60) {
        /* ── THE BUG, REPRODUCED ──────────────────────────────────────────
         * 1900-02-29 never existed.  Excel says it did, this workbook was
         * written by something that agrees with Excel, and a reader that
         * disagrees reports a different day from the one the author saw. */
        y = 1900; mo = 2; da = 29;
    } else {
        int64_t base = date1904 ? days_from_civil(1904, 1, 1)
                     : (whole < 60 ? days_from_civil(1899, 12, 31)
                                   : days_from_civil(1899, 12, 30));
        civil_from_days(base + whole, &y, &mo, &da);
    }

    if (secs)
        snprintf(out, outsz, "%04lld-%02u-%02uT%02d:%02d:%02d",
                 (long long)y, mo, da, hh, mi, ss);
    else
        snprintf(out, outsz, "%04lld-%02u-%02u", (long long)y, mo, da);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════
 * §5  Number formats — which numbers are dates
 *
 * A date cell is a NUMBER cell whose style points at a date number format.
 * Nothing in the cell itself says "date", which is why this part cannot be
 * skipped even under "no styling": telling a date from a number is not
 * presentation, it is the type.
 *
 * `s="3"` on a cell indexes xl/styles.xml's <cellXfs>, whose Nth <xf> carries
 * a numFmtId.  Ids under 164 are built in; 164 and up are defined by
 * <numFmt formatCode="..."> in the same file and have to be read.
 *
 * NOTE THE TWO xf LISTS.  <cellStyleXfs> and <cellXfs> both hold <xf>
 * elements and only the second is what `s=` indexes.  A scanner that collects
 * every <xf> it sees reads the wrong list first and every style index is then
 * off by the size of it.
 * ════════════════════════════════════════════════════════════════════════ */

typedef enum { FMT_NONE = 0, FMT_DATE, FMT_TIME, FMT_DATETIME } FmtKind;

static FmtKind builtin_fmt_kind(unsigned id)
{
    switch (id) {
    case 14: case 15: case 16: case 17:                 return FMT_DATE;
    case 18: case 19: case 20: case 21:                 return FMT_TIME;
    case 22:                                            return FMT_DATETIME;
    case 45: case 46: case 47:                          return FMT_TIME;
    /* 27-36 and 50-58 are the East Asian calendar date formats. */
    case 27: case 28: case 29: case 30: case 31:
    case 32: case 33: case 34: case 35: case 36:
    case 50: case 51: case 52: case 53: case 54:
    case 55: case 56: case 57: case 58:                 return FMT_DATE;
    default:                                            return FMT_NONE;
    }
}

/*
 * classify_format_code — is this custom format code a date, a time, or
 * neither?
 *
 * Only the FIRST section is examined: a code may be
 * `positive;negative;zero;text` and a date never varies by sign.
 *
 * Quoted literals, backslash escapes, `_x` (skip-width) and `*x` (repeat) are
 * skipped, because the letters inside them are text and not format codes —
 * `0.00" metres"` is not a date and `#,##0;[Red]-#,##0` is not a time.
 *
 * `m` is the ambiguous one: it means month next to y/d and minute next to
 * h/s, so it is counted separately and resolved at the end.
 */
static FmtKind classify_format_code(const char *code)
{
    int ymd = 0, hs = 0, mm = 0;
    for (const char *p = code; *p; p++) {
        char c = *p;
        if (c == ';') break;                       /* first section only     */
        if (c == '"') { p++; while (*p && *p != '"') p++; if (!*p) break; continue; }
        if (c == '\\' || c == '_' || c == '*') { if (p[1]) p++; continue; }
        if (c == '[') {
            /* [Red] and [$-409] are skipped; [h], [mm] and [ss] are elapsed
             * time and are NOT — they are the only reason this is a date
             * format at all. */
            const char *q = p + 1;
            while (*q && *q != ']') q++;
            int only_hms = (q > p + 1);
            for (const char *r = p + 1; r < q; r++) {
                char d = *r;
                if (d != 'h' && d != 'H' && d != 'm' && d != 'M' &&
                    d != 's' && d != 'S') { only_hms = 0; break; }
            }
            if (only_hms) hs = 1;
            p = *q ? q : q - 1;
            continue;
        }
        switch (c) {
        case 'y': case 'Y': case 'd': case 'D': ymd = 1; break;
        case 'h': case 'H': case 's': case 'S': hs = 1;  break;
        case 'm': case 'M': mm = 1;  break;
        default: break;
        }
    }
    if (mm && !hs) ymd = 1;                 /* a lone m is a month           */
    if (ymd && hs) return FMT_DATETIME;
    if (ymd)       return FMT_DATE;
    if (hs)        return FMT_TIME;
    return FMT_NONE;
}

/* ════════════════════════════════════════════════════════════════════════
 * §6 - §9  The workbook
 * ════════════════════════════════════════════════════════════════════════ */

struct TkXlsxSheet {
    TkXlsxCell *cells;   /* nrows * ncols, row-major, holes filled in */
    uint32_t    nrows, ncols;
};

struct TkXlsxBook {
    TkZipArchive *z;
    int           date1904;

    uint32_t  nsheets;
    char    **sheetname;   /* display names, workbook.xml order */
    char    **sheetpath;   /* the zip entry each one lives in   */

    uint32_t  nshared;
    char    **shared;      /* the pool cells index into         */

    uint32_t  nxf;
    unsigned char *xfkind; /* FmtKind per cellXfs index         */
};

/* ── zip helpers ─────────────────────────────────────────────────────── */

/* Read one entry as a NUL-terminated string.  XLSX parts are XML text, so a
 * NUL inside one is a corrupt part; we terminate rather than scan past it. */
static char *read_part(TkXlsxBook *wb, const char *name, size_t *out_len)
{
    uint64_t n = 0;
    uint8_t *raw = zip_read_entry(wb->z, name, &n);
    if (!raw) return NULL;
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) { free(raw); return NULL; }
    memcpy(s, raw, (size_t)n);
    s[n] = '\0';
    free(raw);
    if (out_len) *out_len = (size_t)n;
    return s;
}

static int has_entry(TkXlsxBook *wb, const char *name)
{
    uint32_t n = zip_entry_count(wb->z);
    for (uint32_t i = 0; i < n; i++) {
        const TkZipEntry *e = zip_entry_at(wb->z, i);
        if (e && !e->is_dir && !strcmp(e->name, name)) return 1;
    }
    return 0;
}

/* Find an entry whose name ENDS with `suffix`, case-insensitively on the last
 * component.  The conventional paths are universal in practice, but a writer
 * that spells `xl/SharedStrings.xml` is still readable by Excel and would
 * otherwise silently lose every string in the workbook. */
static const char *find_entry_ending(TkXlsxBook *wb, const char *suffix)
{
    size_t sl = strlen(suffix);
    uint32_t n = zip_entry_count(wb->z);
    for (uint32_t i = 0; i < n; i++) {
        const TkZipEntry *e = zip_entry_at(wb->z, i);
        if (!e || e->is_dir) continue;
        size_t nl = strlen(e->name);
        if (nl < sl) continue;
        const char *tail = e->name + (nl - sl);
        size_t k = 0;
        for (; k < sl; k++) {
            char a = tail[k], b = suffix[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == sl) return e->name;
    }
    return NULL;
}

/* ── §6  TRAP 1 — the shared-string pool ──────────────────────────────────
 *
 * A text cell normally holds `t="s"` and an INDEX.  The text lives once in
 * xl/sharedStrings.xml and every cell using it points at its position, which
 * is why a reader that parses only the worksheet comes back with small
 * integers where the column header says `Description` — and why that reader
 * looks correct on a hand-written fixture, where it is easy to write the text
 * inline, and fails on every file Excel has ever saved.
 *
 * Two details inside the pool bite as well:
 *
 *   * one entry can be MANY <t> elements — rich text splits a single string
 *     across a run per formatting change, so `<si><r><t>Total </t></r>
 *     <r><t>due</t></r></si>` is ONE string, "Total due", not two;
 *   * <rPh> holds phonetic (furigana) text, which is an ANNOTATION on the
 *     string rather than part of it, so its <t> children are skipped.
 */
static int load_shared(TkXlsxBook *wb)
{
    const char *path = "xl/sharedStrings.xml";
    if (!has_entry(wb, path)) {
        path = find_entry_ending(wb, "sharedStrings.xml");
        if (!path) return 1;                /* legitimately absent            */
    }

    size_t len = 0;
    char *xml = read_part(wb, path, &len);
    if (!xml) { xfail("xlsx: sharedStrings.xml could not be read"); return 0; }

    char   **arr = NULL;
    uint32_t n = 0, cap = 0;
    Buf      cur = {0};
    int      in_si = 0, in_t = 0, in_rph = 0, ok = 1, close = 0;

    XmlScan s; XmlTok t;
    xml_scan_init(&s, xml, len);
    while (ok && xml_next(&s, &t)) {
        close = 0;
        if (t.kind == XT_START) {
            if (xml_is(&t, "si")) {
                in_si = 1; in_rph = 0; in_t = 0;
                buf_free(&cur);
                /* `<si/>` is a legitimately empty pool entry and STILL
                 * occupies an index; skipping it shifts every string after
                 * it by one, which is trap 1 arriving by another door. */
                close = t.selfclose;
            } else if (in_si && xml_is(&t, "rPh")) {
                if (!t.selfclose) in_rph = 1;
            } else if (in_si && xml_is(&t, "t")) {
                if (!t.selfclose && !in_rph) in_t = 1;
            }
        } else if (t.kind == XT_TEXT) {
            if (in_t && t.tlen) {
                ok = t.cdata ? buf_add(&cur, t.text, t.tlen)
                             : xml_text_add(&cur, t.text, t.tlen);
            }
        } else if (t.kind == XT_END) {
            if (xml_is(&t, "t"))        in_t = 0;
            else if (xml_is(&t, "rPh")) in_rph = 0;
            else if (xml_is(&t, "si"))  close = in_si;
        }

        if (!close) continue;

        if (n >= TK_XLSX_MAX_SHARED) {
            xfail("xlsx: shared-string count cap exceeded");
            ok = 0;
            break;
        }
        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 64;
            char **na = (char **)realloc(arr, (size_t)nc * sizeof *na);
            if (!na) { ok = 0; break; }
            arr = na; cap = nc;
        }
        arr[n] = buf_take(&cur);
        if (!arr[n]) { ok = 0; break; }
        n++;
        in_si = 0; in_t = 0; in_rph = 0;
    }

    buf_free(&cur);
    free(xml);

    if (!ok) {
        for (uint32_t i = 0; i < n; i++) free(arr[i]);
        free(arr);
        if (!*g_xlsx_err) xfail("xlsx: sharedStrings.xml is malformed");
        return 0;
    }
    wb->shared = arr;
    wb->nshared = n;
    return 1;
}

/* ── §5 cont.  styles ────────────────────────────────────────────────── */

typedef struct { unsigned id; FmtKind kind; } CustomFmt;

static int load_styles(TkXlsxBook *wb)
{
    const char *path = "xl/styles.xml";
    if (!has_entry(wb, path)) {
        path = find_entry_ending(wb, "styles.xml");
        if (!path) return 1;                /* no styles: nothing is a date  */
    }

    size_t len = 0;
    char *xml = read_part(wb, path, &len);
    if (!xml) { xfail("xlsx: styles.xml could not be read"); return 0; }

    CustomFmt *cust = NULL;
    uint32_t   ncust = 0, ccap = 0;
    unsigned char *xf = NULL;
    uint32_t   nxf = 0, xcap = 0;
    int in_cellxfs = 0, ok = 1;

    XmlScan s; XmlTok t;
    xml_scan_init(&s, xml, len);
    while (ok && xml_next(&s, &t)) {
        if (t.kind == XT_END) {
            if (xml_is(&t, "cellXfs")) in_cellxfs = 0;
            continue;
        }
        if (t.kind != XT_START) continue;

        if (xml_is(&t, "cellXfs")) { in_cellxfs = !t.selfclose; continue; }

        if (xml_is(&t, "numFmt")) {
            const char *v; size_t vl;
            unsigned id = 0;
            int good;
            if (!xml_attr(&t, "numFmtId", 0, &v, &vl)) continue;
            good = vl > 0;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9') { good = 0; break; }
                id = id * 10u + (unsigned)(v[i] - '0');
            }
            if (!good) continue;
            if (!xml_attr(&t, "formatCode", 0, &v, &vl)) continue;
            char *code = xml_attr_dup(v, vl);
            if (!code) { ok = 0; break; }
            FmtKind k = classify_format_code(code);
            free(code);
            if (ncust == ccap) {
                uint32_t nc = ccap ? ccap * 2 : 16;
                CustomFmt *nn = (CustomFmt *)realloc(cust, (size_t)nc * sizeof *nn);
                if (!nn) { ok = 0; break; }
                cust = nn; ccap = nc;
            }
            cust[ncust].id = id;
            cust[ncust].kind = k;
            ncust++;
            continue;
        }

        /* ONLY the <xf> elements inside <cellXfs>.  <cellStyleXfs> holds its
         * own and `s=` does not index them. */
        if (in_cellxfs && xml_is(&t, "xf")) {
            const char *v; size_t vl;
            unsigned id = 0;
            if (xml_attr(&t, "numFmtId", 0, &v, &vl)) {
                int good = vl > 0;
                for (size_t i = 0; i < vl; i++) {
                    if (v[i] < '0' || v[i] > '9') { good = 0; break; }
                    id = id * 10u + (unsigned)(v[i] - '0');
                }
                if (!good) id = 0;
            }
            FmtKind k = builtin_fmt_kind(id);
            for (uint32_t i = 0; i < ncust; i++)
                if (cust[i].id == id) { k = cust[i].kind; break; }

            if (nxf >= TK_XLSX_MAX_XF) { xfail("xlsx: cellXfs cap exceeded"); ok = 0; break; }
            if (nxf == xcap) {
                uint32_t nc = xcap ? xcap * 2 : 32;
                unsigned char *nn = (unsigned char *)realloc(xf, nc);
                if (!nn) { ok = 0; break; }
                xf = nn; xcap = nc;
            }
            xf[nxf++] = (unsigned char)k;
        }
    }

    free(cust);
    free(xml);
    if (!ok) {
        free(xf);
        if (!*g_xlsx_err) xfail("xlsx: styles.xml is malformed");
        return 0;
    }
    wb->xfkind = xf;
    wb->nxf = nxf;
    return 1;
}

/* ── §7  workbook.xml and its relationships ───────────────────────────────
 *
 * The sheet ORDER is workbook.xml's order, and the PART each sheet lives in
 * comes from `r:id` resolved through xl/_rels/workbook.xml.rels.  Neither is
 * derivable from the other: `sheet1.xml` is not necessarily the first tab,
 * and nothing requires the parts to be named sheetN.xml at all.  Assuming
 * otherwise reads the right cells from the wrong sheet, which is a data
 * error dressed as a success.
 */

/* Normalise a relationship Target against the xl/ base. */
static char *rel_target_path(const char *target)
{
    Buf b = {0};
    const char *t = target;
    if (*t == '/') { t++; }                 /* "/xl/worksheets/s.xml"        */
    else if (!buf_add(&b, "xl/", 3)) { buf_free(&b); return NULL; }

    /* Fold "./" and "../" so that Target="../xl/worksheets/s.xml" resolves. */
    const char *seg = t;
    for (const char *p = t;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t sl = (size_t)(p - seg);
            if (sl == 1 && seg[0] == '.') {
                /* nothing */
            } else if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
                if (b.n) {
                    size_t k = b.n - 1;
                    if (b.p[k] == '/') k--;
                    while (k > 0 && b.p[k] != '/') k--;
                    b.n = (b.p[k] == '/') ? k + 1 : 0;
                }
            } else if (sl) {
                if (!buf_add(&b, seg, sl) || (*p == '/' && !buf_addc(&b, '/'))) {
                    buf_free(&b); return NULL;
                }
            }
            if (!*p) break;
            seg = p + 1;
        }
    }
    return buf_take(&b);
}

typedef struct { char *id; char *target; } Rel;

static Rel *load_rels(TkXlsxBook *wb, uint32_t *out_n)
{
    *out_n = 0;
    const char *path = "xl/_rels/workbook.xml.rels";
    if (!has_entry(wb, path)) {
        path = find_entry_ending(wb, "workbook.xml.rels");
        if (!path) return NULL;
    }
    size_t len = 0;
    char *xml = read_part(wb, path, &len);
    if (!xml) return NULL;

    Rel *rels = NULL;
    uint32_t n = 0, cap = 0;
    XmlScan s; XmlTok t;
    xml_scan_init(&s, xml, len);
    while (xml_next(&s, &t)) {
        if (t.kind != XT_START || !xml_is(&t, "Relationship")) continue;
        const char *iv, *tv; size_t il, tl;
        if (!xml_attr(&t, "Id", 0, &iv, &il)) continue;
        if (!xml_attr(&t, "Target", 0, &tv, &tl)) continue;
        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 16;
            Rel *nn = (Rel *)realloc(rels, (size_t)nc * sizeof *nn);
            if (!nn) break;
            rels = nn; cap = nc;
        }
        char *tgt = xml_attr_dup(tv, tl);
        rels[n].id = xdupn(iv, il);
        rels[n].target = tgt ? rel_target_path(tgt) : NULL;
        free(tgt);
        if (!rels[n].id || !rels[n].target) { free(rels[n].id); free(rels[n].target); break; }
        n++;
    }
    free(xml);
    *out_n = n;
    return rels;
}

static int load_workbook(TkXlsxBook *wb)
{
    const char *path = "xl/workbook.xml";
    if (!has_entry(wb, path)) {
        path = find_entry_ending(wb, "workbook.xml");
        if (!path) { xfail("xlsx: no xl/workbook.xml — not an XLSX workbook"); return 0; }
    }
    size_t len = 0;
    char *xml = read_part(wb, path, &len);
    if (!xml) { xfail("xlsx: xl/workbook.xml could not be read"); return 0; }

    uint32_t nrel = 0;
    Rel *rels = load_rels(wb, &nrel);

    char   **names = NULL, **paths = NULL;
    uint32_t n = 0, cap = 0;
    int ok = 1, in_sheets = 0;

    XmlScan s; XmlTok t;
    xml_scan_init(&s, xml, len);
    while (ok && xml_next(&s, &t)) {
        if (t.kind == XT_END) {
            if (xml_is(&t, "sheets")) in_sheets = 0;
            continue;
        }
        if (t.kind != XT_START) continue;

        if (xml_is(&t, "workbookPr")) {
            const char *v; size_t vl;
            if (xml_attr(&t, "date1904", 0, &v, &vl)) {
                wb->date1904 = (vl == 1 && v[0] == '1') ||
                               (vl == 4 && !memcmp(v, "true", 4));
            }
            continue;
        }
        if (xml_is(&t, "sheets")) { in_sheets = !t.selfclose; continue; }

        if (in_sheets && xml_is(&t, "sheet")) {
            const char *v; size_t vl;
            if (!xml_attr(&t, "name", 0, &v, &vl)) continue;
            char *nm = xml_attr_dup(v, vl);
            if (!nm) { ok = 0; break; }

            char *pt = NULL;
            /* `r:id` — matched on a PREFIXED local name `id`, so whatever
             * prefix this writer bound the relationships namespace to is the
             * one we read. */
            if (xml_attr(&t, "id", 1, &v, &vl)) {
                for (uint32_t i = 0; i < nrel; i++)
                    if (strlen(rels[i].id) == vl && !memcmp(rels[i].id, v, vl)) {
                        pt = xdup(rels[i].target);
                        break;
                    }
            }
            if (!pt) {
                /* FALLBACK, and it is a fallback: without rels the only thing
                 * left is the conventional name at this position.  It is
                 * right for every file a mainstream writer produces and it is
                 * a guess, so it is last. */
                char buf[64];
                snprintf(buf, sizeof buf, "xl/worksheets/sheet%u.xml", (unsigned)(n + 1));
                pt = xdup(buf);
            }
            if (!pt) { free(nm); ok = 0; break; }

            if (n >= TK_XLSX_MAX_SHEETS) {
                xfail("xlsx: sheet count cap exceeded");
                free(nm); free(pt); ok = 0; break;
            }
            if (n == cap) {
                uint32_t nc = cap ? cap * 2 : 8;
                char **nn1 = (char **)realloc(names, (size_t)nc * sizeof *nn1);
                if (!nn1) { free(nm); free(pt); ok = 0; break; }
                names = nn1;
                char **nn2 = (char **)realloc(paths, (size_t)nc * sizeof *nn2);
                if (!nn2) { free(nm); free(pt); ok = 0; break; }
                paths = nn2;
                cap = nc;
            }
            names[n] = nm;
            paths[n] = pt;
            n++;
        }
    }

    for (uint32_t i = 0; i < nrel; i++) { free(rels[i].id); free(rels[i].target); }
    free(rels);
    free(xml);

    if (!ok) {
        for (uint32_t i = 0; i < n; i++) { free(names[i]); free(paths[i]); }
        free(names); free(paths);
        if (!*g_xlsx_err) xfail("xlsx: xl/workbook.xml is malformed");
        return 0;
    }
    if (n == 0) {
        free(names); free(paths);
        xfail("xlsx: workbook declares no sheets");
        return 0;
    }
    wb->sheetname = names;
    wb->sheetpath = paths;
    wb->nsheets = n;
    return 1;
}

/* ── §8  TRAP 3 — worksheets, which are sparse ────────────────────────────
 *
 * A worksheet stores only the cells that have something in them, and each one
 * carries its own reference:
 *
 *     <row r="7"><c r="A7" t="s"><v>0</v></c><c r="C7"><v>12.5</v></c></row>
 *
 * Row 7 has TWO cells and its second one is column C.  A reader that walks
 * `<c>` elements in order and calls the second one column B puts C7's value
 * under B's header — for that row only, so the file looks fine everywhere the
 * rows happen to be full and is wrong exactly where a gap is, which in a bank
 * statement is the optional-reference column.  Rows are sparse the same way:
 * `<row r="9">` may follow `<row r="4">`.
 *
 * Position is therefore NEVER inferred from order here.  Every cell is placed
 * at the (row, col) its own `r=` parses to, and the result handed back is a
 * RECTANGLE with the holes filled in as `empty` cells — so a consumer
 * indexing `rows[i][j]` is right by construction, and one cross-checking
 * `.col` can see that it is.
 */

/* "C7" -> row 7, col 3.  `$` is tolerated; anything else is a reject. */
static int parse_ref(const char *r, size_t n, int64_t *row, int64_t *col)
{
    size_t i = 0;
    uint64_t c = 0, rw = 0;
    int any_c = 0, any_r = 0;

    if (i < n && r[i] == '$') i++;
    while (i < n) {
        char ch = r[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch < 'A' || ch > 'Z') break;
        c = c * 26u + (uint64_t)(ch - 'A' + 1);
        if (c > TK_XLSX_MAX_COL) return 0;
        any_c = 1;
        i++;
    }
    if (i < n && r[i] == '$') i++;
    while (i < n) {
        if (r[i] < '0' || r[i] > '9') return 0;
        rw = rw * 10u + (uint64_t)(r[i] - '0');
        if (rw > TK_XLSX_MAX_ROW) return 0;
        any_r = 1;
        i++;
    }
    if (!any_c || !any_r || rw == 0) return 0;
    *row = (int64_t)rw;
    *col = (int64_t)c;
    return 1;
}

static void col_letters(int64_t col, char *out, size_t n)
{
    char tmp[8];
    size_t k = 0;
    int64_t c = col;
    while (c > 0 && k < sizeof tmp) {
        int64_t rem = (c - 1) % 26;
        tmp[k++] = (char)('A' + rem);
        c = (c - 1) / 26;
    }
    size_t o = 0;
    while (k && o + 1 < n) out[o++] = tmp[--k];
    out[o] = '\0';
}

static void cell_free(TkXlsxCell *c)
{
    if (!c) return;
    free(c->ref); free(c->raw); free(c->value);
    c->ref = c->raw = c->value = NULL;
}

/* A cell as it is collected in pass 1, before placement. */
typedef struct {
    int64_t row, col;
    char   *kind;   /* static */
    char   *raw;    /* owned  */
    char   *value;  /* owned  */
} RawCell;

/*
 * TRAP 1, applied.  `t="s"` means `raw` is an INDEX; the value is the pool
 * entry at that index and nothing else.  An index outside the pool is a
 * corrupt workbook, and it is reported as one rather than rendered as the
 * integer it literally is — an out-of-range index that comes back as "4" is
 * the exact failure this whole module is meant to prevent, and it must not be
 * reachable by an error path either.
 */
static int resolve_cell(TkXlsxBook *wb, const char *tattr, size_t tlen,
                        int has_style, unsigned styleidx,
                        const char *raw, char **kind, char **value)
{
    char tmp[64];

    if (tlen == 1 && tattr[0] == 's') {
        char *end = NULL;
        long idx = strtol(raw, &end, 10);
        if (end == raw || (end && *end) || idx < 0 || (uint32_t)idx >= wb->nshared) {
            xfail("xlsx: shared-string index out of range");
            return 0;
        }
        *kind = TK_XLSX_STR;
        *value = xdup(wb->shared[idx]);
        return *value != NULL;
    }
    if ((tlen == 9 && !memcmp(tattr, "inlineStr", 9)) ||
        (tlen == 3 && !memcmp(tattr, "str", 3))) {
        /* inlineStr text and a formula's cached STRING result are already
         * text; `raw` holds it. */
        *kind = TK_XLSX_STR;
        *value = xdup(raw);
        return *value != NULL;
    }
    if (tlen == 1 && tattr[0] == 'b') {
        *kind = TK_XLSX_BOOL;
        *value = xdup((raw[0] == '0' || raw[0] == '\0') ? "false" : "true");
        return *value != NULL;
    }
    if (tlen == 1 && tattr[0] == 'e') {
        *kind = TK_XLSX_ERR;
        *value = xdup(raw);
        return *value != NULL;
    }
    if (tlen == 1 && tattr[0] == 'd') {
        /* ECMA-376 transitional: an ISO-8601 date written out literally.
         * No serial, no date system, nothing to reproduce. */
        *kind = TK_XLSX_DATE;
        *value = xdup(raw);
        return *value != NULL;
    }

    /* No `t`, or t="n": a NUMBER — and TRAP 2 decides whether it is a date,
     * because the cell itself does not say. */
    {
        FmtKind fk = FMT_NONE;
        if (has_style && styleidx < wb->nxf) fk = (FmtKind)wb->xfkind[styleidx];
        if (fk != FMT_NONE &&
            xlsx_serial_to_text(raw, wb->date1904, fk == FMT_TIME, tmp, sizeof tmp)) {
            *kind = TK_XLSX_DATE;
            *value = xdup(tmp);
            return *value != NULL;
        }
        *kind = TK_XLSX_NUM;
        *value = xdup(raw);
        return *value != NULL;
    }
}

TkXlsxSheet *xlsx_read_sheet(TkXlsxBook *wb, const char *sheet)
{
    g_xlsx_err = "";
    if (!wb || !sheet) return (TkXlsxSheet *)xfail("xlsx: null argument");

    const char *path = NULL;
    for (uint32_t i = 0; i < wb->nsheets; i++)
        if (!strcmp(wb->sheetname[i], sheet)) { path = wb->sheetpath[i]; break; }
    if (!path) return (TkXlsxSheet *)xfail("xlsx: no such sheet in this workbook");

    size_t len = 0;
    char *xml = read_part(wb, path, &len);
    if (!xml) return (TkXlsxSheet *)xfail("xlsx: worksheet part could not be read");

    RawCell *cells = NULL;
    uint32_t n = 0, cap = 0;
    int64_t maxrow = 0, maxcol = 0;
    int ok = 1;

    int      in_data = 0, in_v = 0, in_is_t = 0, in_f = 0, in_c = 0;
    Buf      vbuf = {0};
    int64_t  currow = 0, curcol = 0;
    int64_t  crow = 0, ccol = 0;
    char     ctype[16]; size_t ctlen = 0;
    int      chas_style = 0;
    unsigned cstyle = 0;

    XmlScan s; XmlTok t;
    xml_scan_init(&s, xml, len);

    while (ok && xml_next(&s, &t)) {
        if (t.kind == XT_TEXT) {
            /* `<f>` holds the FORMULA and must never be taken for the value:
             * scope discipline is to return the cached result, and a cell
             * whose "value" is "SUM(A1:A9)" is neither. */
            if ((in_v || in_is_t) && !in_f && t.tlen)
                ok = t.cdata ? buf_add(&vbuf, t.text, t.tlen)
                             : xml_text_add(&vbuf, t.text, t.tlen);
            continue;
        }
        if (t.kind == XT_END) {
            if (xml_is(&t, "v"))            in_v = 0;
            else if (xml_is(&t, "t"))       in_is_t = 0;
            else if (xml_is(&t, "f"))       in_f = 0;
            else if (xml_is(&t, "sheetData")) in_data = 0;
            else if (xml_is(&t, "c") && in_c) {
                in_c = 0;
                char *raw = buf_take(&vbuf);
                if (!raw) { ok = 0; break; }
                if (!raw[0]) {
                    /* A cell with no content: `<c r="B2" s="3"></c>`, which
                     * carries a format and nothing else.  It is a hole like
                     * any other and is left for the fill to place, so that
                     * "formatted but blank" and "absent" read identically —
                     * which is what a consumer means by empty. */
                    free(raw);
                    continue;
                }
                char *kind = NULL, *value = NULL;
                if (!resolve_cell(wb, ctype, ctlen, chas_style, cstyle,
                                  raw, &kind, &value)) {
                    free(raw); ok = 0; break;
                }
                if (n == cap) {
                    uint32_t nc = cap ? cap * 2 : 256;
                    RawCell *nn = (RawCell *)realloc(cells, (size_t)nc * sizeof *nn);
                    if (!nn) { free(raw); free(value); ok = 0; break; }
                    cells = nn; cap = nc;
                }
                cells[n].row = crow; cells[n].col = ccol;
                cells[n].kind = kind; cells[n].raw = raw; cells[n].value = value;
                n++;
                if (crow > maxrow) maxrow = crow;
                if (ccol > maxcol) maxcol = ccol;
            }
            continue;
        }

        /* XT_START */
        if (xml_is(&t, "sheetData")) { in_data = !t.selfclose; continue; }
        if (!in_data) continue;

        if (xml_is(&t, "row")) {
            const char *v; size_t vl;
            currow = 0;
            if (xml_attr(&t, "r", 0, &v, &vl)) {
                uint64_t rr = 0;
                int good = vl > 0;
                for (size_t i = 0; i < vl; i++) {
                    if (v[i] < '0' || v[i] > '9') { good = 0; break; }
                    rr = rr * 10u + (uint64_t)(v[i] - '0');
                    if (rr > TK_XLSX_MAX_ROW) { good = 0; break; }
                }
                if (good) currow = (int64_t)rr;
            }
            if (!currow) currow++;           /* no r=: the next row along     */
            curcol = 0;
            continue;
        }
        if (xml_is(&t, "c")) {
            const char *v; size_t vl;
            crow = currow; ccol = curcol + 1;
            if (xml_attr(&t, "r", 0, &v, &vl)) {
                int64_t rr, cc;
                if (parse_ref(v, vl, &rr, &cc)) { crow = rr; ccol = cc; }
            }
            curcol = ccol;
            ctlen = 0;
            chas_style = 0; cstyle = 0;
            buf_free(&vbuf);
            if (xml_attr(&t, "t", 0, &v, &vl) && vl < sizeof ctype) {
                memcpy(ctype, v, vl); ctlen = vl;
            }
            if (xml_attr(&t, "s", 0, &v, &vl)) {
                unsigned sv = 0; int good = vl > 0;
                for (size_t i = 0; i < vl; i++) {
                    if (v[i] < '0' || v[i] > '9') { good = 0; break; }
                    sv = sv * 10u + (unsigned)(v[i] - '0');
                }
                if (good) { chas_style = 1; cstyle = sv; }
            }
            in_c = 1; in_v = 0; in_is_t = 0; in_f = 0;
            if (t.selfclose) in_c = 0;       /* `<c r="B2" s="3"/>`: a hole   */
            continue;
        }
        if (!in_c) continue;
        if (xml_is(&t, "v"))      in_v = !t.selfclose;
        else if (xml_is(&t, "f")) in_f = !t.selfclose;
        else if (xml_is(&t, "t")) in_is_t = !t.selfclose;
    }

    buf_free(&vbuf);
    free(xml);

    TkXlsxSheet *sh = NULL;
    if (ok) {
        uint64_t total = (uint64_t)maxrow * (uint64_t)maxcol;
        if (total > TK_XLSX_MAX_CELLS) {
            xfail("xlsx: sheet cell cap exceeded (rows x columns)");
            ok = 0;
        } else {
            sh = (TkXlsxSheet *)calloc(1, sizeof *sh);
            if (!sh) ok = 0;
        }
    }

    if (ok && sh) {
        sh->nrows = (uint32_t)maxrow;
        sh->ncols = (uint32_t)maxcol;
        if (maxrow && maxcol) {
            sh->cells = (TkXlsxCell *)calloc((size_t)maxrow * (size_t)maxcol,
                                             sizeof *sh->cells);
            if (!sh->cells) ok = 0;
        }
    }

    /* Fill the rectangle: every position gets a cell, so a consumer indexing
     * by position cannot land on a NULL and cannot be misaligned by a gap. */
    if (ok && sh && sh->cells) {
        for (uint32_t r = 0; r < sh->nrows && ok; r++) {
            for (uint32_t c = 0; c < sh->ncols && ok; c++) {
                TkXlsxCell *d = &sh->cells[(size_t)r * sh->ncols + c];
                char ref[24], letters[8];
                col_letters((int64_t)c + 1, letters, sizeof letters);
                snprintf(ref, sizeof ref, "%s%u", letters, r + 1);
                d->ref = xdup(ref);
                d->row = (int64_t)r + 1;
                d->col = (int64_t)c + 1;
                d->kind = TK_XLSX_EMPTY;
                d->raw = xdup("");
                d->value = xdup("");
                if (!d->ref || !d->raw || !d->value) ok = 0;
            }
        }
    }

    /* Then place the real cells, each by its OWN parsed reference. */
    if (ok && sh && sh->cells) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r = (uint32_t)(cells[i].row - 1);
            uint32_t c = (uint32_t)(cells[i].col - 1);
            if (r >= sh->nrows || c >= sh->ncols) continue;
            TkXlsxCell *d = &sh->cells[(size_t)r * sh->ncols + c];
            free(d->raw); free(d->value);
            d->kind  = cells[i].kind;
            d->raw   = cells[i].raw;
            d->value = cells[i].value;
            cells[i].raw = NULL;
            cells[i].value = NULL;
        }
    }

    for (uint32_t i = 0; i < n; i++) { free(cells[i].raw); free(cells[i].value); }
    free(cells);

    if (!ok) {
        xlsx_sheet_free(sh);
        if (!*g_xlsx_err) xfail("xlsx: worksheet could not be parsed");
        return NULL;
    }
    return sh;
}

void xlsx_sheet_free(TkXlsxSheet *sh)
{
    if (!sh) return;
    if (sh->cells) {
        size_t total = (size_t)sh->nrows * (size_t)sh->ncols;
        for (size_t i = 0; i < total; i++) cell_free(&sh->cells[i]);
        free(sh->cells);
    }
    free(sh);
}

uint32_t xlsx_row_count(const TkXlsxSheet *sh) { return sh ? sh->nrows : 0u; }
uint32_t xlsx_col_count(const TkXlsxSheet *sh) { return sh ? sh->ncols : 0u; }

const TkXlsxCell *xlsx_cell_at(const TkXlsxSheet *sh, uint32_t r, uint32_t c)
{
    if (!sh || !sh->cells || r >= sh->nrows || c >= sh->ncols) return NULL;
    return &sh->cells[(size_t)r * sh->ncols + c];
}

/* ── §9  open / close ─────────────────────────────────────────────────── */

static TkXlsxBook *xlsx_from_zip(TkZipArchive *z)
{
    if (!z) {
        /* zip_lasterr() is already set and says which of its six rules fired;
         * restating it as "xlsx: bad archive" would throw that away. */
        g_xlsx_err = zip_lasterr();
        if (!g_xlsx_err || !*g_xlsx_err) g_xlsx_err = "xlsx: archive rejected";
        return NULL;
    }
    TkXlsxBook *wb = (TkXlsxBook *)calloc(1, sizeof *wb);
    if (!wb) { zip_close(z); return (TkXlsxBook *)xfail("xlsx: out of memory"); }
    wb->z = z;

    if (!load_shared(wb) || !load_styles(wb) || !load_workbook(wb)) {
        xlsx_close(wb);
        return NULL;
    }
    return wb;
}

TkXlsxBook *xlsx_open_mem(const uint8_t *data, uint64_t len)
{
    g_xlsx_err = "";
    return xlsx_from_zip(zip_open_mem(data, len));
}

TkXlsxBook *xlsx_open_file(const char *path)
{
    g_xlsx_err = "";
    if (!path || !*path) return (TkXlsxBook *)xfail("xlsx: empty path");
    return xlsx_from_zip(zip_open_file(path));
}

void xlsx_close(TkXlsxBook *wb)
{
    if (!wb) return;
    for (uint32_t i = 0; i < wb->nsheets; i++) {
        free(wb->sheetname[i]);
        free(wb->sheetpath[i]);
    }
    free(wb->sheetname);
    free(wb->sheetpath);
    for (uint32_t i = 0; i < wb->nshared; i++) free(wb->shared[i]);
    free(wb->shared);
    free(wb->xfkind);
    zip_close(wb->z);
    free(wb);
}

uint32_t xlsx_sheet_count(const TkXlsxBook *wb) { return wb ? wb->nsheets : 0u; }

const char *xlsx_sheet_name(const TkXlsxBook *wb, uint32_t i)
{
    if (!wb || i >= wb->nsheets) return NULL;
    return wb->sheetname[i];
}

int xlsx_date1904(const TkXlsxBook *wb) { return wb ? wb->date1904 : 0; }
