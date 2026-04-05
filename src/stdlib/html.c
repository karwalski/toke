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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal helper: growable string buffer
 * ----------------------------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Buf;

static void buf_init(Buf *b)
{
    b->cap  = 256;
    b->len  = 0;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void buf_grow(Buf *b, size_t needed)
{
    while (b->len + needed + 1 > b->cap) {
        b->cap *= 2;
        b->data = (char *)realloc(b->data, b->cap);
    }
}

static void buf_append(Buf *b, const char *s)
{
    if (!s) return;
    size_t slen = strlen(s);
    buf_grow(b, slen);
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
    b->data[b->len] = '\0';
}

static void buf_appendc(Buf *b, char c)
{
    buf_grow(b, 1);
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

/* Return ownership of buffer's data (caller must free). */
static char *buf_take(Buf *b)
{
    char *p = b->data;
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
    return p;
}

/* -----------------------------------------------------------------------
 * TkHtmlNode
 * ----------------------------------------------------------------------- */

/* Attribute: name="value" pair. */
typedef struct TkAttr {
    char          *name;
    char          *value;
    struct TkAttr *next;
} TkAttr;

struct TkHtmlNode {
    char          *tag;           /* e.g. "div", "p", "table" */
    TkAttr        *attrs;         /* linked list of attributes */
    char          *text_content;  /* text inside element (may be NULL) */
    TkHtmlNode    *first_child;   /* first child node */
    TkHtmlNode    *last_child;    /* last child node (for O(1) append) */
    TkHtmlNode    *next_sibling;  /* next sibling in parent */
    int            void_element;  /* 1 = self-closing (img, etc.) */
};

static TkHtmlNode *node_new(const char *tag, int void_element)
{
    TkHtmlNode *n = (TkHtmlNode *)calloc(1, sizeof(TkHtmlNode));
    if (!n) return NULL;
    n->tag         = strdup(tag);
    n->void_element = void_element;
    return n;
}

static void node_add_attr(TkHtmlNode *n, const char *name, const char *value)
{
    if (!name || !value) return;
    TkAttr *a = (TkAttr *)malloc(sizeof(TkAttr));
    if (!a) return;
    a->name  = strdup(name);
    a->value = strdup(value);
    a->next  = NULL;
    /* Append to end so attributes appear in insertion order. */
    if (!n->attrs) {
        n->attrs = a;
    } else {
        TkAttr *cur = n->attrs;
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
    attr_free(node->attrs);
    free(node->tag);
    free(node->text_content);
    free(node);
}

/* -----------------------------------------------------------------------
 * Rendering helpers
 * ----------------------------------------------------------------------- */

/* Forward declaration. */
static void render_node(TkHtmlNode *node, Buf *b);

static void render_attrs(TkHtmlNode *node, Buf *b)
{
    TkAttr *a = node->attrs;
    while (a) {
        buf_appendc(b, ' ');
        buf_append(b, a->name);
        buf_append(b, "=\"");
        buf_append(b, a->value);
        buf_appendc(b, '"');
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
    buf_appendc(b, '<');
    buf_append(b, node->tag);
    render_attrs(node, b);
    if (node->void_element) {
        buf_append(b, ">");
        return;
    }
    buf_appendc(b, '>');
    if (node->text_content) {
        buf_append(b, node->text_content);
    }
    render_children(node, b);
    buf_append(b, "</");
    buf_append(b, node->tag);
    buf_appendc(b, '>');
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
    char        *title;       /* NULL if not set */
    char        *style;       /* NULL if not set */
    char        *script;      /* NULL if not set */
    TkBodyEntry *body_head;
    TkBodyEntry *body_tail;
    char        *rendered;    /* cached render result (freed on next render) */
};

TkHtmlDoc *html_doc(void)
{
    TkHtmlDoc *doc = (TkHtmlDoc *)calloc(1, sizeof(TkHtmlDoc));
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

    Buf b;
    buf_init(&b);

    buf_append(&b, "<!DOCTYPE html>\n<html>\n<head>\n");

    if (doc->title) {
        buf_append(&b, "<title>");
        buf_append(&b, doc->title);
        buf_append(&b, "</title>\n");
    }

    if (doc->style) {
        buf_append(&b, "<style>");
        buf_append(&b, doc->style);
        buf_append(&b, "</style>\n");
    }

    if (doc->script) {
        buf_append(&b, "<script>");
        buf_append(&b, doc->script);
        buf_append(&b, "</script>\n");
    }

    buf_append(&b, "</head>\n<body>\n");

    TkBodyEntry *entry = doc->body_head;
    while (entry) {
        render_node(entry->node, &b);
        buf_appendc(&b, '\n');
        entry = entry->next;
    }

    buf_append(&b, "</body>\n</html>");

    doc->rendered = buf_take(&b);
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
    if (content) n->text_content = strdup(content);
    return n;
}

TkHtmlNode *html_p(const char *content)
{
    TkHtmlNode *n = node_new("p", 0);
    if (!n) return NULL;
    if (content) n->text_content = strdup(content);
    return n;
}

TkHtmlNode *html_h1(const char *content)
{
    TkHtmlNode *n = node_new("h1", 0);
    if (!n) return NULL;
    if (content) n->text_content = strdup(content);
    return n;
}

TkHtmlNode *html_h2(const char *content)
{
    TkHtmlNode *n = node_new("h2", 0);
    if (!n) return NULL;
    if (content) n->text_content = strdup(content);
    return n;
}

TkHtmlNode *html_span(const char *class_, const char *content)
{
    TkHtmlNode *n = node_new("span", 0);
    if (!n) return NULL;
    if (class_) node_add_attr(n, "class", class_);
    if (content) n->text_content = strdup(content);
    return n;
}

TkHtmlNode *html_a(const char *href, const char *text)
{
    TkHtmlNode *n = node_new("a", 0);
    if (!n) return NULL;
    if (href) node_add_attr(n, "href", href);
    if (text) n->text_content = strdup(text);
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
        if (headers && headers[c]) th->text_content = strdup(headers[c]);
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
                if (cell) td->text_content = strdup(cell);
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
    Buf b;
    buf_init(&b);
    render_node(node, &b);
    return buf_take(&b);
}

/* -----------------------------------------------------------------------
 * Internal helper: append uint64 as decimal string
 * ----------------------------------------------------------------------- */

static void buf_append_u64(Buf *b, uint64_t v)
{
    char tmp[24];
    int  len = 0;
    if (v == 0) {
        buf_appendc(b, '0');
        return;
    }
    while (v > 0) {
        tmp[len++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    /* tmp is reversed */
    for (int i = len - 1; i >= 0; i--) {
        buf_appendc(b, tmp[i]);
    }
}

/* -----------------------------------------------------------------------
 * String-based element constructors (Story 34.1.1)
 * ----------------------------------------------------------------------- */

const char *html_form(const char *action, const char *method,
                      const char *const *children, uint64_t nchild)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<form action=\"");
    if (action) buf_append(&b, action);
    buf_append(&b, "\" method=\"");
    if (method) buf_append(&b, method);
    buf_append(&b, "\">\n");
    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i]) {
            buf_append(&b, children[i]);
            buf_appendc(&b, '\n');
        }
    }
    buf_append(&b, "</form>");
    return buf_take(&b);
}

const char *html_input(const char *type, const char *name, const char *value)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<input type=\"");
    if (type)  buf_append(&b, type);
    buf_append(&b, "\" name=\"");
    if (name)  buf_append(&b, name);
    buf_append(&b, "\" value=\"");
    if (value) buf_append(&b, value);
    buf_append(&b, "\">");
    return buf_take(&b);
}

