/*
 * md.c — Implementation of the std.md standard library module.
 *
 * FFI wrapper around the cmark CommonMark reference implementation.
 * cmark is vendored at stdlib/vendor/cmark/.
 *
 * Story: 55.5.2  Branch: feature/stdlib-55.5-md
 */

#include "md.h"
#include "../../stdlib/vendor/cmark/src/cmark.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Table pre-processor
 *
 * Vanilla cmark does not support GFM tables.  We detect pipe-delimited
 * table blocks, render each to HTML ourselves (every cell byte escaped),
 * and lift it out of the source, leaving a placeholder paragraph.  cmark
 * then runs in its default SAFE mode, which neutralises any raw HTML the
 * source itself carries, and the trusted tables are put back in place of
 * their placeholders afterwards (124.3, ADR-0011; placeholders 121.41).
 * ---------------------------------------------------------------------- */

typedef struct { char *buf; size_t len; size_t cap; } MdBuf;

static void mdb_init(MdBuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }

/* Returns 1 when `need` more bytes (plus a NUL) fit, 0 when they cannot.
 * 135.3c: this returned void and every caller then wrote regardless -- on a
 * failed realloc that was a write past the old capacity, or through NULL
 * (clang unix.cstring.NullArg md.c:40). */
static int mdb_grow(MdBuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap * 2 : 512;
    while (nc < b->len + need + 1) nc *= 2;
    char *p = realloc(b->buf, nc);
    if (!p) return 0;
    b->buf = p; b->cap = nc;
    return 1;
}

static void mdb_cat(MdBuf *b, const char *s, size_t n) {
    if (!n) return;
    if (!mdb_grow(b, n)) return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void mdb_cats(MdBuf *b, const char *s) { if (s) mdb_cat(b, s, strlen(s)); }

static void mdb_esc(MdBuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '<': mdb_cats(b, "&lt;");   break;
            case '>': mdb_cats(b, "&gt;");   break;
            case '&': mdb_cats(b, "&amp;");  break;
            case '"': mdb_cats(b, "&quot;"); break;
            default:  if (!mdb_grow(b, 1)) return;
                      b->buf[b->len++] = s[i]; b->buf[b->len] = '\0'; break;
        }
    }
}

/* Is this line a table row? (starts with optional spaces then '|') */
static int md_is_table_row(const char *line) {
    while (*line == ' ') line++;
    return *line == '|';
}

/* Is this line a separator row? e.g. |---|:---:|---| */
static int md_is_separator(const char *line) {
    while (*line == ' ') line++;
    if (*line != '|') return 0;
    line++;
    int has_dash = 0;
    while (*line) {
        if (*line == '-' || *line == ':') has_dash = 1;
        else if (*line != '|' && *line != ' ') return 0;
        line++;
    }
    return has_dash;
}

/* The same two tests on the bytes [s, s+n) of a line that is not
 * NUL-terminated, with any trailing CRs ignored (121.41).  The quick scan in
 * md_preprocess_tables used to copy each candidate line into a char[512]
 * and skip any line that did not fit, so a table whose separator row was
 * 512 bytes or longer was never detected -- and it tested the copy with its
 * trailing '\r' still on, so a CRLF table was never detected either. */
static int md_line_test(const char *s, size_t n, int (*test)(const char *)) {
    while (n > 0 && s[n - 1] == '\r') n--;
    size_t i = 0;
    while (i < n && s[i] == ' ') i++;
    if (i == n || s[i] != '|') return 0;   /* both tests need a leading '|' */
    char *tmp = malloc(n + 1);
    if (!tmp) return 0;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    int r = test(tmp);
    free(tmp);
    return r;
}

/* 121.16 (PAR-05 residue): 124.3 moved cmark to SAFE mode, but pipe tables
 * are rendered here and re-injected as trusted HTML after cmark has run, so
 * cmark's URL filter never saw a table-cell link: `| [x](javascript:alert(1)) |`
 * came out as `<a href="javascript:alert(1)">`.  This is cmark's rule
 * (javascript:, vbscript:, file:, and data: other than image/png, gif, jpeg
 * or webp, case-insensitively) applied the way a browser reads the
 * attribute: leading C0 controls and spaces are skipped and ASCII tab/LF/CR
 * inside the scheme are ignored (the WHATWG URL parser strips them), so
 * " java\tscript:" is caught too.  A dangerous URL becomes href="", as
 * cmark renders it. */
