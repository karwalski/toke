/* SPDX-License-Identifier: MIT */
/*
 * template.c — Implementation of the std.template standard library module.
 *
 * Implements {{IDENT}} slot-based string templating with block tags:
 *   {{#if key}}...{{/if}}
 *   {{#unless key}}...{{/unless}}
 *   {{#each key}}...{{/each}}
 *
 * Each compiled template is stored as an array of nodes; each node is a
 * LITERAL, SLOT, IF, UNLESS, or EACH.  Block nodes own a child node array
 * that is rendered conditionally or iteratively.
 *
 * Slot syntax: {{IDENT}} where IDENT matches [A-Za-z_][A-Za-z0-9_]*
 * Special slots inside EACH blocks: {{.}} (current item), {{@index}} (index).
 * Any {{ not matching a known tag is treated as a literal.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary.
 * Callers own returned strings.
 *
 * Story: 15.1.4, 33.1.1, 33.1.2
 */

#include "template.h"
#include "strbuf.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>   /* strncasecmp (121.37 URL scheme check) */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal node representation
 * ----------------------------------------------------------------------- */

typedef enum {
    NODE_LITERAL,  /* fixed text */
    NODE_SLOT,     /* {{IDENT}} variable slot; optionally with a helper */
    NODE_IF,       /* {{#if key}}...{{/if}} */
    NODE_UNLESS,   /* {{#unless key}}...{{/unless}} */
    NODE_EACH,     /* {{#each key}}...{{/each}} */
    NODE_PARTIAL   /* {{>name}} include a registered partial */
} NodeKind;

/* Forward declaration for recursive struct */
typedef struct Node Node;

struct Node {
    NodeKind  kind;
    char     *text;        /* LITERAL: literal text; SLOT/IF/UNLESS/EACH/PARTIAL: key/name */
    char     *helper_name; /* SLOT: optional helper name from {{key|helper}} syntax */
    Node     *children;    /* block nodes: child node array */
    uint64_t  nchild;      /* number of children */
};

struct TkTmpl {
    Node     *nodes;
    uint64_t  nnode;
};

/* -----------------------------------------------------------------------
 * Global partial registry (Story 33.1.2)
 * ----------------------------------------------------------------------- */

static struct { const char *name; TkTmpl *compiled; } g_partials[64];
static int g_npartials = 0;

/* -----------------------------------------------------------------------
 * Global helper registry (Story 33.1.2)
 * ----------------------------------------------------------------------- */

static struct { const char *name; TkTmplHelperFn fn; } g_helpers[64];
static int g_nhelpers = 0;

/* -----------------------------------------------------------------------
 * Character classification helpers
 * ----------------------------------------------------------------------- */

