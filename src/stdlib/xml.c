/*
 * xml.c — std.xml standard library implementation.
 *
 * Pure libc XML builder and parser. No external dependencies.
 * All returned strings are heap-allocated.
 *
 * Story: 88.6.1 (builder) / 88.6.2 (parser)
 */

#include "xml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Allocate and build a string from format. Caller owns result. */
static char *xprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Builder API                                                         */
/* ------------------------------------------------------------------ */

const char *xml_element(const char *tag, const char *content, const char *attrs) {
    if (!tag) tag = "";
    if (!content) content = "";
    if (!attrs) attrs = "";
    size_t tlen = strlen(tag);
    size_t clen = strlen(content);
    size_t alen = strlen(attrs);
    /* <tag attrs>content</tag> — with space before attrs if non-empty */
    size_t need = 1 + tlen + (alen ? 1 + alen : 0) + 1 + clen + 2 + tlen + 1 + 1;
    char *buf = (char *)malloc(need);
    if (!buf) return NULL;
    if (alen) {
        snprintf(buf, need, "<%s %s>%s</%s>", tag, attrs, content, tag);
    } else {
        snprintf(buf, need, "<%s>%s</%s>", tag, content, tag);
    }
    return buf;
}

const char *xml_element_self_closing(const char *tag, const char *attrs) {
    if (!tag) tag = "";
    if (!attrs) attrs = "";
    size_t tlen = strlen(tag);
    size_t alen = strlen(attrs);
    size_t need = 1 + tlen + (alen ? 1 + alen : 0) + 3 + 1;
    char *buf = (char *)malloc(need);
    if (!buf) return NULL;
    if (alen) {
        snprintf(buf, need, "<%s %s />", tag, attrs);
    } else {
        snprintf(buf, need, "<%s />", tag);
    }
    return buf;
}

const char *xml_escape(const char *s) {
    if (!s) return strdup("");
    /* Count how much space we need */
    size_t need = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '<':  need += 4; break; /* &lt; */
            case '>':  need += 4; break; /* &gt; */
            case '&':  need += 5; break; /* &amp; */
            case '"':  need += 6; break; /* &quot; */
            case '\'': need += 6; break; /* &apos; */
            default:   need += 1; break;
        }
    }
    char *buf = (char *)malloc(need + 1);
    if (!buf) return NULL;
    char *d = buf;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '<':  memcpy(d, "&lt;", 4);   d += 4; break;
            case '>':  memcpy(d, "&gt;", 4);   d += 4; break;
            case '&':  memcpy(d, "&amp;", 5);  d += 5; break;
            case '"':  memcpy(d, "&quot;", 6); d += 6; break;
            case '\'': memcpy(d, "&apos;", 6); d += 6; break;
            default:   *d++ = *p; break;
        }
    }
    *d = '\0';
    return buf;
}

const char *xml_cdata(const char *s) {
    if (!s) s = "";
    return xprintf("<![CDATA[%s]]>", s);
}

const char *xml_declaration(const char *version, const char *encoding) {
    if (!version) version = "1.0";
    if (encoding && *encoding) {
        return xprintf("<?xml version=\"%s\" encoding=\"%s\"?>", version, encoding);
    }
    return xprintf("<?xml version=\"%s\"?>", version);
}

const char *xml_comment(const char *text) {
    if (!text) text = "";
    return xprintf("<!-- %s -->", text);
}

const char *xml_element_ns(const char *ns, const char *tag, const char *content, const char *attrs) {
    if (!ns || !*ns) return xml_element(tag, content, attrs);
    if (!tag) tag = "";
    /* Build "ns:tag" and delegate */
    char *nstag = xprintf("%s:%s", ns, tag);
    if (!nstag) return NULL;
    const char *result = xml_element(nstag, content, attrs);
    free(nstag);
    return result;
}

/* ------------------------------------------------------------------ */
/* Parser internals                                                    */
/* ------------------------------------------------------------------ */

/* Growable node array */
typedef struct {
    XmlNode *items;
    int      len;
    int      cap;
} NodeVec;

static void nv_init(NodeVec *v) {
    v->items = NULL; v->len = 0; v->cap = 0;
}

