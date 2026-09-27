/*
 * html.c — Implementation of the std.html standard library module.
 *
 * Provides a simple tree-based HTML builder. Nodes form a linked-list tree
 * via first_child / next_sibling pointers. The document holds head metadata
 * (title, style, script) and an ordered list of body nodes.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * content strings passed to node constructors are NOT auto-escaped; callers
 * must use html_escape() when they want safe output.
 *
 * Story: 18.1.2
 */

#include "html.h"
#include "strbuf.h"
#include "tk_array.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* -----------------------------------------------------------------------
 * Internal helper: growable string buffer
 * ----------------------------------------------------------------------- */

/* 122.2: html.c's private Buf (realloc unchecked: a failure left data NULL
 * and the next memcpy wrote through it) is the shared TkStrBuf now. */
typedef TkStrBuf Buf;

/* -----------------------------------------------------------------------
 * TkHtmlNode
 * ----------------------------------------------------------------------- */

/* Attribute: name="value" pair. */
typedef struct TkAttr {
    char          *name;
    char          *value;
    struct TkAttr *next;
} TkAttr;

/* struct TkHtmlNode is in html.h: its first three fields are the
 * $htmlnode record toke reads (135.30). */

static TkHtmlNode *node_new(const char *tag, int void_element)
{
    TkHtmlNode *n = (TkHtmlNode *)calloc(1, sizeof(TkHtmlNode));
    if (!n) return NULL;
    n->tag         = strdup(tag);
    n->attrs       = tk_arr_alloc(4, 0);   /* empty, never a null handle */
    n->content     = strdup("");
    n->void_element = void_element;
    return n;
}

/* node_set_text — the element's text (the $htmlnode content field). */
static void node_set_text(TkHtmlNode *n, const char *text)
{
    char *t = strdup(text ? text : "");
    if (!t) return;
    free(n->content);
    n->content = t;
}

static void node_add_attr(TkHtmlNode *n, const char *name, const char *value)
{
    if (!name || !value) return;
    TkAttr *a = (TkAttr *)malloc(sizeof(TkAttr));
    if (!a) return;
    a->name  = strdup(name);
    a->value = strdup(value);
    a->next  = NULL;
    /* 135.30: mirror it into the toke-visible attrs, as @(name; value) */
    int64_t pair = tk_arr_alloc(2, 2);
    if (pair) {
        ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)a->name;
        ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)a->value;
        int64_t h = tk_arr_push(n->attrs, pair);
        if (h) n->attrs = h;
    }
    /* Append to end so attributes appear in insertion order. */
    if (!n->attr_list) {
        n->attr_list = a;
    } else {
        TkAttr *cur = n->attr_list;
        while (cur->next) cur = cur->next;
        cur->next = a;
    }
}

static void attr_free(TkAttr *a)
{
    while (a) {
        TkAttr *next = a->next;
        free(a->name);
        free(a->value);
        free(a);
        a = next;
    }
}

/* Forward declaration for recursive free. */
void html_node_free(TkHtmlNode *node);

void html_node_free(TkHtmlNode *node)
{
    if (!node) return;
    TkHtmlNode *child = node->first_child;
    while (child) {
        TkHtmlNode *next = child->next_sibling;
        html_node_free(child);
        child = next;
    }
    attr_free(node->attr_list);
    free(node->tag);
    free(node->content);
    free(node);
}

/* -----------------------------------------------------------------------
 * Rendering helpers
 * ----------------------------------------------------------------------- */

/* Forward declaration. */
static void render_node(TkHtmlNode *node, Buf *b);

/* 124.3 (ADR-0011): URL-context attribute check. Attributes whose value is a URL
 * can carry a `javascript:`/`vbscript:`/`data:` scheme that HTML-escaping does NOT
 * neutralize (the scheme has no HTML-special chars), giving script execution on
 * click/load. These are detected so the dangerous value can be dropped. */