static int ident_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ident_cont(char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

/* -----------------------------------------------------------------------
 * Tag parsing helpers
 * ----------------------------------------------------------------------- */

/*
 * skip_ws — advance *pp past spaces/tabs.
 */
static void skip_ws(const char **pp)
{
    while (**pp == ' ' || **pp == '\t') (*pp)++;
}

/* Tag kind identifiers returned by try_parse_tag */
typedef enum {
    TAG_NONE,
    TAG_SLOT,        /* {{IDENT}} or {{IDENT|helper}} */
    TAG_PARTIAL,     /* {{>name}} */
    TAG_OPEN_IF,     /* {{#if key}} */
    TAG_OPEN_UNLESS, /* {{#unless key}} */
    TAG_OPEN_EACH,   /* {{#each key}} */
    TAG_CLOSE_IF,    /* {{/if}} */
    TAG_CLOSE_UNLESS,/* {{/unless}} */
    TAG_CLOSE_EACH   /* {{/each}} */
} TagKind;

/*
 * try_parse_tag — attempt to parse a template tag starting at *pp which
 * already points past the opening "{{".
 *
 * On success sets *out_kind, *out_key (heap-allocated, caller frees; may
 * be NULL for close tags), and *out_helper (heap-allocated helper name for
 * TAG_SLOT with pipe syntax, otherwise NULL) and advances *pp past "}}".
 * Returns 1 on success, 0 on failure (pp not modified on failure).
 */
static int try_parse_tag(const char *src, size_t *out_consumed,
                         TagKind *out_kind, char **out_key,
                         char **out_helper)
{
    const char *p = src;
    *out_key = NULL;
    *out_kind = TAG_NONE;
    *out_helper = NULL;

    /* Check for partial include: {{>name}} */
    if (p[0] == '>') {
        p++;
        skip_ws(&p);
        const char *name_start = p;
        while (*p && ident_cont(*p)) p++;
        size_t name_len = (size_t)(p - name_start);
        if (name_len == 0) return 0;
        skip_ws(&p);
        if (p[0] != '}' || p[1] != '}') return 0;
        p += 2;
        char *key = (char *)malloc(name_len + 1);
        if (!key) return 0;
        memcpy(key, name_start, name_len);
        key[name_len] = '\0';
        *out_key  = key;
        *out_kind = TAG_PARTIAL;
        *out_consumed = (size_t)(p - src);
        return 1;
    }

    /* Check for block open: {{#word key}} */
    if (p[0] == '#') {
        p++;
        /* read the directive word */
        const char *dir_start = p;
        while (*p && ident_cont(*p)) p++;
        size_t dir_len = (size_t)(p - dir_start);
        if (dir_len == 0) return 0;

        skip_ws(&p);

        /* read the key */
        const char *key_start = p;
        while (*p && ident_cont(*p)) p++;
        size_t key_len = (size_t)(p - key_start);
        if (key_len == 0) return 0;

        skip_ws(&p);
        if (p[0] != '}' || p[1] != '}') return 0;
        p += 2;

        /* determine directive */
        if (dir_len == 2 && memcmp(dir_start, "if", 2) == 0)
            *out_kind = TAG_OPEN_IF;
        else if (dir_len == 6 && memcmp(dir_start, "unless", 6) == 0)
            *out_kind = TAG_OPEN_UNLESS;
        else if (dir_len == 4 && memcmp(dir_start, "each", 4) == 0)
            *out_kind = TAG_OPEN_EACH;
        else
            return 0;

        char *key = (char *)malloc(key_len + 1);
        if (!key) return 0;
        memcpy(key, key_start, key_len);
        key[key_len] = '\0';
        *out_key = key;
        *out_consumed = (size_t)(p - src);
        return 1;
    }

    /* Check for block close: {{/word}} */
    if (p[0] == '/') {
        p++;
        const char *dir_start = p;
        while (*p && ident_cont(*p)) p++;
        size_t dir_len = (size_t)(p - dir_start);
        if (dir_len == 0) return 0;

        skip_ws(&p);
        if (p[0] != '}' || p[1] != '}') return 0;
        p += 2;

        if (dir_len == 2 && memcmp(dir_start, "if", 2) == 0)
            *out_kind = TAG_CLOSE_IF;
        else if (dir_len == 6 && memcmp(dir_start, "unless", 6) == 0)
            *out_kind = TAG_CLOSE_UNLESS;
        else if (dir_len == 4 && memcmp(dir_start, "each", 4) == 0)
            *out_kind = TAG_CLOSE_EACH;
        else
            return 0;

        *out_consumed = (size_t)(p - src);
        return 1;
    }

    /* Check for special EACH-context slots: {{.}} and {{@index}} */
    if (p[0] == '.') {
        if (p[1] == '}' && p[2] == '}') {
            char *key = (char *)malloc(2);
            if (!key) return 0;
            key[0] = '.';
            key[1] = '\0';
            *out_key = key;
            *out_kind = TAG_SLOT;
            *out_consumed = 3; /* . + }} */
            return 1;
        }
        return 0;
    }
    if (p[0] == '@') {
        p++;
        const char *key_start = p;
        while (*p && ident_cont(*p)) p++;
        size_t key_len = (size_t)(p - key_start);
        if (key_len == 0) return 0;
        if (p[0] != '}' || p[1] != '}') return 0;
        p += 2;
        /* build "@name" key */
        char *key = (char *)malloc(1 + key_len + 1);
        if (!key) return 0;
        key[0] = '@';
        memcpy(key + 1, key_start, key_len);
        key[1 + key_len] = '\0';
        *out_key = key;
        *out_kind = TAG_SLOT;
        *out_consumed = (size_t)(p - src);
        return 1;
    }

    /* Plain IDENT slot, optionally with {{key|helper}} */
    if (!ident_start(p[0])) return 0;
    const char *key_start = p;
    size_t n = 0;
    while (p[n] && ident_cont(p[n])) n++;

    /* After the key there may be a pipe introducing a helper name */
    if (p[n] == '|') {
        /* parse helper name */
        const char *hlp_start = p + n + 1;
        size_t hn = 0;
        while (hlp_start[hn] && ident_cont(hlp_start[hn])) hn++;
        if (hn == 0) return 0;
        if (hlp_start[hn] != '}' || hlp_start[hn + 1] != '}') return 0;

        char *key = (char *)malloc(n + 1);
        if (!key) return 0;
        memcpy(key, key_start, n);
        key[n] = '\0';

        char *hlp = (char *)malloc(hn + 1);
        if (!hlp) { free(key); return 0; }
        memcpy(hlp, hlp_start, hn);
        hlp[hn] = '\0';

        *out_key     = key;
        *out_helper  = hlp;
        *out_kind    = TAG_SLOT;
        *out_consumed = n + 1 + hn + 2; /* key + '|' + helper + '}}' */
        return 1;
    }

    if (p[n] != '}' || p[n + 1] != '}') return 0;

    char *key = (char *)malloc(n + 1);
    if (!key) return 0;
    memcpy(key, key_start, n);
    key[n] = '\0';
    *out_key = key;
    *out_kind = TAG_SLOT;
    *out_consumed = n + 2;
    return 1;
}

/* -----------------------------------------------------------------------
 * Node array helpers
 * ----------------------------------------------------------------------- */

static void free_nodes(Node *nodes, uint64_t nnode);

static int append_node(Node **arr, uint64_t *n, uint64_t *cap, Node nd)
{
    if (*n == *cap) {
        uint64_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        Node *tmp = (Node *)realloc(*arr, new_cap * sizeof(Node));
        if (!tmp) return 0;
        *arr = tmp;
        *cap = new_cap;
    }
    (*arr)[(*n)++] = nd;
    return 1;
}

static void free_node(Node *nd)
{
    if (!nd) return;
    free(nd->text);
    free(nd->helper_name);
    free_nodes(nd->children, nd->nchild);
}

static void free_nodes(Node *nodes, uint64_t nnode)
{
    if (!nodes) return;
    for (uint64_t i = 0; i < nnode; i++) free_node(&nodes[i]);
    free(nodes);
}

/* -----------------------------------------------------------------------
 * parse_nodes — recursive parser
 *
 * Parses nodes from *pp into a freshly allocated array, stopping at end-of-
 * string or when a close tag matching close_kind is encountered.
 * On success returns 1, sets *out_nodes / *out_nnode, and advances *pp past
 * the close tag (if any).
 * On failure returns 0.
 * ----------------------------------------------------------------------- */
/* 121.16 PAR-09 (121.41 (5)): bounds on the two recursions.  Before
 * 2026-09-30 neither had one: a template of 100 000 nested `{{#if a}}` tags
 * overflowed the stack in parse_nodes, and a partial that includes itself
 * (`{{>self}}` registered as "self") recursed in render_nodes until the
 * process died.  Exceeding either now fails the compile / render (NULL),
 * which tpl.compile / tpl.render report as $tmplerr. */
#define TMPL_MAX_NEST          64   /* nested if/unless/each blocks per template */
#define TMPL_MAX_RENDER_DEPTH 128   /* blocks + partial expansions while rendering */

static int parse_nodes(const char **pp, TagKind close_kind,
                       Node **out_nodes, uint64_t *out_nnode, int depth)
{
    if (depth > TMPL_MAX_NEST) return 0;
    Node    *arr = NULL;
    uint64_t n   = 0;
    uint64_t cap = 0;

    const char *p = *pp;

    while (*p) {
        /* Check for {{ */
        if (p[0] == '{' && p[1] == '{') {
            TagKind  kind   = TAG_NONE;
            char    *key    = NULL;
            char    *helper = NULL;
            size_t   consumed = 0;

            int ok = try_parse_tag(p + 2, &consumed, &kind, &key, &helper);
            if (ok) {
                /* Check if this is the expected close tag */
                if (close_kind != TAG_NONE && kind == close_kind) {
                    free(key); /* close tags have no key */
                    free(helper);
                    p += 2 + consumed;
                    goto done;
                }

                if (kind == TAG_SLOT) {
                    Node nd;
                    nd.kind        = NODE_SLOT;
                    nd.text        = key;
                    nd.helper_name = helper;
                    nd.children    = NULL;
                    nd.nchild      = 0;
                    if (!append_node(&arr, &n, &cap, nd)) {
                        free(key);
                        free(helper);
                        goto fail;
                    }
                    p += 2 + consumed;
                    continue;
                }

                if (kind == TAG_PARTIAL) {
                    Node nd;
                    nd.kind        = NODE_PARTIAL;
                    nd.text        = key;
                    nd.helper_name = NULL;
                    nd.children    = NULL;
                    nd.nchild      = 0;
                    if (!append_node(&arr, &n, &cap, nd)) {
                        free(key);
                        goto fail;
                    }
                    p += 2 + consumed;
                    continue;
                }

                if (kind == TAG_OPEN_IF || kind == TAG_OPEN_UNLESS ||
                    kind == TAG_OPEN_EACH) {
                    free(helper);
                    p += 2 + consumed;

                    /* parse children until matching close */
                    TagKind expected_close =
                        (kind == TAG_OPEN_IF)     ? TAG_CLOSE_IF    :
                        (kind == TAG_OPEN_UNLESS) ? TAG_CLOSE_UNLESS :
                                                    TAG_CLOSE_EACH;

                    Node    *children = NULL;
                    uint64_t nchild   = 0;
                    if (!parse_nodes(&p, expected_close, &children, &nchild, depth + 1)) {
                        free(key);
                        goto fail;
                    }

                    Node nd;
                    nd.kind        = (kind == TAG_OPEN_IF)     ? NODE_IF    :
                                     (kind == TAG_OPEN_UNLESS) ? NODE_UNLESS :
                                                                 NODE_EACH;
                    nd.text        = key;
                    nd.helper_name = NULL;
                    nd.children    = children;
                    nd.nchild      = nchild;
                    if (!append_node(&arr, &n, &cap, nd)) {
                        free(key);
                        free_nodes(children, nchild);
                        goto fail;
                    }
                    continue;
                }

                /* Unexpected close tag at top level — treat as literal */
                free(key);
                free(helper);
                /* fall through to literal handling */
            }
            /* Not a valid tag — treat {{ as literal start */
        }

        /* Accumulate a literal segment up to the next {{ */
        const char *lit_start = p;
        while (*p) {
            if (p[0] == '{' && p[1] == '{') {
                TagKind  kind2   = TAG_NONE;
                char    *key2    = NULL;
                char    *helper2 = NULL;
                size_t   consumed2 = 0;
                if (try_parse_tag(p + 2, &consumed2, &kind2, &key2, &helper2)) {
                    free(key2);
                    free(helper2);
                    break; /* stop literal before this valid tag */
                }
            }
            p++;
        }
        size_t lit_len = (size_t)(p - lit_start);
        if (lit_len == 0) {
            /* Stuck on an invalid {{ — advance one char to avoid infinite loop */
            p++;
            continue;
        }
        char *lit = (char *)malloc(lit_len + 1);
        if (!lit) goto fail;
        memcpy(lit, lit_start, lit_len);
        lit[lit_len] = '\0';

        Node nd;
        nd.kind        = NODE_LITERAL;
        nd.text        = lit;
        nd.helper_name = NULL;
        nd.children    = NULL;
        nd.nchild      = 0;
        if (!append_node(&arr, &n, &cap, nd)) {
            free(lit);
            goto fail;
        }
    }

    /* If we expected a close tag but hit end-of-string, that's an error
     * only if close_kind was set.  For top-level (close_kind==TAG_NONE) it
     * is normal termination. */
    if (close_kind != TAG_NONE) {
        /* Treat missing close as best-effort: just return what we have */
    }

done:
    *pp = p;
    *out_nodes = arr;
    *out_nnode = n;
    return 1;

fail:
    free_nodes(arr, n);
    return 0;
}

/* -----------------------------------------------------------------------
 * tmpl_compile
 * ----------------------------------------------------------------------- */

TkTmpl *tmpl_compile(const char *source)
{
    if (!source) source = "";

    TkTmpl *t = (TkTmpl *)malloc(sizeof(TkTmpl));
    if (!t) return NULL;

    const char *p = source;
    Node    *nodes = NULL;
    uint64_t nnode = 0;

    if (!parse_nodes(&p, TAG_NONE, &nodes, &nnode, 0)) {
        free(t);
        return NULL;
    }

    t->nodes = nodes;
    t->nnode = nnode;
    return t;
}

/* -----------------------------------------------------------------------
 * tmpl_free
 * ----------------------------------------------------------------------- */

void tmpl_free(TkTmpl *t)
{
    if (!t) return;
    free_nodes(t->nodes, t->nnode);
    free(t);
}

/* -----------------------------------------------------------------------
 * tmpl_escape
 * ----------------------------------------------------------------------- */

const char *tmpl_escape(const char *s)
{
    if (!s) s = "";

    size_t len = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '&':  len += 5; break;
            case '<':  len += 4; break;
            case '>':  len += 4; break;
            case '"':  len += 6; break;
            case '\'': len += 5; break;
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
 * lookup_var
 * ----------------------------------------------------------------------- */

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

/* -----------------------------------------------------------------------
 * JSON array parsing for {{#each}}
 *
 * Parses a JSON array string like ["a","b","c"] into a heap-allocated array
 * of heap-allocated C strings.  Items are simple JSON strings; basic escape
 * sequences \" \\ \/ \n \t \r are unescaped.
 *
 * Returns number of items (0 for empty array or parse failure).
 * *out_items is set to a malloc'd array of malloc'd strings (may be NULL if
 * count is 0).  Caller must free each string and the array.
 * ----------------------------------------------------------------------- */

static uint64_t parse_json_array(const char *src,
                                  char ***out_items)
{
    *out_items = NULL;
    if (!src) return 0;

    const char *p = src;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '[') return 0;
    p++;

    char    **items = NULL;
    uint64_t  n     = 0;
    uint64_t  cap   = 0;

    while (1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']') break;
        if (*p == '\0') break;

        if (*p != '"') {
            /* not a string item — skip to next comma or ] */
            while (*p && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
            continue;
        }
        p++; /* past opening quote */

        /* measure the string length after unescaping */
        size_t len = 0;
        const char *scan = p;
        while (*scan && *scan != '"') {
            if (*scan == '\\') {
                scan++;
                if (*scan) { scan++; len++; }
            } else {
                scan++;
                len++;
            }
        }

        char *item = (char *)malloc(len + 1);
        if (!item) goto fail;

        char *q = item;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                    case '"':  *q++ = '"';  p++; break;
                    case '\\': *q++ = '\\'; p++; break;
                    case '/':  *q++ = '/';  p++; break;
                    case 'n':  *q++ = '\n'; p++; break;
                    case 't':  *q++ = '\t'; p++; break;
                    case 'r':  *q++ = '\r'; p++; break;
                    default:   *q++ = *p;   p++; break;
                }
            } else {
                *q++ = *p++;
            }
        }
        *q = '\0';
        if (*p == '"') p++;

        /* append item */
        if (n == cap) {
            uint64_t new_cap = (cap == 0) ? 8 : cap * 2;
            char **tmp = (char **)realloc(items, new_cap * sizeof(char *));
            if (!tmp) { free(item); goto fail; }
            items = tmp;
            cap = new_cap;
        }
        items[n++] = item;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') p++;
    }

    *out_items = items;
    return n;