static void nv_push(NodeVec *v, XmlNode n) {
    if (v->len >= v->cap) {
        int newcap = v->cap ? v->cap * 2 : 16;
        XmlNode *tmp = (XmlNode *)realloc(v->items, (size_t)newcap * sizeof(XmlNode));
        if (!tmp) return;
        v->items = tmp;
        v->cap = newcap;
    }
    v->items[v->len++] = n;
}

/* Growable char buffer */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CharBuf;

static void cb_init(CharBuf *b) {
    b->data = NULL; b->len = 0; b->cap = 0;
}

static void cb_append(CharBuf *b, const char *s, size_t n) {
    if (n == 0) return;
    if (b->len + n >= b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 64;
        while (newcap < b->len + n + 1) newcap *= 2;
        char *tmp = (char *)realloc(b->data, newcap);
        if (!tmp) return;
        b->data = tmp;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void cb_appendc(CharBuf *b, char c) {
    cb_append(b, &c, 1);
}

static char *cb_detach(CharBuf *b) {
    if (!b->data) return strdup("");
    char *s = b->data;
    b->data = NULL; b->len = 0; b->cap = 0;
    return s;
}

/* cb_free unused but retained for API completeness */

/* Unescape XML entities in-place style (returns new heap string) */
static char *xml_unescape(const char *s, size_t n) {
    CharBuf out;
    cb_init(&out);
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&') {
            if (i + 3 < n && s[i+1] == 'l' && s[i+2] == 't' && s[i+3] == ';') {
                cb_appendc(&out, '<'); i += 4;
            } else if (i + 3 < n && s[i+1] == 'g' && s[i+2] == 't' && s[i+3] == ';') {
                cb_appendc(&out, '>'); i += 4;
            } else if (i + 4 < n && s[i+1] == 'a' && s[i+2] == 'm' && s[i+3] == 'p' && s[i+4] == ';') {
                cb_appendc(&out, '&'); i += 5;
            } else if (i + 5 < n && s[i+1] == 'q' && s[i+2] == 'u' && s[i+3] == 'o' && s[i+4] == 't' && s[i+5] == ';') {
                cb_appendc(&out, '"'); i += 6;
            } else if (i + 5 < n && s[i+1] == 'a' && s[i+2] == 'p' && s[i+3] == 'o' && s[i+4] == 's' && s[i+5] == ';') {
                cb_appendc(&out, '\''); i += 6;
            } else {
                cb_appendc(&out, s[i]); i++;
            }
        } else {
            cb_appendc(&out, s[i]); i++;
        }
    }
    return cb_detach(&out);
}