static int md_url_dangerous(const char *u, size_t n) {
    char sch[16];
    size_t k = 0, i = 0;
    while (i < n && (unsigned char)u[i] <= 0x20) i++;
    for (; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        if (c == ':') break;
        if (k + 1 >= sizeof sch) return 0;        /* longer than any we refuse */
        sch[k++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    if (i >= n) return 0;                         /* no scheme: relative URL */
    sch[k] = '\0';
    if (!strcmp(sch, "javascript") || !strcmp(sch, "vbscript") || !strcmp(sch, "file"))
        return 1;
    if (strcmp(sch, "data") != 0) return 0;
    /* data: -- only the image types cmark allows */
    static const char *ok[] = { "image/png", "image/gif", "image/jpeg", "image/webp" };
    const char *rest = u + i + 1;
    size_t rn = n - i - 1;
    for (size_t t = 0; t < sizeof ok / sizeof ok[0]; t++) {
        size_t L = strlen(ok[t]);
        if (rn >= L) {
            size_t m = 0;
            while (m < L && (char)((rest[m] >= 'A' && rest[m] <= 'Z') ? rest[m] + 32 : rest[m]) == ok[t][m]) m++;
            if (m == L) return 0;
        }
    }
    return 1;
}

/* Render inline markdown for a cell: `code`, **bold**, *italic*, [link](url) */
static void md_inline_cell(MdBuf *b, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        /* inline code */
        if (s[i] == '`') {
            size_t j = i + 1;
            while (j < len && s[j] != '`') j++;
            if (j < len) {
                mdb_cats(b, "<code>");
                mdb_esc(b, s + i + 1, j - i - 1);
                mdb_cats(b, "</code>");
                i = j + 1;
                continue;
            }
        }
        /* bold */
        if (s[i] == '*' && i + 1 < len && s[i+1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(s[j] == '*' && s[j+1] == '*')) j++;
            if (j + 1 < len) {
                mdb_cats(b, "<strong>");
                md_inline_cell(b, s + i + 2, j - i - 2);
                mdb_cats(b, "</strong>");
                i = j + 2;
                continue;
            }
        }
        /* italic */
        if (s[i] == '*') {
            size_t j = i + 1;
            while (j < len && s[j] != '*') j++;
            if (j < len) {
                mdb_cats(b, "<em>");
                md_inline_cell(b, s + i + 1, j - i - 1);
                mdb_cats(b, "</em>");
                i = j + 1;
                continue;
            }
        }
        /* link */
        if (s[i] == '[') {
            size_t j = i + 1;
            while (j < len && s[j] != ']') j++;
            if (j < len && j + 1 < len && s[j+1] == '(') {
                size_t k = j + 2;
                while (k < len && s[k] != ')') k++;
                if (k < len) {
                    mdb_cats(b, "<a href=\"");
                    if (!md_url_dangerous(s + j + 2, k - j - 2))
                        mdb_esc(b, s + j + 2, k - j - 2);
                    mdb_cats(b, "\">");
                    md_inline_cell(b, s + i + 1, j - i - 1);
                    mdb_cats(b, "</a>");
                    i = k + 1;
                    continue;
                }
            }
        }
        /* escape special chars */
        switch (s[i]) {
            case '<': mdb_cats(b, "&lt;");  break;
            case '>': mdb_cats(b, "&gt;");  break;
            case '&': mdb_cats(b, "&amp;"); break;
            default:  if (!mdb_grow(b, 1)) return;
                      b->buf[b->len++] = s[i]; b->buf[b->len] = '\0'; break;
        }
        i++;
    }
}

/* Emit cells from a row into <th> or <td> tags */
static void md_emit_cells(MdBuf *b, const char *line, const char *tag) {
    while (*line == ' ') line++;
    if (*line == '|') line++;  /* skip leading pipe */

    mdb_cats(b, "<tr>");
    const char *cell_start = line;
    while (*line) {
        if (*line == '|' || *(line + 1) == '\0') {
            const char *end = (*line == '|') ? line : line + 1;
            /* trim cell */
            const char *cs = cell_start;
            while (cs < end && *cs == ' ') cs++;
            const char *ce = end - 1;
            while (ce > cs && *ce == ' ') ce--;
            size_t clen = (ce >= cs) ? (size_t)(ce - cs + 1) : 0;
            /* skip empty trailing cell after final pipe */
            if (clen > 0 || *line != '\0') {
                mdb_cats(b, "<"); mdb_cats(b, tag); mdb_cats(b, ">");
                md_inline_cell(b, cs, clen);
                mdb_cats(b, "</"); mdb_cats(b, tag); mdb_cats(b, ">");
            }
            if (*line == '|') {
                cell_start = line + 1;
                /* if this pipe is at end of line, don't emit empty cell */
                if (*(line + 1) == '\0' || (*(line + 1) == '\n')) break;
            }
        }
        if (*line) line++;
    }
    mdb_cats(b, "</tr>\n");
}

/*
 * Pre-process markdown source: convert pipe tables to HTML.
 * Returns a new heap-allocated string (caller must free).
 * Returns NULL if no tables found (caller should use original src).
 */
/* 124.3 (ADR-0011): side-list of trusted table HTML generated by
 * md_preprocess_tables. The tables are lifted out of the source and replaced
 * with placeholders so cmark can run in SAFE mode (neutralizing untrusted raw
 * HTML, including any user-written <table>), then re-injected afterwards. */
typedef struct { char **items; int n; int cap; } MdTables;
static void mdtables_init(MdTables *t) { t->items = NULL; t->n = 0; t->cap = 0; }
static int  mdtables_add(MdTables *t, const char *s, size_t len) {
    if (t->n >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 8;
        char **ni = realloc(t->items, (size_t)nc * sizeof(char *));
        if (!ni) return -1;
        t->items = ni; t->cap = nc;
    }
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, s, len); c[len] = '\0';
    t->items[t->n] = c;
    return t->n++;
}
static void mdtables_free(MdTables *t) {
    for (int i = 0; i < t->n; i++) free(t->items[i]);
    free(t->items);
}
/* Placeholders (121.41).  A table is lifted out as the paragraph
 * "<prefix><n>", and cmark renders it as "<p><prefix><n></p>".  Before
 * 121.41 the prefix was fixed and EVERY occurrence of it in cmark's output
 * was replaced, so the literal text TKMDTABLEPLACEHOLDER0 in a document
 * was replaced by a copy of table 0 -- and inside a link destination,
 * [y](TKMDTABLEPLACEHOLDER0), the table's own quotes closed the href and
 * its cell text became attributes of the link.
 *
 * Now a placeholder is replaced only as a whole paragraph, and only when the
 * prefix occurs in the output exactly once per table, each index once.  Any
 * other occurrence -- a literal in the source, or one spelled with character
 * references that cmark decodes -- rejects the prefix, and md_render tries
 * the next one (the base followed by one or more 'Q').  After MD_PH_TRIES
 * rejected prefixes it renders without lifting tables, as plain text. */