fail:
    for (uint64_t i = 0; i < n; i++) free(items[i]);
    free(items);
    return 0;
}

/* -----------------------------------------------------------------------
 * render_nodes — forward declaration
 * ----------------------------------------------------------------------- */

/* Appending output buffer.  122.13: the shared TkStrBuf (template.c's
 * private copy was one of the buffer duplicates 122.2 left); the escaper is
 * strbuf_append_xml, the same five entities the private one wrote.  Both
 * wrappers report failure once an allocation has failed (sticky). */
typedef TkStrBuf Buf;

/* -----------------------------------------------------------------------
 * Contextual auto-escaping — 121.37 (ADR-0011 (b))
 *
 * THE DEFECT.  124.3 made tpl.render escape every value, but with ONE
 * escaper, the HTML-body one, wherever the slot sat.  Measured on main
 * f605aec9 with values u="javascript:alert(1)", x="');alert(1);//":
 *   <a href="{{u}}">          -> href="javascript:alert(1)"      (runs)
 *   onclick="f('{{x}}')"      -> f('&#39;);alert(1);//')  -- the browser
 *                                decodes &#39; before the JS runs (runs)
 *   <script>var s={{x}};      -> var s=&#39;);alert(1);//;  (wrong, and
 *                                `1;alert(1)` would run as-is)
 * and tpl.renderfile rendered with NO escaping at all.
 *
 * THE FIX.  The escaper is chosen per slot from where the slot sits in the
 * HTML being produced, Go html/template style.  The context is read from the
 * OUTPUT, not the template source: an HtmlCtx is fed every byte appended --
 * literal text, escaped values and |raw values alike -- so a slot inside
 * {{#if}}, {{#each}} or a partial gets the context its surroundings produce.
 * The escaped values are built never to change the context they sit in
 * (except that a JS value is a whole string literal, which leaves the JS
 * state where it was), so feeding them keeps the tracker in step; a |raw
 * value is trusted markup and the tracker simply follows it.
 *
 *   context                          escaper
 *   text, comment, <title>/<textarea> HTML entities (& < > " ')
 *   attribute value, quoted           HTML entities
 *   attribute value, unquoted         every non-alphanumeric as &#N;
 *   URL attribute, value start        scheme must be http/https/mailto or
 *                                     absent, else "#ZtokeZ"; then
 *                                     percent-normalised, then as above
 *   URL attribute, after ? or #       percent-encoded as a component
 *   <script>, on* attribute           in a JS string or comment: \uXXXX
 *                                     for all but [A-Za-z0-9 _,.]; in code:
 *                                     a quoted JS string literal
 *   <style>, style attribute          in a CSS string or comment: \HH ;
 *                                     in code: [A-Za-z0-9 #%.,_-] only,
 *                                     else "ZtokeZ"
 *   tag or attribute name             [A-Za-z0-9_-] naming a plain
 *                                     attribute, else "ZtokeZ"
 *
 * Limits, stated: character references inside an attribute value are not
 * decoded by the tracker, and a JS regular-expression literal is not
 * recognised.  Either can only make the tracker believe it is inside a
 * JS/CSS string when it is not, or the reverse; every JS/CSS escaper's
 * output is inert in all of those states (no quote, slash, star, newline
 * or angle bracket survives), so a mis-tracked slot is wrong text, not
 * script.
 * ----------------------------------------------------------------------- */