static int is_url_attr(const char *name) {
    static const char *urlattrs[] = {
        "href", "src", "action", "formaction", "poster",
        "cite", "background", "xlink:href", NULL
    };
    for (int i = 0; urlattrs[i]; i++)
        if (strcmp(name, urlattrs[i]) == 0) return 1;
    return 0;
}
static int ci_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}
static int is_dangerous_url(const char *val) {
    if (!val) return 0;
    while (*val && (unsigned char)*val <= 0x20) val++;   /* leading ws/controls */
    /* scheme compare tolerating embedded whitespace/controls (browsers do too) */
    static const char *bad[] = { "javascript:", "vbscript:", NULL };
    for (int i = 0; bad[i]; i++) {
        const char *s = val; const char *bp = bad[i]; int ok = 1;
        while (*bp) {
            while (*s && (unsigned char)*s <= 0x20) s++;
            if (tolower((unsigned char)*s) != *bp) { ok = 0; break; }
            s++; bp++;
        }
        if (ok) return 1;
    }
    /* data: is dangerous except an image subtype (allow inline data:image/... URIs) */
    if (ci_starts(val, "data:") && !ci_starts(val, "data:image/")) return 1;
    return 0;
}

/* 124.3 (ADR-0011): append text into a raw-text element (<script>/<style>),
 * neutralizing a `</tag` breakout by inserting a backslash (`<\/tag`). The HTML
 * tokenizer then no longer sees a close tag, while JS/CSS treat `\/` as `/`, so
 * a `</script>`/`</style>` embedded in the content can't terminate the element
 * early and inject markup. `closetag` is e.g. "/script" (no leading '<'). */
static void buf_append_rawtext(Buf *b, const char *s, const char *closetag) {
    if (!s) return;
    for (const char *p = s; *p; ) {
        if (*p == '<' && ci_starts(p + 1, closetag)) {
            strbuf_append(b, "<\\");   /* emit "<\" then let "/tag" append normally */
            p++;
        } else {
            strbuf_putc(b, *p);
            p++;
        }
    }
}

static void render_attrs(TkHtmlNode *node, Buf *b)
{
    TkAttr *a = node->attr_list;
    while (a) {
        strbuf_putc(b, ' ');
        strbuf_append(b, a->name);
        strbuf_append(b, "=\"");
        /* 124.3 (ADR-0011): context-aware escaping — attribute values are emitted
         * in a double-quoted attribute context. A raw value containing `"` would
         * break out of the attribute and inject further attributes / event
         * handlers (XSS). html_escape covers & < > " ' — safe for quoted attrs. */
        const char *val = a->value ? a->value : "";
        /* URL-context: neutralize a dangerous scheme (javascript:, etc.) that
         * escaping alone would leave executable. */
        if (is_url_attr(a->name) && is_dangerous_url(val))
            val = "";
        const char *esc = html_escape(val);
        strbuf_append(b, esc ? esc : val);
        free((void *)esc);
        strbuf_putc(b, '"');
        a = a->next;
    }
}

static void render_children(TkHtmlNode *node, Buf *b)
{
    TkHtmlNode *child = node->first_child;
    while (child) {
        render_node(child, b);
        child = child->next_sibling;
    }
}

static void render_node(TkHtmlNode *node, Buf *b)
{
    if (!node) return;
    strbuf_putc(b, '<');
    strbuf_append(b, node->tag);
    render_attrs(node, b);
    if (node->void_element) {
        strbuf_append(b, ">");
        return;
    }
    strbuf_putc(b, '>');
    if (node->content) {
        strbuf_append(b, node->content);
    }
    render_children(node, b);
    strbuf_append(b, "</");
    strbuf_append(b, node->tag);
    strbuf_putc(b, '>');
}

/* -----------------------------------------------------------------------
 * TkHtmlDoc
 * ----------------------------------------------------------------------- */

/* Body node list entry. */
typedef struct TkBodyEntry {
    TkHtmlNode        *node;
    struct TkBodyEntry *next;
} TkBodyEntry;

struct TkHtmlDoc {
    TkHtmlDocHead head;       /* first: the $htmldoc record (135.30) */
    char        *title;       /* NULL if not set */
    char        *style;       /* NULL if not set */
    char        *script;      /* NULL if not set */
    TkBodyEntry *body_head;
    TkBodyEntry *body_tail;
    char        *rendered;    /* cached render result (freed on next render) */
};

TkHtmlDoc *html_doc(void)
{
    static int64_t next_id = 0;
    TkHtmlDoc *doc = (TkHtmlDoc *)calloc(1, sizeof(TkHtmlDoc));
    if (doc) doc->head.id = ++next_id;
    return doc;
}

