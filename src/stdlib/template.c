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
 * Story: 15.1.4, 33.1.1
 */

#include "template.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal node representation
 * ----------------------------------------------------------------------- */

typedef enum {
    NODE_LITERAL,  /* fixed text */
    NODE_SLOT,     /* {{IDENT}} variable slot */
    NODE_IF,       /* {{#if key}}...{{/if}} */
    NODE_UNLESS,   /* {{#unless key}}...{{/unless}} */
    NODE_EACH      /* {{#each key}}...{{/each}} */
} NodeKind;

/* Forward declaration for recursive struct */
typedef struct Node Node;

struct Node {
    NodeKind  kind;
    char     *text;      /* LITERAL: literal text; SLOT/IF/UNLESS/EACH: key */
    Node     *children;  /* block nodes: child node array */
    uint64_t  nchild;    /* number of children */
};

struct TkTmpl {
    Node     *nodes;
    uint64_t  nnode;
};

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
    TAG_SLOT,       /* {{IDENT}} */
    TAG_OPEN_IF,    /* {{#if key}} */
    TAG_OPEN_UNLESS,/* {{#unless key}} */
    TAG_OPEN_EACH,  /* {{#each key}} */
    TAG_CLOSE_IF,   /* {{/if}} */
    TAG_CLOSE_UNLESS,/* {{/unless}} */
    TAG_CLOSE_EACH  /* {{/each}} */
} TagKind;

/*
 * try_parse_tag — attempt to parse a template tag starting at *pp which
 * already points past the opening "{{".
 *
 * On success sets *out_kind and *out_key (heap-allocated, caller frees; may
 * be NULL for close tags) and advances *pp past the closing "}}".
 * Returns 1 on success, 0 on failure (pp not modified on failure).
 */