#define MD_TABLE_PH "TKMDTABLEPLACEHOLDER"
#define MD_PH_TRIES 8

/* Move the just-emitted table HTML (out->buf[ts..len]) into the side-list and
 * replace it with a standalone placeholder paragraph. */
static void md_flush_table(MdBuf *out, size_t ts, MdTables *tables, const char *ph) {
    if (out->len <= ts) return;
    int idx = mdtables_add(tables, out->buf + ts, out->len - ts);
    if (idx < 0) return;  /* OOM: leave the table inline (dropped by safe cmark) */
    out->len = ts;
    if (out->buf) out->buf[out->len] = '\0';
    char num[16];
    snprintf(num, sizeof num, "%d", idx);
    mdb_cats(out, "\n\n");
    mdb_cats(out, ph);
    mdb_cats(out, num);
    mdb_cats(out, "\n\n");
}

/* Replace each "<p><ph><n></p>" in cmark's output with table n.  Returns
 * NULL -- the caller then tries another prefix -- when ph occurs anywhere
 * else, or a table's placeholder is missing or repeated. */
static char *md_reinject_tables(const char *html, MdTables *tables, const char *ph) {
    size_t phlen = strlen(ph);
    int seen_n = 0;
    for (const char *q = strstr(html, ph); q; q = strstr(q + phlen, ph)) seen_n++;
    if (seen_n != tables->n) return NULL;
    char *seen = calloc((size_t)tables->n, 1);
    if (!seen) return NULL;

    MdBuf out; mdb_init(&out);
    const char *p = html;
    while (*p) {
        if (strncmp(p, "<p>", 3) == 0 && strncmp(p + 3, ph, phlen) == 0) {
            const char *d = p + 3 + phlen; size_t nd = 0;
            while (d[nd] >= '0' && d[nd] <= '9' && nd < 9) nd++;
            int idx = (nd > 0) ? atoi(d) : -1;
            if (idx >= 0 && idx < tables->n && !seen[idx] &&
                strncmp(d + nd, "</p>", 4) == 0) {
                seen[idx] = 1;
                mdb_cats(&out, tables->items[idx]);
                p = d + nd + 4;
                continue;
            }
        }
        mdb_cat(&out, p, 1); p++;
    }
    int ok = 1;
    for (int i = 0; i < tables->n; i++) if (!seen[i]) ok = 0;
    free(seen);
    if (!ok) { free(out.buf); return NULL; }
    if (!out.buf) { char *e = malloc(1); if (e) e[0] = '\0'; return e; }
    return out.buf;
}