void html_title(TkHtmlDoc *doc, const char *title)
{
    if (!doc || !title) return;
    free(doc->title);
    doc->title = strdup(title);
}

void html_style(TkHtmlDoc *doc, const char *css)
{
    if (!doc || !css) return;
    free(doc->style);
    doc->style = strdup(css);
}

void html_script(TkHtmlDoc *doc, const char *js)
{
    if (!doc || !js) return;
    free(doc->script);
    doc->script = strdup(js);
}

void html_append(TkHtmlDoc *doc, TkHtmlNode *node)
{
    if (!doc || !node) return;
    TkBodyEntry *entry = (TkBodyEntry *)malloc(sizeof(TkBodyEntry));
    if (!entry) return;
    entry->node = node;
    entry->next = NULL;
    if (!doc->body_tail) {
        doc->body_head = entry;
        doc->body_tail = entry;
    } else {
        doc->body_tail->next = entry;
        doc->body_tail       = entry;
    }
}

void html_append_child(TkHtmlNode *parent, TkHtmlNode *child)
{
    if (!parent || !child) return;
    if (!parent->last_child) {
        parent->first_child = child;
        parent->last_child  = child;
    } else {
        parent->last_child->next_sibling = child;
        parent->last_child               = child;
    }
}

const char *html_render(TkHtmlDoc *doc)
{
    if (!doc) return NULL;
    free(doc->rendered);
    doc->rendered = NULL;

    Buf b = TK_STRBUF_INIT;

    strbuf_append(&b, "<!DOCTYPE html>\n<html>\n<head>\n");

    if (doc->title) {
        strbuf_append(&b, "<title>");
        { const char *te = html_escape(doc->title);   /* title is HTML text */
          strbuf_append(&b, te ? te : doc->title); free((void *)te); }
        strbuf_append(&b, "</title>\n");
    }

    if (doc->style) {
        strbuf_append(&b, "<style>");
        buf_append_rawtext(&b, doc->style, "/style");
        strbuf_append(&b, "</style>\n");
    }

    if (doc->script) {
        strbuf_append(&b, "<script>");
        buf_append_rawtext(&b, doc->script, "/script");
        strbuf_append(&b, "</script>\n");
    }

    strbuf_append(&b, "</head>\n<body>\n");

    TkBodyEntry *entry = doc->body_head;
    while (entry) {
        render_node(entry->node, &b);
        strbuf_putc(&b, '\n');
        entry = entry->next;
    }

    strbuf_append(&b, "</body>\n</html>");

    doc->rendered = strbuf_take(&b);
    return doc->rendered;
}

void html_free(TkHtmlDoc *doc)
{
    if (!doc) return;
    free(doc->title);
    free(doc->style);
    free(doc->script);
    free(doc->rendered);
    TkBodyEntry *entry = doc->body_head;
    while (entry) {
        TkBodyEntry *next = entry->next;
        html_node_free(entry->node);
        free(entry);
        entry = next;
    }
    free(doc);
}

/* -----------------------------------------------------------------------
 * Node constructors
 * ----------------------------------------------------------------------- */

TkHtmlNode *html_div(const char *class_, const char *content)
{
    TkHtmlNode *n = node_new("div", 0);
    if (!n) return NULL;
    if (class_) node_add_attr(n, "class", class_);
    if (content) node_set_text(n, content);
    return n;
}

TkHtmlNode *html_p(const char *content)
{
    TkHtmlNode *n = node_new("p", 0);
    if (!n) return NULL;
    if (content) node_set_text(n, content);
    return n;
}

TkHtmlNode *html_h1(const char *content)
{
    TkHtmlNode *n = node_new("h1", 0);
    if (!n) return NULL;
    if (content) node_set_text(n, content);
    return n;
}

TkHtmlNode *html_h2(const char *content)
{
    TkHtmlNode *n = node_new("h2", 0);
    if (!n) return NULL;
    if (content) node_set_text(n, content);
    return n;
}

TkHtmlNode *html_span(const char *class_, const char *content)
{
    TkHtmlNode *n = node_new("span", 0);
    if (!n) return NULL;
    if (class_) node_add_attr(n, "class", class_);
    if (content) node_set_text(n, content);
    return n;
}