typedef enum {
    HC_TEXT, HC_TAG_OPEN, HC_END_TAG_OPEN, HC_TAG_NAME, HC_IN_TAG,
    HC_ATTR_NAME, HC_AFTER_ATTR_NAME, HC_BEFORE_VALUE, HC_VALUE,
    HC_MARKUP, HC_COMMENT, HC_RAWTEXT
} HcState;
typedef enum { AK_NORMAL, AK_URL, AK_JS, AK_CSS } HcAttrKind;
typedef enum { EL_NONE, EL_SCRIPT, EL_STYLE, EL_RCDATA } HcElem;
typedef enum { JS_CODE, JS_DQ, JS_SQ, JS_TPL, JS_LINE, JS_BLOCK } HcJs;
typedef enum { CSS_CODE, CSS_DQ, CSS_SQ, CSS_CMT } HcCss;

#define HC_NAME_MAX 32

typedef struct {
    HcState    st;
    int        closing;      /* the tag being read is an end tag */
    HcElem     el;           /* element named by the start tag being read */
    HcElem     raw;          /* RAWTEXT: the element whose content this is */
    char       rname[HC_NAME_MAX];  /* RAWTEXT: its name, for "</name" */
    char       name[HC_NAME_MAX]; int nlen; int nover;  /* tag/attr name */
    HcAttrKind ak;           /* kind of the attribute being read */
    char       quote;        /* VALUE: '"' or '\'', 0 when unquoted */
    size_t     vlen;         /* VALUE: bytes after leading whitespace */
    int        urlpart;      /* VALUE+URL: a '?' or '#' was seen */
    HcJs       js; HcCss css;
    int        esc;          /* JS/CSS string: a backslash is pending */
    int        slash, star;  /* JS/CSS: previous byte was '/' / '*' */
    int        dashes;       /* COMMENT/MARKUP: run of '-' just read */
    int        mcount;       /* MARKUP: bytes read after "<!" */
    int        endlen;       /* RAWTEXT: bytes of "</name" matched */
} HtmlCtx;

typedef struct {
    Buf     b;
    HtmlCtx ctx;
    int     escape;          /* 0: tmpl_render, values verbatim, no tracking */
} Out;

static int hc_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
static int hc_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int hc_alnum(int c) { return hc_alpha(c) || (c >= '0' && c <= '9'); }

static void hc_name_reset(HtmlCtx *h) { h->nlen = 0; h->nover = 0; h->name[0] = '\0'; }
static void hc_name_add(HtmlCtx *h, int c) {
    if (h->nlen < HC_NAME_MAX - 1) {
        h->name[h->nlen++] = (char)tolower((unsigned char)c);
        h->name[h->nlen] = '\0';
    } else {
        h->nover = 1;
    }
}

static HcAttrKind hc_attr_kind(const char *n, int over) {
    static const char *const url_attrs[] = {
        "href", "src", "action", "formaction", "cite", "data", "poster",
        "background", "codebase", "longdesc", "usemap", "manifest", "icon",
        "profile", "classid", "archive", "srcset", "ping", "lowsrc",
        "dynsrc", "xlink:href", "xmlns", NULL
    };
    if (n[0] == 'o' && n[1] == 'n') return AK_JS;         /* any length */
    if (over) return AK_NORMAL;
    if (!strcmp(n, "style")) return AK_CSS;
    for (int i = 0; url_attrs[i]; i++)
        if (!strcmp(n, url_attrs[i])) return AK_URL;
    if (strstr(n, "url") || strstr(n, "uri")) return AK_URL;
    return AK_NORMAL;
}

static HcElem hc_elem(const char *n, int over) {
    if (over) return EL_NONE;
    if (!strcmp(n, "script")) return EL_SCRIPT;
    if (!strcmp(n, "style")) return EL_STYLE;
    if (!strcmp(n, "title") || !strcmp(n, "textarea")) return EL_RCDATA;
    return EL_NONE;
}

static void hc_js_byte(HtmlCtx *h, int c) {
    switch (h->js) {
        case JS_CODE:
            if (h->slash && c == '/') { h->js = JS_LINE; h->slash = 0; return; }
            if (h->slash && c == '*') { h->js = JS_BLOCK; h->slash = 0; h->star = 0; return; }
            h->slash = (c == '/');
            if (c == '"') h->js = JS_DQ;
            else if (c == '\'') h->js = JS_SQ;
            else if (c == '`') h->js = JS_TPL;
            h->esc = 0;
            return;
        case JS_DQ: case JS_SQ: case JS_TPL: {
            if (h->esc) { h->esc = 0; return; }
            if (c == '\\') { h->esc = 1; return; }
            char q = h->js == JS_DQ ? '"' : h->js == JS_SQ ? '\'' : '`';
            if (c == q || (c == '\n' && h->js != JS_TPL)) h->js = JS_CODE;
            h->slash = 0;
            return;
        }
        case JS_LINE:
            if (c == '\n') h->js = JS_CODE;
            return;
        case JS_BLOCK:
            if (h->star && c == '/') { h->js = JS_CODE; h->star = 0; h->slash = 0; return; }
            h->star = (c == '*');
            return;
    }
}

static void hc_css_byte(HtmlCtx *h, int c) {
    switch (h->css) {
        case CSS_CODE:
            if (h->slash && c == '*') { h->css = CSS_CMT; h->slash = 0; h->star = 0; return; }
            h->slash = (c == '/');
            if (c == '"') h->css = CSS_DQ;
            else if (c == '\'') h->css = CSS_SQ;
            h->esc = 0;
            return;
        case CSS_DQ: case CSS_SQ: {
            if (h->esc) { h->esc = 0; return; }
            if (c == '\\') { h->esc = 1; return; }
            char q = h->css == CSS_DQ ? '"' : '\'';
            if (c == q || c == '\n') h->css = CSS_CODE;
            return;
        }
        case CSS_CMT:
            if (h->star && c == '/') { h->css = CSS_CODE; h->star = 0; return; }
            h->star = (c == '*');
            return;
    }
}

static void hc_sub_reset(HtmlCtx *h) {
    h->js = JS_CODE; h->css = CSS_CODE;
    h->esc = h->slash = h->star = 0;
}

/* '>' closes the tag being read. */
static void hc_tag_end(HtmlCtx *h) {
    if (!h->closing && h->el != EL_NONE) {
        h->st = HC_RAWTEXT; h->raw = h->el; h->endlen = 0;
        memcpy(h->rname, h->name, sizeof h->rname);
        hc_sub_reset(h);
    } else {
        h->st = HC_TEXT;
    }
    h->el = EL_NONE; h->closing = 0;
}

static void hc_attr_start(HtmlCtx *h, int c) {
    hc_name_reset(h); hc_name_add(h, c);
    h->st = HC_ATTR_NAME;
}

