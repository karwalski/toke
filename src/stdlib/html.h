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

/* -----------------------------------------------------------------------
 * String-based element constructors (Story 34.1.1)
 * All return heap-allocated null-terminated strings; callers own them.
 * ----------------------------------------------------------------------- */

/* Form elements */
const char *html_form(const char *action, const char *method,
                      const char *const *children, uint64_t nchild);
const char *html_input(const char *type, const char *name, const char *value);
const char *html_select(const char *name,
                        const char *const *options, uint64_t nopts,
                        const char *selected);
const char *html_textarea(const char *name, const char *content,
                           uint64_t rows, uint64_t cols);
const char *html_button(const char *text, const char *type);
const char *html_label(const char *for_id, const char *text);

/* List elements */
const char *html_ul(const char *const *items, uint64_t n);
const char *html_ol(const char *const *items, uint64_t n);

/* Inline / structural */
const char *html_br(void);
const char *html_hr(void);
const char *html_pre(const char *content);
const char *html_code(const char *content);

/* -----------------------------------------------------------------------
 * String-based attribute helpers (Story 34.1.2)
 * These operate on an existing HTML string and return a new string.
 * ----------------------------------------------------------------------- */

const char *html_attr(const char *node, const char *name, const char *value);
const char *html_class_add(const char *node, const char *cls);
const char *html_id(const char *node, const char *id);
const char *html_meta(const char *name, const char *content);
const char *html_link_stylesheet(const char *href);

#endif /* TK_STDLIB_HTML_H */