TkHtmlNode *html_a(const char *href, const char *text)
{
    TkHtmlNode *n = node_new("a", 0);
    if (!n) return NULL;
    if (href) node_add_attr(n, "href", href);
    if (text) node_set_text(n, text);
    return n;
}

TkHtmlNode *html_img(const char *src, const char *alt)
{
    TkHtmlNode *n = node_new("img", 1);  /* void element */
    if (!n) return NULL;
    if (src) node_add_attr(n, "src", src);
    if (alt) node_add_attr(n, "alt", alt);
    return n;
}

TkHtmlNode *html_table(const char **headers, uint64_t ncols,
                        const char **rows, uint64_t nrows)
{
    TkHtmlNode *table = node_new("table", 0);
    if (!table) return NULL;

    /* <thead> */
    TkHtmlNode *thead = node_new("thead", 0);
    if (!thead) { html_node_free(table); return NULL; }
    html_append_child(table, thead);

    TkHtmlNode *hrow = node_new("tr", 0);
    if (!hrow) { html_node_free(table); return NULL; }
    html_append_child(thead, hrow);

    for (uint64_t c = 0; c < ncols; c++) {
        TkHtmlNode *th = node_new("th", 0);
        if (!th) { html_node_free(table); return NULL; }
        if (headers && headers[c]) node_set_text(th, headers[c]);
        html_append_child(hrow, th);
    }

    /* <tbody> */
    TkHtmlNode *tbody = node_new("tbody", 0);
    if (!tbody) { html_node_free(table); return NULL; }
    html_append_child(table, tbody);

    /* nrows is number of data rows; cells are flat row-major: rows[r*ncols+c] */
    for (uint64_t r = 0; r < nrows; r++) {
        TkHtmlNode *tr = node_new("tr", 0);
        if (!tr) { html_node_free(table); return NULL; }
        html_append_child(tbody, tr);
        for (uint64_t c = 0; c < ncols; c++) {
            TkHtmlNode *td = node_new("td", 0);
            if (!td) { html_node_free(table); return NULL; }
            if (rows) {
                const char *cell = rows[r * ncols + c];
                if (cell) node_set_text(td, cell);
            }
            html_append_child(tr, td);
        }
    }

    return table;
}

/* -----------------------------------------------------------------------
 * Utilities
 * ----------------------------------------------------------------------- */

const char *html_escape(const char *s)
{
    if (!s) return NULL;

    /* First pass: count extra bytes needed. */
    size_t extra = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&':  extra += 4; break;  /* &amp;  = 5 chars, was 1 → +4 */
            case '<':  extra += 3; break;  /* &lt;   = 4 chars → +3 */
            case '>':  extra += 3; break;  /* &gt;   = 4 chars → +3 */
            case '"':  extra += 5; break;  /* &quot; = 6 chars → +5 */
            case '\'': extra += 4; break;  /* &#39;  = 5 chars → +4 */
            default:   break;
        }
    }

    size_t srclen = strlen(s);
    char  *out    = (char *)malloc(srclen + extra + 1);
    if (!out) return NULL;

    char *dst = out;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&':
                memcpy(dst, "&amp;",  5); dst += 5; break;
            case '<':
                memcpy(dst, "&lt;",   4); dst += 4; break;
            case '>':
                memcpy(dst, "&gt;",   4); dst += 4; break;
            case '"':
                memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'':
                memcpy(dst, "&#39;",  5); dst += 5; break;
            default:
                *dst++ = *p; break;
        }
    }
    *dst = '\0';
    return out;
}

const char *html_node_render(TkHtmlNode *node)
{
    if (!node) return NULL;
    Buf b = TK_STRBUF_INIT;
    render_node(node, &b);
    return strbuf_take(&b);
}

/* -----------------------------------------------------------------------
 * Internal helper: append uint64 as decimal string
 * ----------------------------------------------------------------------- */