static void hc_value_start(HtmlCtx *h, char quote) {
    h->st = HC_VALUE; h->quote = quote; h->vlen = 0; h->urlpart = 0;
    hc_sub_reset(h);
}

static void hc_value_byte(HtmlCtx *h, int c) {
    if (h->vlen == 0 && hc_space(c)) return;    /* browsers strip it */
    h->vlen++;
    if (h->ak == AK_URL && (c == '?' || c == '#')) h->urlpart = 1;
    else if (h->ak == AK_JS) hc_js_byte(h, c);
    else if (h->ak == AK_CSS) hc_css_byte(h, c);
}

static void hc_byte(HtmlCtx *h, int c) {
    switch (h->st) {
        case HC_TEXT:
            if (c == '<') h->st = HC_TAG_OPEN;
            return;
        case HC_TAG_OPEN:
            if (hc_alpha(c)) {
                h->closing = 0; hc_name_reset(h); hc_name_add(h, c);
                h->st = HC_TAG_NAME;
            } else if (c == '/') {
                h->st = HC_END_TAG_OPEN;
            } else if (c == '!' || c == '?') {
                h->st = HC_MARKUP; h->mcount = 0; h->dashes = 0;
                if (c == '?') h->mcount = 2;             /* never a comment */
            } else {
                h->st = HC_TEXT;
                if (c == '<') h->st = HC_TAG_OPEN;
            }
            return;
        case HC_END_TAG_OPEN:
            if (hc_alpha(c)) {
                h->closing = 1; hc_name_reset(h); hc_name_add(h, c);
                h->st = HC_TAG_NAME;
            } else if (c == '>') {
                h->st = HC_TEXT;
            } else {
                h->st = HC_MARKUP; h->mcount = 2; h->dashes = 0;
            }
            return;
        case HC_TAG_NAME:
            if (hc_space(c) || c == '/') {
                h->el = h->closing ? EL_NONE : hc_elem(h->name, h->nover);
                h->st = HC_IN_TAG;
            } else if (c == '>') {
                h->el = h->closing ? EL_NONE : hc_elem(h->name, h->nover);
                hc_tag_end(h);
            } else {
                hc_name_add(h, c);
            }
            return;
        case HC_IN_TAG:
            if (hc_space(c) || c == '/') return;
            if (c == '>') { hc_tag_end(h); return; }
            hc_attr_start(h, c);
            return;
        case HC_ATTR_NAME:
            if (hc_space(c)) {
                h->ak = hc_attr_kind(h->name, h->nover);
                h->st = HC_AFTER_ATTR_NAME;
            } else if (c == '=') {
                h->ak = hc_attr_kind(h->name, h->nover);
                h->st = HC_BEFORE_VALUE;
            } else if (c == '/') {
                h->st = HC_IN_TAG;
            } else if (c == '>') {
                hc_tag_end(h);
            } else {
                hc_name_add(h, c);
            }
            return;
        case HC_AFTER_ATTR_NAME:
            if (hc_space(c)) return;
            if (c == '=') { h->st = HC_BEFORE_VALUE; return; }
            if (c == '/') { h->st = HC_IN_TAG; return; }
            if (c == '>') { hc_tag_end(h); return; }
            hc_attr_start(h, c);
            return;
        case HC_BEFORE_VALUE:
            if (hc_space(c)) return;
            if (c == '"' || c == '\'') { hc_value_start(h, (char)c); return; }
            if (c == '>') { hc_tag_end(h); return; }
            hc_value_start(h, 0);
            hc_value_byte(h, c);
            return;
        case HC_VALUE:
            if (h->quote ? c == h->quote : hc_space(c)) { h->st = HC_IN_TAG; return; }
            if (!h->quote && c == '>') { hc_tag_end(h); return; }
            hc_value_byte(h, c);
            return;
        case HC_MARKUP:
            /* "<!--" opens a comment; any other "<!" / "<?" runs to '>' */
            if (h->mcount < 2) {
                h->mcount++;
                if (c == '-') {
                    if (h->mcount == 2) { h->st = HC_COMMENT; h->dashes = 0; }
                    return;
                }
                h->mcount = 2;
            }
            if (c == '>') h->st = HC_TEXT;
            return;
        case HC_COMMENT:
            if (c == '>' && h->dashes >= 2) { h->st = HC_TEXT; return; }
            h->dashes = (c == '-') ? h->dashes + 1 : 0;
            return;
        case HC_RAWTEXT: {
            /* "</script" (any case) followed by whitespace, '/' or '>' ends
             * the element even inside a JS string -- that is how HTML parses
             * it; likewise "</style", "</title", "</textarea". */
            const char *want = h->rname;
            int wlen = (int)strlen(want);
            if (h->endlen == 2 + wlen) {                 /* "</name" read */
                h->endlen = 0;
                if (hc_space(c) || c == '/') {
                    h->st = HC_IN_TAG; h->closing = 1; h->el = EL_NONE;
                    h->raw = EL_NONE;
                    return;
                }
                if (c == '>') { h->st = HC_TEXT; h->raw = EL_NONE; return; }
            }
            if (c == '<') h->endlen = 1;
            else if (h->endlen == 1 && c == '/') h->endlen = 2;
            else if (h->endlen >= 2 && tolower(c) == want[h->endlen - 2]) h->endlen++;
            else h->endlen = 0;
            if (h->raw == EL_SCRIPT) hc_js_byte(h, c);
            else if (h->raw == EL_STYLE) hc_css_byte(h, c);
            return;
        }
    }
}

static void hc_feed(HtmlCtx *h, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) hc_byte(h, (unsigned char)s[i]);
}

/* ── escapers: each appends to b ─────────────────────────────────────── */

static void esc_attr_unquoted(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (hc_alnum(c) || c >= 0x80) strbuf_appendn(b, (const char *)&s[i], 1);
        else strbuf_appendf(b, "&#%u;", (unsigned)c);
    }
}

/* the attribute layer, applied to text already escaped for its sub-context */
static void esc_attr(Buf *b, const HtmlCtx *h, const char *s) {
    if (h->quote) strbuf_append_xml(b, s);
    else esc_attr_unquoted(b, s, strlen(s));   /* unquoted, or starts one */
}

static int js_keep(unsigned char c) {
    return hc_alnum(c) || c == ' ' || c == '_' || c == ',' || c == '.' || c >= 0x80;
}

static void esc_js_string_body(Buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        /* U+2028/U+2029 end a line in pre-2019 JS strings */
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) {
            strbuf_appendf(b, "\\u%04X", p[2] == 0xA8 ? 0x2028u : 0x2029u);
            p += 2;
            continue;
        }
        if (js_keep(*p)) strbuf_appendn(b, (const char *)p, 1);
        else strbuf_appendf(b, "\\u%04X", (unsigned)*p);
    }
}

static void esc_js(Buf *b, HcJs js, const char *s) {
    if (js == JS_CODE) {
        strbuf_putc(b, '"');
        esc_js_string_body(b, s);
        strbuf_putc(b, '"');
    } else {
        esc_js_string_body(b, s);
    }
}

static void esc_css(Buf *b, HcCss css, const char *s) {
    if (css == CSS_CODE) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            if (!(hc_alnum(*p) || *p == ' ' || *p == '#' || *p == '%' ||
                  *p == '.' || *p == ',' || *p == '_' || *p == '-' || *p >= 0x80)) {
                strbuf_append(b, "ZtokeZ");
                return;
            }
        }
        strbuf_append(b, s);
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (hc_alnum(*p) || *p == ' ' || *p >= 0x80) strbuf_appendn(b, (const char *)p, 1);
        else strbuf_appendf(b, "\\%X ", (unsigned)*p);
    }
}