static char *md_preprocess_tables(const char *src, MdTables *tables, const char *ph) {
    /* Quick scan: any pipe tables at all? */
    int has_table = 0;
    const char *scan = src;
    while (*scan) {
        const char *ls = scan;
        while (*scan && *scan != '\n') scan++;
        size_t ll = (size_t)(scan - ls);
        if (*scan == '\n') scan++;
        /* The same row-then-separator test the main loop applies. */
        if (ll > 0 && md_line_test(ls, ll, md_is_table_row)) {
            const char *ns = scan;
            while (*ns && *ns != '\n') ns++;
            size_t nl = (size_t)(ns - scan);
            if (nl > 0 && md_line_test(scan, nl, md_is_separator)) { has_table = 1; break; }
        }
    }
    if (!has_table) return NULL;

    MdBuf out;
    mdb_init(&out);

    const char *p = src;
    while (*p) {
        /* Collect current line */
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') p++;

        /* Make null-terminated copy of line */
        char *line = malloc(line_len + 1);
        if (!line) { mdb_cat(&out, line_start, line_len); mdb_cats(&out, "\n"); continue; }
        memcpy(line, line_start, line_len);
        /* strip trailing \r */
        while (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        line[line_len] = '\0';

        /* Check for table start: row followed by separator */
        if (md_is_table_row(line)) {
            /* peek at next line */
            const char *next_start = p;
            const char *np = p;
            while (*np && *np != '\n') np++;
            size_t next_len = (size_t)(np - next_start);
            char *next_line = malloc(next_len + 1);
            if (next_line) {
                memcpy(next_line, next_start, next_len);
                while (next_len > 0 && next_line[next_len - 1] == '\r') next_len--;
                next_line[next_len] = '\0';
            }

            if (next_line && md_is_separator(next_line)) {
                /* Found a table! */
                size_t ts = out.len;
                mdb_cats(&out, "<table>\n<thead>\n");
                md_emit_cells(&out, line, "th");
                mdb_cats(&out, "</thead>\n<tbody>\n");

                /* Skip separator line */
                p = np;
                if (*p == '\n') p++;
                free(next_line);

                /* Body rows */
                while (*p) {
                    const char *rs = p;
                    while (*p && *p != '\n') p++;
                    size_t rlen = (size_t)(p - rs);
                    if (*p == '\n') p++;
                    char *row = malloc(rlen + 1);
                    if (!row) break;
                    memcpy(row, rs, rlen);
                    while (rlen > 0 && row[rlen - 1] == '\r') rlen--;
                    row[rlen] = '\0';

                    if (!md_is_table_row(row) || md_is_separator(row)) {
                        /* End of table — emit this line normally */
                        mdb_cats(&out, "</tbody>\n</table>\n");
                        md_flush_table(&out, ts, tables, ph);
                        mdb_cat(&out, rs, (size_t)((p > rs ? p - rs : 0)));
                        if (*(p - 1) != '\n') mdb_cats(&out, "\n");
                        free(row);
                        goto next_line_label;
                    }
                    md_emit_cells(&out, row, "td");
                    free(row);
                }
                mdb_cats(&out, "</tbody>\n</table>\n");
                md_flush_table(&out, ts, tables, ph);
                free(line);
                continue;
            }
            free(next_line);
        }

        /* Not a table — pass through */
        mdb_cat(&out, line_start, (size_t)(p - line_start));
        next_line_label:
        free(line);
    }

    if (!out.buf) return NULL;
    return out.buf;
}

/*
 * md_render — render CommonMark Markdown to HTML.
 *
 * src: null-terminated UTF-8 Markdown string.
 * Returns a heap-allocated null-terminated HTML string.
 * Never returns NULL — on allocation failure returns a copy of src.
 * Invalid Markdown degrades gracefully (cmark never errors).
 */
const char *md_render(const char *src)
{
    if (!src) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    /* 124.3 (ADR-0011): lift trusted pipe-tables out to a side-list (leaving
     * placeholders), render with SAFE cmark — which neutralizes untrusted raw
     * HTML from the source (`<script>`, `<div onclick=…>`, even a user-written
     * `<table>`) instead of passing it through (stored/reflected XSS) — then
     * re-inject the trusted tables into the sanitized output. */
    int options = CMARK_OPT_DEFAULT;
    char *html = NULL;
    char ph[sizeof MD_TABLE_PH + MD_PH_TRIES];
    for (int attempt = 0; attempt < MD_PH_TRIES && !html; attempt++) {
        /* 121.41: a prefix is used only if the placeholders it produces are
         * the only occurrences of it in cmark's output (see above). */
        memcpy(ph, MD_TABLE_PH, sizeof MD_TABLE_PH - 1);
        memset(ph + sizeof MD_TABLE_PH - 1, 'Q', (size_t)attempt);
        ph[sizeof MD_TABLE_PH - 1 + (size_t)attempt] = '\0';
        if (strstr(src, ph)) continue;

        MdTables tables; mdtables_init(&tables);
        char *preprocessed = md_preprocess_tables(src, &tables, ph);
        const char *input = preprocessed ? preprocessed : src;
        char *raw = cmark_markdown_to_html(input, strlen(input), options);
        free(preprocessed);  /* safe even if NULL */
        if (raw && tables.n > 0) {
            char *reinj = md_reinject_tables(raw, &tables, ph);
            free(raw);
            raw = reinj;            /* NULL: prefix rejected, try the next */
        }
        mdtables_free(&tables);
        html = raw;
    }
    if (!html) {
        /* Every prefix rejected: render with no tables lifted.  SAFE cmark
         * shows the pipe rows as text. */
        html = cmark_markdown_to_html(src, strlen(src), options);
    }

    if (!html) {
        /* Allocation failure — return copy of source as fallback. */
        size_t slen = strlen(src);
        char *copy = malloc(slen + 1);
        if (!copy) return src;  /* last resort: return literal pointer */
        memcpy(copy, src, slen + 1);
        return copy;
    }
    return html;
}

/*
 * md_render_file — read a file and render its Markdown content to HTML.
 * Returns NULL on file read failure (sets err_msg).
 */
MdFileResult md_render_file(const char *path)
{
    MdFileResult r = {NULL, 0, NULL};
    if (!path) { r.is_err = 1; r.err_msg = "null path"; return r; }

    FILE *f = fopen(path, "rb");
    if (!f) { r.is_err = 1; r.err_msg = "file not found"; return r; }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        r.is_err = 1; r.err_msg = "seek failed"; return r;
    }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); r.is_err = 1; r.err_msg = "ftell failed"; return r; }
    /* 135.3c: fseek, not rewind() -- rewind reports failure only through errno, which nothing read (clang unix.Errno). */
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); r.is_err = 1; r.err_msg = "seek failed"; return r; }

    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    size_t nread = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[nread] = '\0';

    r.ok = md_render(src);
    free(src);
    return r;
}
