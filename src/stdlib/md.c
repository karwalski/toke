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
 * table blocks and convert them to raw HTML before handing the source to
 * cmark (which passes raw HTML through when CMARK_OPT_UNSAFE is set).
 * ---------------------------------------------------------------------- */

typedef struct { char *buf; size_t len; size_t cap; } MdBuf;

static void mdb_init(MdBuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }

static void mdb_grow(MdBuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 512;
    while (nc < b->len + need + 1) nc *= 2;
    char *p = realloc(b->buf, nc);
    if (!p) return;
    b->buf = p; b->cap = nc;
}

static void mdb_cat(MdBuf *b, const char *s, size_t n) {
    if (!n) return;
    mdb_grow(b, n);
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
            default:  mdb_grow(b, 1); b->buf[b->len++] = s[i]; b->buf[b->len] = '\0'; break;
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
            default:  mdb_grow(b, 1); b->buf[b->len++] = s[i]; b->buf[b->len] = '\0'; break;
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
static char *md_preprocess_tables(const char *src) {
    /* Quick scan: any pipe tables at all? */
    int has_table = 0;
    const char *scan = src;
    while (*scan) {
        const char *ls = scan;
        while (*scan && *scan != '\n') scan++;
        size_t ll = (size_t)(scan - ls);
        if (*scan == '\n') scan++;
        /* Need null-terminated copy for detection functions */
        if (ll > 0 && ls[0] == '|') {
            char tmp[512];
            if (ll < sizeof(tmp)) {
                memcpy(tmp, ls, ll); tmp[ll] = '\0';
                if (md_is_table_row(tmp)) {
                    const char *ns = scan;
                    while (*ns && *ns != '\n') ns++;
                    size_t nl = (size_t)(ns - scan);
                    if (nl > 0 && nl < sizeof(tmp)) {
                        memcpy(tmp, scan, nl); tmp[nl] = '\0';
                        if (md_is_separator(tmp)) { has_table = 1; break; }
                    }
                }
            }
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
                        mdb_cat(&out, rs, (size_t)((p > rs ? p - rs : 0)));
                        if (*(p - 1) != '\n') mdb_cats(&out, "\n");
                        free(row);
                        goto next_line_label;
                    }
                    md_emit_cells(&out, row, "td");
                    free(row);
                }
                mdb_cats(&out, "</tbody>\n</table>\n");
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

    /* Pre-process tables into raw HTML before cmark */
    char *preprocessed = md_preprocess_tables(src);
    const char *input = preprocessed ? preprocessed : src;

    int options = CMARK_OPT_UNSAFE;  /* allow raw HTML passthrough */
    char *html = cmark_markdown_to_html(input, strlen(input), options);
    free(preprocessed);  /* safe even if NULL */

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
    rewind(f);

    char *src = malloc((size_t)sz + 1);
    if (!src) { fclose(f); r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    size_t nread = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[nread] = '\0';

    r.ok = md_render(src);
    free(src);
    return r;
}