static int url_keep(unsigned char c, int component) {
    if (hc_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~') return 1;
    if (component) return 0;
    return strchr(":/?#[]@!$&'()*+,;=%", c) != NULL && c != '\0';
}

static void esc_url(Buf *b, const HtmlCtx *h, const char *s) {
    if (h->vlen == 0 && !h->urlpart) {
        /* value start: the scheme is decided here */
        const char *p = s;
        while (*p && hc_space((unsigned char)*p)) p++;
        size_t i = 0;
        while (p[i] && p[i] != ':' && p[i] != '/' && p[i] != '?' && p[i] != '#') i++;
        if (p[i] == ':') {
            int ok = (i == 4 && !strncasecmp(p, "http", 4)) ||
                     (i == 5 && !strncasecmp(p, "https", 5)) ||
                     (i == 6 && !strncasecmp(p, "mailto", 6));
            if (!ok) { strbuf_append(b, "#ZtokeZ"); return; }
        }
    }
    int component = h->urlpart;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (url_keep(*p, component)) strbuf_appendn(b, (const char *)p, 1);
        else strbuf_appendf(b, "%%%02X", (unsigned)*p);
    }
}

/* tag or attribute name position */
static void esc_name(Buf *b, const char *s) {
    char low[HC_NAME_MAX] = {0};
    size_t n = 0;
    int ok = s[0] != '\0';
    for (const char *p = s; *p && ok; p++, n++) {
        if (n >= sizeof low - 1 ||
            !(hc_alnum((unsigned char)*p) || *p == '-' || *p == '_'))
            ok = 0;
        else
            low[n] = (char)tolower((unsigned char)*p);
    }
    if (ok && hc_attr_kind(low, 0) != AK_NORMAL) ok = 0;
    strbuf_append(b, ok ? s : "ZtokeZ");
}

/* Escape one slot value for the context the output is in. */
static void ctx_escape(Buf *b, const HtmlCtx *h, const char *v) {
    switch (h->st) {
        case HC_TEXT: case HC_COMMENT: case HC_MARKUP:
            strbuf_append_xml(b, v);
            return;
        case HC_RAWTEXT:
            if (h->raw == EL_SCRIPT) esc_js(b, h->js, v);
            else if (h->raw == EL_STYLE) esc_css(b, h->css, v);
            else strbuf_append_xml(b, v);
            return;
        case HC_TAG_OPEN: case HC_END_TAG_OPEN: case HC_TAG_NAME:
        case HC_IN_TAG: case HC_ATTR_NAME: case HC_AFTER_ATTR_NAME:
            esc_name(b, v);
            return;
        case HC_BEFORE_VALUE: case HC_VALUE: {
            /* BEFORE_VALUE: the slot starts an unquoted value */
            HtmlCtx vh = *h;
            if (vh.st == HC_BEFORE_VALUE) {
                vh.st = HC_VALUE; vh.quote = 0; vh.vlen = 0; vh.urlpart = 0;
                vh.js = JS_CODE; vh.css = CSS_CODE;
            }
            TkStrBuf inner = TK_STRBUF_INIT;
            switch (vh.ak) {
                case AK_URL: esc_url(&inner, &vh, v); break;
                case AK_JS:  esc_js(&inner, vh.js, v); break;
                case AK_CSS: esc_css(&inner, vh.css, v); break;
                default:     strbuf_append(&inner, v); break;
            }
            if (inner.oom) { b->oom = 1; strbuf_free(&inner); return; }
            esc_attr(b, &vh, inner.buf ? inner.buf : "");
            strbuf_free(&inner);
            return;
        }
    }
}

static int out_literal(Out *o, const char *s, size_t n)
{
    strbuf_appendn(&o->b, s, n);
    if (o->escape) hc_feed(&o->ctx, s, n);
    return !o->b.oom;
}

/* A slot value: escaped for its context unless raw, then fed to the tracker
 * (the escaped text is what the browser will parse). */
static int out_value(Out *o, const char *v, int raw)
{
    if (!v) v = "";
    size_t before = o->b.len;
    if (!o->escape || raw) strbuf_append(&o->b, v);
    else ctx_escape(&o->b, &o->ctx, v);
    if (o->b.oom) return 0;
    if (o->escape) hc_feed(&o->ctx, o->b.buf + before, o->b.len - before);
    return 1;
}

/* render_nodes — render node array into Out, using vars for variable lookup.
 * extra_vars / extra_nvar are prepended (used for EACH item vars). */
static int render_nodes(const Node *nodes, uint64_t nnode,
                        const TkTmplVar *extra_vars, uint64_t extra_nvar,
                        const TkTmplVar *vars, uint64_t nvar,
                        Out *out, int depth);

static const char *lookup_combined(const TkTmplVar *extra, uint64_t nextra,
                                   const TkTmplVar *vars, uint64_t nvar,
                                   const char *key)
{
    /* check extra vars first (EACH item context) */
    for (uint64_t i = 0; i < nextra; i++) {
        if (extra[i].key && strcmp(extra[i].key, key) == 0)
            return extra[i].value ? extra[i].value : "";
    }
    return lookup_var(vars, nvar, key);
}