static void buf_append_u64(Buf *b, uint64_t v)
{
    char tmp[24];
    int  len = 0;
    if (v == 0) {
        strbuf_putc(b, '0');
        return;
    }
    while (v > 0) {
        tmp[len++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    /* tmp is reversed */
    for (int i = len - 1; i >= 0; i--) {
        strbuf_putc(b, tmp[i]);
    }
}

/* -----------------------------------------------------------------------
 * String-based element constructors (Story 34.1.1)
 * ----------------------------------------------------------------------- */

const char *html_form(const char *action, const char *method,
                      const char *const *children, uint64_t nchild)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<form action=\"");
    if (action) strbuf_append(&b, action);
    strbuf_append(&b, "\" method=\"");
    if (method) strbuf_append(&b, method);
    strbuf_append(&b, "\">\n");
    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i]) {
            strbuf_append(&b, children[i]);
            strbuf_putc(&b, '\n');
        }
    }
    strbuf_append(&b, "</form>");
    return strbuf_take(&b);
}

const char *html_input(const char *type, const char *name, const char *value)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<input type=\"");
    if (type)  strbuf_append(&b, type);
    strbuf_append(&b, "\" name=\"");
    if (name)  strbuf_append(&b, name);
    strbuf_append(&b, "\" value=\"");
    if (value) strbuf_append(&b, value);
    strbuf_append(&b, "\">");
    return strbuf_take(&b);
}

const char *html_select(const char *name,
                        const char *const *options, uint64_t nopts,
                        const char *selected)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<select name=\"");
    if (name) strbuf_append(&b, name);
    strbuf_append(&b, "\">\n");
    for (uint64_t i = 0; i < nopts; i++) {
        const char *opt = (options && options[i]) ? options[i] : "";
        strbuf_append(&b, "<option value=\"");
        strbuf_append(&b, opt);
        strbuf_putc(&b, '"');
        if (selected && strcmp(opt, selected) == 0) {
            strbuf_append(&b, " selected");
        }
        strbuf_putc(&b, '>');
        strbuf_append(&b, opt);
        strbuf_append(&b, "</option>\n");
    }
    strbuf_append(&b, "</select>");
    return strbuf_take(&b);
}

const char *html_textarea(const char *name, const char *content,
                           uint64_t rows, uint64_t cols)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<textarea name=\"");
    if (name) strbuf_append(&b, name);
    strbuf_append(&b, "\" rows=\"");
    buf_append_u64(&b, rows);
    strbuf_append(&b, "\" cols=\"");
    buf_append_u64(&b, cols);
    strbuf_append(&b, "\">");
    if (content) strbuf_append(&b, content);
    strbuf_append(&b, "</textarea>");
    return strbuf_take(&b);
}

const char *html_button(const char *text, const char *type)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<button type=\"");
    if (type) strbuf_append(&b, type);
    strbuf_append(&b, "\">");
    if (text) strbuf_append(&b, text);
    strbuf_append(&b, "</button>");
    return strbuf_take(&b);
}

const char *html_label(const char *for_id, const char *text)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<label for=\"");
    if (for_id) strbuf_append(&b, for_id);
    strbuf_append(&b, "\">");
    if (text) strbuf_append(&b, text);
    strbuf_append(&b, "</label>");
    return strbuf_take(&b);
}

static const char *list_build(const char *tag,
                               const char *const *items, uint64_t n)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_putc(&b, '<');
    strbuf_append(&b, tag);
    strbuf_append(&b, ">\n");
    for (uint64_t i = 0; i < n; i++) {
        strbuf_append(&b, "<li>");
        if (items && items[i]) strbuf_append(&b, items[i]);
        strbuf_append(&b, "</li>\n");
    }
    strbuf_append(&b, "</");
    strbuf_append(&b, tag);
    strbuf_putc(&b, '>');
    return strbuf_take(&b);
}

const char *html_ul(const char *const *items, uint64_t n)
{
    return list_build("ul", items, n);
}

const char *html_ol(const char *const *items, uint64_t n)
{
    return list_build("ol", items, n);
}

const char *html_br(void)
{
    return strdup("<br>");
}

const char *html_hr(void)
{
    return strdup("<hr>");
}

const char *html_pre(const char *content)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<pre>");
    if (content) strbuf_append(&b, content);
    strbuf_append(&b, "</pre>");
    return strbuf_take(&b);
}

const char *html_code(const char *content)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<code>");
    if (content) strbuf_append(&b, content);
    strbuf_append(&b, "</code>");
    return strbuf_take(&b);
}

/* -----------------------------------------------------------------------
 * String-based attribute helpers (Story 34.1.2)
 *
 * html_attr, html_class_add, html_id: parse the HTML string to find the
 * first '>' and insert the attribute before it.
 * ----------------------------------------------------------------------- */

