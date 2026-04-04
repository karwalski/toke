#ifndef TK_STDLIB_HTML_H
#define TK_STDLIB_HTML_H

/*
 * html.h — C interface for the std.html standard library module.
 *
 * Type mappings:
 *   Str     = const char *  (null-terminated UTF-8)
 *   bool    = int  (0 = false, 1 = true)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * malloc is permitted: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 18.1.2
 */

#include <stdint.h>

typedef struct TkHtmlNode TkHtmlNode;
typedef struct TkHtmlDoc  TkHtmlDoc;

/* Document */
TkHtmlDoc  *html_doc(void);
void        html_title(TkHtmlDoc *doc, const char *title);
void        html_style(TkHtmlDoc *doc, const char *css);
void        html_script(TkHtmlDoc *doc, const char *js);
const char *html_render(TkHtmlDoc *doc);   /* full <!DOCTYPE html>...<html>... string */
void        html_free(TkHtmlDoc *doc);

/* Node constructors — all return heap-allocated TkHtmlNode */
TkHtmlNode *html_div(const char *class_, const char *content);   /* class_ may be NULL */
TkHtmlNode *html_p(const char *content);
TkHtmlNode *html_h1(const char *content);
TkHtmlNode *html_h2(const char *content);
TkHtmlNode *html_span(const char *class_, const char *content);
TkHtmlNode *html_table(const char **headers, uint64_t ncols,
                        const char **rows, uint64_t nrows);  /* flat row-major cells */
TkHtmlNode *html_a(const char *href, const char *text);
TkHtmlNode *html_img(const char *src, const char *alt);

/* Tree building */
void        html_append(TkHtmlDoc *doc, TkHtmlNode *node);    /* add to <body> */
void        html_append_child(TkHtmlNode *parent, TkHtmlNode *child);

/* Utilities */
const char *html_escape(const char *s);  /* &, <, >, ", ' */
const char *html_node_render(TkHtmlNode *node);  /* render node subtree to string */
void        html_node_free(TkHtmlNode *node);

#endif /* TK_STDLIB_HTML_H */