static int render_nodes(const Node *nodes, uint64_t nnode,
                        const TkTmplVar *extra_vars, uint64_t extra_nvar,
                        const TkTmplVar *vars, uint64_t nvar,
                        Out *out, int depth)
{
    if (depth > TMPL_MAX_RENDER_DEPTH) return 0;
    for (uint64_t i = 0; i < nnode; i++) {
        const Node *nd = &nodes[i];
        switch (nd->kind) {
            case NODE_LITERAL:
                if (!out_literal(out, nd->text, strlen(nd->text))) return 0;
                break;

            case NODE_SLOT: {
                const char *val = lookup_combined(extra_vars, extra_nvar,
                                                  vars, nvar, nd->text);

                /* Apply helper transform if one is registered */
                const char *transformed = val;
                int         helper_owned = 0;
                if (nd->helper_name) {
                    for (int hi = 0; hi < g_nhelpers; hi++) {
                        if (strcmp(g_helpers[hi].name, nd->helper_name) == 0) {
                            const char *r = g_helpers[hi].fn(val);
                            if (r && r != val) {
                                transformed  = r;
                                helper_owned = 1;
                            }
                            break;
                        }
                    }
                }

                /* 124.3 (ADR-0011): escape-by-default, with an explicit `|raw`
                 * opt-out for trusted HTML (e.g. composing sub-templates). A
                 * slot written `{{key|raw}}` is emitted unescaped; every other
                 * slot is escaped for its context (121.37) when escaping is
                 * on. */
                int is_raw = (nd->helper_name &&
                              strcmp(nd->helper_name, "raw") == 0);
                int slot_ok = out_value(out, transformed, is_raw);
                if (helper_owned) free((void *)transformed);
                if (!slot_ok) return 0;
                break;
            }

            case NODE_PARTIAL: {
                /* Look up partial in the global registry */
                TkTmpl *partial = NULL;
                for (int pi = 0; pi < g_npartials; pi++) {
                    if (strcmp(g_partials[pi].name, nd->text) == 0) {
                        partial = g_partials[pi].compiled;
                        break;
                    }
                }
                if (partial) {
                    /* Render the partial's nodes with the same variable bindings */
                    if (!render_nodes(partial->nodes, partial->nnode,
                                      extra_vars, extra_nvar,
                                      vars, nvar, out, depth + 1))
                        return 0;
                }
                /* If not found, render as empty string (per spec) */
                break;
            }

            case NODE_IF: {
                const char *val = lookup_var(vars, nvar, nd->text);
                if (val && val[0] != '\0') {
                    if (!render_nodes(nd->children, nd->nchild,
                                      extra_vars, extra_nvar,
                                      vars, nvar, out, depth + 1))
                        return 0;
                }
                break;
            }

            case NODE_UNLESS: {
                const char *val = lookup_var(vars, nvar, nd->text);
                if (!val || val[0] == '\0') {
                    if (!render_nodes(nd->children, nd->nchild,
                                      extra_vars, extra_nvar,
                                      vars, nvar, out, depth + 1))
                        return 0;
                }
                break;
            }

            case NODE_EACH: {
                const char *json = lookup_var(vars, nvar, nd->text);
                char    **items  = NULL;
                uint64_t  nitems = parse_json_array(json, &items);
                for (uint64_t j = 0; j < nitems; j++) {
                    /* Build index string */
                    char idx_buf[32];
                    snprintf(idx_buf, sizeof(idx_buf), "%" PRIu64, j);

                    TkTmplVar item_vars[2];
                    item_vars[0].key   = ".";
                    item_vars[0].value = items[j];
                    item_vars[1].key   = "@index";
                    item_vars[1].value = idx_buf;

                    if (!render_nodes(nd->children, nd->nchild,
                                      item_vars, 2,
                                      vars, nvar, out, depth + 1)) {
                        for (uint64_t k = 0; k < nitems; k++) free(items[k]);
                        free(items);
                        return 0;
                    }
                }
                for (uint64_t k = 0; k < nitems; k++) free(items[k]);
                free(items);
                break;
            }
        }
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * render_internal
 * ----------------------------------------------------------------------- */

static const char *render_internal(TkTmpl *t, const TkTmplVar *vars,
                                    uint64_t nvar, int escape_values)
{
    if (!t) return NULL;

    Out o;
    memset(&o, 0, sizeof o);
    o.escape = escape_values;       /* ctx zeroed: HC_TEXT */

    if (!render_nodes(t->nodes, t->nnode, NULL, 0, vars, nvar, &o, 0)) {
        strbuf_free(&o.b);
        return NULL;
    }

    return strbuf_take(&o.b);   /* NUL-terminated; NULL after any OOM */
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

    size_t total = 1 + tag_len;

    for (uint64_t i = 0; i < nattr; i++) {
        if (!attr_keys || !attr_values) break;
        const char *ak = attr_keys[i]   ? attr_keys[i]   : "";
        const char *av = attr_values[i]  ? attr_values[i] : "";
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
        total += 1;
    }

    total += 1;

    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i])
            total += strlen(children[i]);
    }

    total += 2 + tag_len + 1;

    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;

    char *q = out;

    *q++ = '<';
    memcpy(q, tag, tag_len);
    q += tag_len;

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

    for (uint64_t i = 0; i < nchild; i++) {
        if (children && children[i]) {
            size_t clen = strlen(children[i]);
            memcpy(q, children[i], clen);
            q += clen;
        }
    }

    *q++ = '<';
    *q++ = '/';
    memcpy(q, tag, tag_len);
    q += tag_len;
    *q++ = '>';
    *q = '\0';

    return out;
}

/* -----------------------------------------------------------------------
 * tmpl_register_partial — Story 33.1.2
 * ----------------------------------------------------------------------- */

void tmpl_register_partial(const char *name, const char *source)
{
    if (!name || !source) return;

    /* Replace existing entry with the same name */
    for (int i = 0; i < g_npartials; i++) {
        if (strcmp(g_partials[i].name, name) == 0) {
            tmpl_free(g_partials[i].compiled);
            g_partials[i].compiled = tmpl_compile(source);
            return;
        }
    }

    if (g_npartials >= 64) return; /* registry full */

    g_partials[g_npartials].name     = name;
    g_partials[g_npartials].compiled = tmpl_compile(source);
    g_npartials++;
}

/* -----------------------------------------------------------------------
 * tmpl_register_helper — Story 33.1.2
 * ----------------------------------------------------------------------- */

void tmpl_register_helper(const char *name, TkTmplHelperFn fn)
{
    if (!name || !fn) return;

    /* Replace existing entry with the same name */
    for (int i = 0; i < g_nhelpers; i++) {
        if (strcmp(g_helpers[i].name, name) == 0) {
            g_helpers[i].fn = fn;
            return;
        }
    }

    if (g_nhelpers >= 64) return; /* registry full */

    g_helpers[g_nhelpers].name = name;
    g_helpers[g_nhelpers].fn   = fn;
    g_nhelpers++;
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

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long fsize = ftell(fp);
    if (fsize < 0) { fclose(fp); return NULL; }
    /* 135.3c: fseek, not rewind() -- rewind reports failure only through errno, which nothing read (clang unix.Errno). */
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }

    char *src = (char *)malloc((size_t)fsize + 1);
    if (!src) { fclose(fp); return NULL; }

    size_t nread = fread(src, 1, (size_t)fsize, fp);
    fclose(fp);
    src[nread] = '\0';

    TkTmpl *t = tmpl_compile(src);
    free(src);
    if (!t) return NULL;

    /* 121.37: escaped for context like tpl.render.  Until 2026-09-30 this
     * was tmpl_render -- no escaping at all -- so tpl.renderfile emitted
     * every value verbatim. */
    const char *result = tmpl_renderhtml(t, vars, nvar);
    tmpl_free(t);
    return result;
}

/* -----------------------------------------------------------------------
 * tmpl_renderpage — Story 95.2
 *
 * Render a .tkt page file with layout/block/yield directive support.
 *
 * Directive syntax:
 *   {! layout("name") !}     — use templates_dir/name.tkt as layout
 *   {! block("name") !}...{! end !} — define named content block
 *   {! yield("name") !}      — in layout, replaced by named block content
 *
 * After directive processing, the result is rendered through the {{var}}
 * template engine with the provided variable bindings.
 * ----------------------------------------------------------------------- */

/* Read a file into a heap-allocated string. Caller owns result. */
static char *tmpl_readfile(const char *path) {
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long fsize = ftell(fp);
    if (fsize < 0) { fclose(fp); return NULL; }
    /* 135.3c: fseek, not rewind() -- rewind reports failure only through errno, which nothing read (clang unix.Errno). */
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    buf[nread] = '\0';
    return buf;
}

/* A named block extracted from a page template. */
typedef struct {
    char *name;
    char *content;
} TmplBlock;

#define TMPL_MAX_BLOCKS 32

/*
 * parse_page_directives — scan page source for {! !} directives.
 *
 * Extracts:
 *   layout_name  — from {! layout("name") !}  (heap-allocated, caller frees)
 *   blocks       — from {! block("name") !}...{! end !}
 *   nblocks      — number of blocks found
 *
 * Returns 1 on success, 0 on failure.
 */