static int try_parse_tag(const char *src, size_t *out_consumed,
                         TagKind *out_kind, char **out_key)
{
    const char *p = src;
    *out_key = NULL;
    *out_kind = TAG_NONE;

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

    /* Plain IDENT slot */
    if (!ident_start(p[0])) return 0;
    const char *key_start = p;
    size_t n = 0;
    while (p[n] && ident_cont(p[n])) n++;
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
static int parse_nodes(const char **pp, TagKind close_kind,
                       Node **out_nodes, uint64_t *out_nnode)
{
    Node    *arr = NULL;
    uint64_t n   = 0;
    uint64_t cap = 0;

    const char *p = *pp;

    while (*p) {
        /* Check for {{ */
        if (p[0] == '{' && p[1] == '{') {
            TagKind  kind = TAG_NONE;
            char    *key  = NULL;
            size_t   consumed = 0;

            int ok = try_parse_tag(p + 2, &consumed, &kind, &key);
            if (ok) {
                /* Check if this is the expected close tag */
                if (close_kind != TAG_NONE && kind == close_kind) {
                    free(key); /* close tags have no key */
                    p += 2 + consumed;
                    goto done;
                }

                if (kind == TAG_SLOT) {
                    Node nd;
                    nd.kind     = NODE_SLOT;
                    nd.text     = key;
                    nd.children = NULL;
                    nd.nchild   = 0;
                    if (!append_node(&arr, &n, &cap, nd)) {
                        free(key);
                        goto fail;
                    }
                    p += 2 + consumed;
                    continue;
                }

                if (kind == TAG_OPEN_IF || kind == TAG_OPEN_UNLESS ||
                    kind == TAG_OPEN_EACH) {
                    p += 2 + consumed;

                    /* parse children until matching close */
                    TagKind expected_close =
                        (kind == TAG_OPEN_IF)     ? TAG_CLOSE_IF    :
                        (kind == TAG_OPEN_UNLESS) ? TAG_CLOSE_UNLESS :
                                                    TAG_CLOSE_EACH;

                    Node    *children = NULL;
                    uint64_t nchild   = 0;
                    if (!parse_nodes(&p, expected_close, &children, &nchild)) {
                        free(key);
                        goto fail;
                    }

                    Node nd;
                    nd.kind     = (kind == TAG_OPEN_IF)     ? NODE_IF    :
                                  (kind == TAG_OPEN_UNLESS) ? NODE_UNLESS :
                                                              NODE_EACH;
                    nd.text     = key;
                    nd.children = children;
                    nd.nchild   = nchild;
                    if (!append_node(&arr, &n, &cap, nd)) {
                        free(key);
                        free_nodes(children, nchild);
                        goto fail;
                    }
                    continue;
                }

                /* Unexpected close tag at top level — treat as literal */
                free(key);
                /* fall through to literal handling */
            }
            /* Not a valid tag — treat {{ as literal start */
        }

        /* Accumulate a literal segment up to the next {{ */
        const char *lit_start = p;
        while (*p) {
            if (p[0] == '{' && p[1] == '{') {
                TagKind  kind2 = TAG_NONE;
                char    *key2  = NULL;
                size_t   consumed2 = 0;
                if (try_parse_tag(p + 2, &consumed2, &kind2, &key2)) {
                    free(key2);
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
        nd.kind     = NODE_LITERAL;
        nd.text     = lit;
        nd.children = NULL;
        nd.nchild   = 0;
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

    if (!parse_nodes(&p, TAG_NONE, &nodes, &nnode)) {
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

/* Appending output buffer */
typedef struct {
    char    *buf;
    size_t   len;
    size_t   cap;
} Buf;

static int buf_append(Buf *b, const char *s, size_t slen)
{
    if (slen == 0) return 1;
    if (b->len + slen >= b->cap) {
        size_t new_cap = b->cap ? b->cap : 64;
        while (new_cap <= b->len + slen) new_cap *= 2;
        char *tmp = (char *)realloc(b->buf, new_cap);
        if (!tmp) return 0;
        b->buf = tmp;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->len, s, slen);
    b->len += slen;
    return 1;
}

static int buf_append_escaped(Buf *b, const char *s)
{
    for (const char *p = s; *p; p++) {
        int ok;
        switch (*p) {
            case '&':  ok = buf_append(b, "&amp;",  5); break;
            case '<':  ok = buf_append(b, "&lt;",   4); break;
            case '>':  ok = buf_append(b, "&gt;",   4); break;
            case '"':  ok = buf_append(b, "&quot;", 6); break;
            case '\'': ok = buf_append(b, "&#39;",  5); break;
            default: {
                char c = *p;
                ok = buf_append(b, &c, 1);
                break;
            }
        }
        if (!ok) return 0;
    }
    return 1;
}

/* render_nodes — render node array into Buf, using vars for variable lookup.
 * extra_vars / extra_nvar are prepended (used for EACH item vars). */
static int render_nodes(const Node *nodes, uint64_t nnode,
                        const TkTmplVar *extra_vars, uint64_t extra_nvar,
                        const TkTmplVar *vars, uint64_t nvar,
                        int escape_values, Buf *out);

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
                        int escape_values, Buf *out)
{
    for (uint64_t i = 0; i < nnode; i++) {
        const Node *nd = &nodes[i];
        switch (nd->kind) {
            case NODE_LITERAL:
                if (!buf_append(out, nd->text, strlen(nd->text))) return 0;
                break;

            case NODE_SLOT: {
                const char *val = lookup_combined(extra_vars, extra_nvar,
                                                  vars, nvar, nd->text);
                if (escape_values) {
                    if (!buf_append_escaped(out, val)) return 0;
                } else {
                    if (!buf_append(out, val, strlen(val))) return 0;
                }
                break;
            }

            case NODE_IF: {
                const char *val = lookup_var(vars, nvar, nd->text);
                if (val && val[0] != '\0') {
                    if (!render_nodes(nd->children, nd->nchild,
                                      extra_vars, extra_nvar,
                                      vars, nvar, escape_values, out))
                        return 0;
                }
                break;
            }

            case NODE_UNLESS: {
                const char *val = lookup_var(vars, nvar, nd->text);
                if (!val || val[0] == '\0') {
                    if (!render_nodes(nd->children, nd->nchild,
                                      extra_vars, extra_nvar,
                                      vars, nvar, escape_values, out))
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
                    int idx_len = snprintf(idx_buf, sizeof(idx_buf),
                                           "%" PRIu64, j);
                    if (idx_len < 0) idx_len = 0;

                    TkTmplVar item_vars[2];
                    item_vars[0].key   = ".";
                    item_vars[0].value = items[j];
                    item_vars[1].key   = "@index";
                    item_vars[1].value = idx_buf;

                    if (!render_nodes(nd->children, nd->nchild,
                                      item_vars, 2,
                                      vars, nvar, escape_values, out)) {
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

    Buf b;
    b.buf = NULL;
    b.len = 0;
    b.cap = 0;

    if (!render_nodes(t->nodes, t->nnode, NULL, 0, vars, nvar,
                      escape_values, &b)) {
        free(b.buf);
        return NULL;
    }

    /* NUL-terminate */
    if (!buf_append(&b, "\0", 1)) {
        free(b.buf);
        return NULL;
    }

    return b.buf;
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