/* Skip whitespace */
static size_t skip_ws(const char *s, size_t pos, size_t len) {
    while (pos < len && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        pos++;
    return pos;
}

/* Tag path stack */
#define MAX_DEPTH 256
typedef struct {
    char *tags[MAX_DEPTH];
    int   depth;
} TagStack;

static void ts_init(TagStack *ts) { ts->depth = 0; }

static void ts_push(TagStack *ts, const char *tag, size_t tlen) {
    if (ts->depth >= MAX_DEPTH) return;
    char *t = (char *)malloc(tlen + 1);
    if (t) { memcpy(t, tag, tlen); t[tlen] = '\0'; }
    ts->tags[ts->depth++] = t;
}

static void ts_pop(TagStack *ts) {
    if (ts->depth > 0) {
        ts->depth--;
        free(ts->tags[ts->depth]);
        ts->tags[ts->depth] = NULL;
    }
}

static char *ts_path(TagStack *ts) {
    if (ts->depth == 0) return strdup("");
    CharBuf b;
    cb_init(&b);
    for (int i = 0; i < ts->depth; i++) {
        if (i > 0) cb_appendc(&b, '.');
        if (ts->tags[i]) cb_append(&b, ts->tags[i], strlen(ts->tags[i]));
    }
    return cb_detach(&b);
}

static void ts_cleanup(TagStack *ts) {
    for (int i = 0; i < ts->depth; i++) {
        free(ts->tags[i]);
        ts->tags[i] = NULL;
    }
    ts->depth = 0;
}

/* ------------------------------------------------------------------ */
/* Parser API                                                          */
/* ------------------------------------------------------------------ */

XmlNodeArray xml_parse(const char *xml) {
    XmlNodeArray result = { NULL, 0 };
    if (!xml || !*xml) return result;

    NodeVec nodes;
    nv_init(&nodes);
    TagStack stack;
    ts_init(&stack);

    size_t len = strlen(xml);
    size_t pos = 0;

    while (pos < len) {
        /* Look for next '<' */
        size_t text_start = pos;
        while (pos < len && xml[pos] != '<') pos++;

        /* Text content before this tag — we'll capture it below when we
         * encounter a closing tag. Skip for now. */

        if (pos >= len) break;

        /* We're at '<' */
        pos++; /* skip '<' */
        if (pos >= len) break;

        /* XML declaration: <?...?> */
        if (xml[pos] == '?') {
            while (pos < len && !(xml[pos] == '?' && pos + 1 < len && xml[pos+1] == '>'))
                pos++;
            if (pos < len) pos += 2; /* skip ?> */
            continue;
        }

        /* Comment: <!--...--> */
        if (pos + 2 < len && xml[pos] == '!' && xml[pos+1] == '-' && xml[pos+2] == '-') {
            pos += 3;
            while (pos + 2 < len && !(xml[pos] == '-' && xml[pos+1] == '-' && xml[pos+2] == '>'))
                pos++;
            if (pos + 2 < len) pos += 3; /* skip --> */
            continue;
        }

        /* CDATA: <![CDATA[...]]> */
        if (pos + 7 < len && xml[pos] == '!' && xml[pos+1] == '[' &&
            xml[pos+2] == 'C' && xml[pos+3] == 'D' && xml[pos+4] == 'A' &&
            xml[pos+5] == 'T' && xml[pos+6] == 'A' && xml[pos+7] == '[') {
            pos += 8;
            size_t cdata_start = pos;
            while (pos + 2 < len && !(xml[pos] == ']' && xml[pos+1] == ']' && xml[pos+2] == '>'))
                pos++;
            /* CDATA content is text content for current element — collect as text */
            /* For simplicity, we emit a node for the current tag path with CDATA content */
            if (stack.depth > 0) {
                char *path = ts_path(&stack);
                char *cdata_content = xml_unescape(xml + cdata_start, pos - cdata_start);
                XmlNode node = { path, cdata_content, strdup("") };
                nv_push(&nodes, node);
            }
            if (pos + 2 < len) pos += 3; /* skip ]]> */
            continue;
        }

        /* DOCTYPE or other !-directives: skip */
        if (xml[pos] == '!') {
            while (pos < len && xml[pos] != '>') pos++;
            if (pos < len) pos++;
            continue;
        }

        /* Closing tag: </tag> */
        if (xml[pos] == '/') {
            pos++; /* skip / */
            while (pos < len && xml[pos] != '>') pos++;

            /* Text content is everything from text_start to the '<' of this closing tag */
            size_t close_lt = text_start;
            while (close_lt < len && xml[close_lt] != '<') close_lt++;

            if (close_lt > text_start && stack.depth > 0) {
                /* There is text content */
                char *path = ts_path(&stack);
                char *text = xml_unescape(xml + text_start, close_lt - text_start);
                /* Check if the text is only whitespace */
                int only_ws = 1;
                for (size_t k = 0; text[k]; k++) {
                    if (text[k] != ' ' && text[k] != '\t' && text[k] != '\n' && text[k] != '\r') {
                        only_ws = 0; break;
                    }
                }
                if (!only_ws) {
                    XmlNode node = { path, text, strdup("") };
                    nv_push(&nodes, node);
                } else {
                    free(path);
                    free(text);
                }
            }

            ts_pop(&stack);
            if (pos < len) pos++; /* skip > */
            continue;
        }

        /* Opening tag: <tag attrs> or self-closing <tag attrs /> */
        size_t tag_start = pos;
        /* Read tag name (includes namespace prefix) */
        while (pos < len && xml[pos] != ' ' && xml[pos] != '\t' &&
               xml[pos] != '\n' && xml[pos] != '>' && xml[pos] != '/')
            pos++;
        size_t tag_end = pos;
        size_t taglen = tag_end - tag_start;

        /* Read attributes */
        pos = skip_ws(xml, pos, len);
        size_t attr_start = pos;
        int self_closing = 0;
        while (pos < len && xml[pos] != '>' && xml[pos] != '/') {
            if (xml[pos] == '"') {
                pos++;
                while (pos < len && xml[pos] != '"') pos++;
                if (pos < len) pos++;
            } else if (xml[pos] == '\'') {
                pos++;
                while (pos < len && xml[pos] != '\'') pos++;
                if (pos < len) pos++;
            } else {
                pos++;
            }
        }
        size_t attr_end = pos;

        if (pos < len && xml[pos] == '/') {
            self_closing = 1;
            pos++;
        }
        if (pos < len && xml[pos] == '>') pos++;

        /* Trim trailing whitespace from attrs */
        while (attr_end > attr_start &&
               (xml[attr_end-1] == ' ' || xml[attr_end-1] == '\t' ||
                xml[attr_end-1] == '\n' || xml[attr_end-1] == '\r'))
            attr_end--;

        if (self_closing) {
            /* Emit node with current stack + this tag */
            ts_push(&stack, xml + tag_start, taglen);
            char *path = ts_path(&stack);
            char *attrs = (attr_end > attr_start)
                ? strndup(xml + attr_start, attr_end - attr_start)
                : strdup("");
            XmlNode node = { path, strdup(""), attrs };
            nv_push(&nodes, node);
            ts_pop(&stack);
        } else {
            /* Push onto stack */
            ts_push(&stack, xml + tag_start, taglen);
            /* If there are attributes, emit a node now */
            if (attr_end > attr_start) {
                char *path = ts_path(&stack);
                char *attrs = strndup(xml + attr_start, attr_end - attr_start);
                XmlNode node = { path, strdup(""), attrs };
                nv_push(&nodes, node);
            }
        }
    }

    ts_cleanup(&stack);

    result.nodes = nodes.items;
    result.count = nodes.len;
    return result;
}

const char *xml_get(const char *xml, const char *tag) {
    if (!xml || !tag) return NULL;
    XmlNodeArray arr = xml_parse(xml);
    const char *result = NULL;
    for (int i = 0; i < arr.count; i++) {
        if (arr.nodes[i].tag && strcmp(arr.nodes[i].tag, tag) == 0) {
            if (arr.nodes[i].content && *arr.nodes[i].content) {
                result = strdup(arr.nodes[i].content);
                break;
            }
        }
    }
    xml_free(&arr);
    return result;
}

const char *xml_attr(const char *element, const char *attr_name) {
    if (!element || !attr_name) return NULL;
    size_t nlen = strlen(attr_name);
    const char *p = element;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;

        /* Read attribute name */
        const char *name_start = p;
        while (*p && *p != '=' && *p != ' ' && *p != '\t') p++;
        size_t name_len = (size_t)(p - name_start);

        /* Skip whitespace around = */
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') { /* malformed, skip */
            while (*p && *p != ' ' && *p != '\t') p++;
            continue;
        }
        p++; /* skip = */
        while (*p == ' ' || *p == '\t') p++;

        /* Read value */
        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p;
            p++;
        }
        const char *val_start = p;
        if (quote) {
            while (*p && *p != quote) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '/') p++;
        }
        size_t val_len = (size_t)(p - val_start);
        if (quote && *p) p++; /* skip closing quote */

        /* Check if this is the attribute we want */
        if (name_len == nlen && memcmp(name_start, attr_name, nlen) == 0) {
            return xml_unescape(val_start, val_len);
        }
    }
    return NULL;
}

void xml_free(XmlNodeArray *arr) {
    if (!arr || !arr->nodes) return;
    for (int i = 0; i < arr->count; i++) {
        free((void *)arr->nodes[i].tag);
        free((void *)arr->nodes[i].content);
        free((void *)arr->nodes[i].attrs);
    }
    free(arr->nodes);
    arr->nodes = NULL;
    arr->count = 0;
}