static int parse_page_directives(const char *src, char **layout_name,
                                  TmplBlock *blocks, int *nblocks)
{
    *layout_name = NULL;
    *nblocks = 0;
    const char *p = src;

    while (*p) {
        /* Look for {! */
        if (p[0] == '{' && p[1] == '!') {
            p += 2;

            /* Skip whitespace */
            while (*p == ' ' || *p == '\t') p++;

            /* Check for layout("name") */
            if (strncmp(p, "layout(\"", 8) == 0) {
                p += 8;
                const char *name_start = p;
                while (*p && *p != '"') p++;
                if (*p != '"') return 0;
                size_t nlen = (size_t)(p - name_start);
                /* 135.3c: a second layout("...") overwrote -- and leaked --
                 * the first; the last one wins, as before. */
                free(*layout_name);
                *layout_name = (char *)malloc(nlen + 1);
                if (!*layout_name) return 0;
                memcpy(*layout_name, name_start, nlen);
                (*layout_name)[nlen] = '\0';
                p++; /* skip closing quote */
                while (*p == ')' || *p == ' ' || *p == '\t') p++;
                if (p[0] == '!' && p[1] == '}') p += 2;
                continue;
            }

            /* Check for block("name") */
            if (strncmp(p, "block(\"", 7) == 0) {
                p += 7;
                const char *name_start = p;
                while (*p && *p != '"') p++;
                if (*p != '"') return 0;
                size_t nlen = (size_t)(p - name_start);
                char *bname = (char *)malloc(nlen + 1);
                if (!bname) return 0;
                memcpy(bname, name_start, nlen);
                bname[nlen] = '\0';
                p++; /* skip closing quote */
                while (*p == ')' || *p == ' ' || *p == '\t') p++;
                if (p[0] == '!' && p[1] == '}') p += 2;

                /* Now collect content until {! end !} */
                const char *content_start = p;
                const char *content_end = NULL;

                while (*p) {
                    if (p[0] == '{' && p[1] == '!') {
                        const char *tp = p + 2;
                        while (*tp == ' ' || *tp == '\t') tp++;
                        if (strncmp(tp, "end", 3) == 0) {
                            content_end = p;
                            tp += 3;
                            while (*tp == ' ' || *tp == '\t') tp++;
                            if (tp[0] == '!' && tp[1] == '}') {
                                p = tp + 2;
                                break;
                            }
                        }
                    }
                    p++;
                }

                if (!content_end) {
                    /* No matching {! end !} found, use rest of string */
                    content_end = p;
                }

                if (*nblocks < TMPL_MAX_BLOCKS) {
                    size_t clen = (size_t)(content_end - content_start);
                    char *content = (char *)malloc(clen + 1);
                    if (!content) { free(bname); return 0; }
                    memcpy(content, content_start, clen);
                    content[clen] = '\0';
                    blocks[*nblocks].name = bname;
                    blocks[*nblocks].content = content;
                    (*nblocks)++;
                } else {
                    free(bname);
                }
                continue;
            }

            /* Unknown directive — skip to !} */
            while (*p) {
                if (p[0] == '!' && p[1] == '}') { p += 2; break; }
                p++;
            }
            continue;
        }
        p++;
    }
    return 1;
}

/*
 * apply_yields — replace {! yield("name") !} in layout with block content.
 * Returns a heap-allocated string. Caller owns it.
 */
static char *apply_yields(const char *layout_src, const TmplBlock *blocks,
                           int nblocks)
{
    /* 122.13: a TkStrBuf, not a hand-grown char* (the last private buffer
     * in this file). */
    TkStrBuf out = TK_STRBUF_INIT;
    const char *p = layout_src;

    while (*p) {
        if (p[0] == '{' && p[1] == '!') {
            const char *tp = p + 2;
            while (*tp == ' ' || *tp == '\t') tp++;

            if (strncmp(tp, "yield(\"", 7) == 0) {
                tp += 7;
                const char *name_start = tp;
                while (*tp && *tp != '"') tp++;
                if (*tp == '"') {
                    size_t nlen = (size_t)(tp - name_start);
                    tp++; /* skip quote */
                    while (*tp == ')' || *tp == ' ' || *tp == '\t') tp++;
                    if (tp[0] == '!' && tp[1] == '}') {
                        tp += 2;
                        /* Find matching block */
                        const char *replacement = NULL;
                        for (int i = 0; i < nblocks; i++) {
                            if (strlen(blocks[i].name) == nlen &&
                                memcmp(blocks[i].name, name_start, nlen) == 0) {
                                replacement = blocks[i].content;
                                break;
                            }
                        }
                        strbuf_append(&out, replacement);
                        p = tp;
                        continue;
                    }
                }
            }
        }

        strbuf_putc(&out, *p++);
    }
    return strbuf_take(&out);
}

const char *tmpl_renderpage(const char *page_path, const char *templates_dir,
                            const TkTmplVar *vars, uint64_t nvar)
{
    if (!page_path || !templates_dir) return NULL;

    /* Read the page file */
    char *page_src = tmpl_readfile(page_path);
    if (!page_src) return NULL;

    /* Parse directives */
    char *layout_name = NULL;
    TmplBlock blocks[TMPL_MAX_BLOCKS];
    int nblocks = 0;

    if (!parse_page_directives(page_src, &layout_name, blocks, &nblocks)) {
        /* A malformed directive after a layout() or a block() returned
         * with those already allocated; they leaked here. */
        free(layout_name);
        for (int i = 0; i < nblocks; i++) {
            free(blocks[i].name);
            free(blocks[i].content);
        }
        free(page_src);
        return NULL;
    }

    char *final_src = NULL;

    if (layout_name) {
        /* 121.16 PAR-09 (121.41 (5)): the layout name is a file NAME inside
         * templates_dir -- not a path.  Until 2026-09-30 layout("../x")
         * read templates_dir/../x.tkt.  Refuse a separator, a `..`, and an
         * empty name, and fail the render. */
        if (layout_name[0] == '\0' || strchr(layout_name, '/') ||
            strchr(layout_name, '\\') || strstr(layout_name, "..")) {
            goto cleanup;
        }
        /* Build layout path: templates_dir/layout_name.tkt */
        size_t dir_len = strlen(templates_dir);
        size_t name_len = strlen(layout_name);
        /* +1 for '/', +4 for ".tkt", +1 for '\0' */
        char *layout_path = (char *)malloc(dir_len + 1 + name_len + 4 + 1);
        if (!layout_path) goto cleanup;

        memcpy(layout_path, templates_dir, dir_len);
        size_t pos = dir_len;
        if (pos > 0 && layout_path[pos - 1] != '/') layout_path[pos++] = '/';
        memcpy(layout_path + pos, layout_name, name_len);
        pos += name_len;
        memcpy(layout_path + pos, ".tkt", 4);
        pos += 4;
        layout_path[pos] = '\0';

        char *layout_src = tmpl_readfile(layout_path);
        free(layout_path);
        if (!layout_src) goto cleanup;

        /* The layout may itself have a layout (recursive) — for now, support
         * one level of nesting by processing {! yield !} in the layout */
        final_src = apply_yields(layout_src, blocks, nblocks);
        free(layout_src);
    } else {
        /* No layout — just use the page source directly (after stripping
         * any {! !} directives, treat remaining content as the template) */
        final_src = strdup(page_src);
    }

    if (!final_src) goto cleanup;

    /* Now render {{var}} slots in the assembled template */
    TkTmpl *t = tmpl_compile(final_src);
    free(final_src);
    final_src = NULL;
    if (!t) goto cleanup;

    const char *result = tmpl_renderhtml(t, vars, nvar);   /* 121.37 */
    tmpl_free(t);

    /* Cleanup blocks and page source */
    free(page_src);
    free(layout_name);
    for (int i = 0; i < nblocks; i++) {
        free(blocks[i].name);
        free(blocks[i].content);
    }
    return result;

cleanup:
    free(page_src);
    free(layout_name);
    free(final_src);
    for (int i = 0; i < nblocks; i++) {
        free(blocks[i].name);
        free(blocks[i].content);
    }
    return NULL;
}