const char *html_select(const char *name,
                        const char *const *options, uint64_t nopts,
                        const char *selected)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<select name=\"");
    if (name) buf_append(&b, name);
    buf_append(&b, "\">\n");
    for (uint64_t i = 0; i < nopts; i++) {
        const char *opt = (options && options[i]) ? options[i] : "";
        buf_append(&b, "<option value=\"");
        buf_append(&b, opt);
        buf_appendc(&b, '"');
        if (selected && strcmp(opt, selected) == 0) {
            buf_append(&b, " selected");
        }
        buf_appendc(&b, '>');
        buf_append(&b, opt);
        buf_append(&b, "</option>\n");
    }
    buf_append(&b, "</select>");
    return buf_take(&b);
}

const char *html_textarea(const char *name, const char *content,
                           uint64_t rows, uint64_t cols)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<textarea name=\"");
    if (name) buf_append(&b, name);
    buf_append(&b, "\" rows=\"");
    buf_append_u64(&b, rows);
    buf_append(&b, "\" cols=\"");
    buf_append_u64(&b, cols);
    buf_append(&b, "\">");
    if (content) buf_append(&b, content);
    buf_append(&b, "</textarea>");
    return buf_take(&b);
}

const char *html_button(const char *text, const char *type)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<button type=\"");
    if (type) buf_append(&b, type);
    buf_append(&b, "\">");
    if (text) buf_append(&b, text);
    buf_append(&b, "</button>");
    return buf_take(&b);
}

const char *html_label(const char *for_id, const char *text)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<label for=\"");
    if (for_id) buf_append(&b, for_id);
    buf_append(&b, "\">");
    if (text) buf_append(&b, text);
    buf_append(&b, "</label>");
    return buf_take(&b);
}

static const char *list_build(const char *tag,
                               const char *const *items, uint64_t n)
{
    Buf b;
    buf_init(&b);
    buf_appendc(&b, '<');
    buf_append(&b, tag);
    buf_append(&b, ">\n");
    for (uint64_t i = 0; i < n; i++) {
        buf_append(&b, "<li>");
        if (items && items[i]) buf_append(&b, items[i]);
        buf_append(&b, "</li>\n");
    }
    buf_append(&b, "</");
    buf_append(&b, tag);
    buf_appendc(&b, '>');
    return buf_take(&b);
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
    Buf b;
    buf_init(&b);
    buf_append(&b, "<pre>");
    if (content) buf_append(&b, content);
    buf_append(&b, "</pre>");
    return buf_take(&b);
}

const char *html_code(const char *content)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<code>");
    if (content) buf_append(&b, content);
    buf_append(&b, "</code>");
    return buf_take(&b);
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
    Buf b;
    buf_init(&b);
    buf_append(&b, "<meta name=\"");
    if (name)    buf_append(&b, name);
    buf_append(&b, "\" content=\"");
    if (content) buf_append(&b, content);
    buf_append(&b, "\">");
    return buf_take(&b);
}

const char *html_link_stylesheet(const char *href)
{
    Buf b;
    buf_init(&b);
    buf_append(&b, "<link rel=\"stylesheet\" href=\"");
    if (href) buf_append(&b, href);
    buf_append(&b, "\">");
    return buf_take(&b);
}