/*
 * Insert " name=\"value\"" before the first '>' in `node`.
 * Returns a new heap-allocated string.
 */
static const char *insert_attr_before_gt(const char *node,
                                          const char *name,
                                          const char *value)
{
    if (!node || !name || !value) return node ? strdup(node) : NULL;

    const char *gt = strchr(node, '>');
    if (!gt) {
        /* No '>' found; just return a copy unchanged. */
        return strdup(node);
    }

    /* Build:  prefix + " name=\"value\"" + suffix */
    size_t prefix_len = (size_t)(gt - node);
    size_t name_len   = strlen(name);
    size_t value_len  = strlen(value);
    size_t suffix_len = strlen(gt);   /* includes '>' */

    /* " name=\"value\"" = 1 + name_len + 2 + value_len + 1 */
    size_t insert_len = 1 + name_len + 2 + value_len + 1;
    size_t total      = prefix_len + insert_len + suffix_len + 1;

    char *out = (char *)malloc(total);
    if (!out) return NULL;

    char *p = out;
    memcpy(p, node, prefix_len); p += prefix_len;
    *p++ = ' ';
    memcpy(p, name, name_len);   p += name_len;
    *p++ = '=';
    *p++ = '"';
    memcpy(p, value, value_len); p += value_len;
    *p++ = '"';
    memcpy(p, gt, suffix_len);   p += suffix_len;
    *p = '\0';
    return out;
}

const char *html_attr(const char *node, const char *name, const char *value)
{
    return insert_attr_before_gt(node, name, value);
}

const char *html_class_add(const char *node, const char *cls)
{
    if (!node || !cls) return node ? strdup(node) : NULL;

    /* Look for class="..." in the string (within the opening tag only). */
    const char *gt  = strchr(node, '>');
    size_t tag_len  = gt ? (size_t)(gt - node) : strlen(node);

    /* Search for ' class="' within the tag portion. */
    const char *found = NULL;
    {
        /* Manual scan limited to tag_len characters. */
        const char *needle = "class=\"";
        size_t nlen = strlen(needle);
        for (size_t i = 0; i + nlen <= tag_len; i++) {
            if (memcmp(node + i, needle, nlen) == 0) {
                found = node + i;
                break;
            }
        }
    }

    if (found) {
        /*
         * Append " cls" to the existing class value.
         * found points to "class=\"...value...\""
         * We want to insert " cls" before the closing '"'.
         */
        const char *class_val_start = found + 7;  /* skip 'class="' */
        const char *closing_quote   = strchr(class_val_start, '"');
        if (!closing_quote) {
            /* Malformed; fall back to inserting a new attribute. */
            return insert_attr_before_gt(node, "class", cls);
        }

        /* Build new string with " cls" inserted before closing_quote. */
        size_t before_len  = (size_t)(closing_quote - node);
        size_t cls_len     = strlen(cls);
        size_t after_len   = strlen(closing_quote); /* includes '"' */
        size_t total       = before_len + 1 + cls_len + after_len + 1;

        char *out = (char *)malloc(total);
        if (!out) return NULL;

        char *p = out;
        memcpy(p, node, before_len);    p += before_len;
        *p++ = ' ';
        memcpy(p, cls, cls_len);        p += cls_len;
        memcpy(p, closing_quote, after_len); p += after_len;
        *p = '\0';
        return out;
    }

    /* No existing class attribute — add one. */
    return insert_attr_before_gt(node, "class", cls);
}

const char *html_id(const char *node, const char *id)
{
    return insert_attr_before_gt(node, "id", id);
}

const char *html_meta(const char *name, const char *content)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<meta name=\"");
    if (name)    strbuf_append(&b, name);
    strbuf_append(&b, "\" content=\"");
    if (content) strbuf_append(&b, content);
    strbuf_append(&b, "\">");
    return strbuf_take(&b);
}

const char *html_link_stylesheet(const char *href)
{
    Buf b = TK_STRBUF_INIT;
    strbuf_append(&b, "<link rel=\"stylesheet\" href=\"");
    if (href) strbuf_append(&b, href);
    strbuf_append(&b, "\">");
    return strbuf_take(&b);
}
