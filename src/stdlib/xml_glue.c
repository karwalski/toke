/* SPDX-License-Identifier: MIT */
/*
 * xml_glue.c — i64-ABI wrappers for std.xml module.
 *
 * Each wrapper casts i64 arguments to C pointer types, calls the
 * underlying xml.h function, and returns the result as i64.
 *
 * Story: 88.6.1 / 88.6.2
 */

#include "xml.h"
#include <stdint.h>
#include <stdlib.h>

/* xml.element(tag, content, attrs) */
int64_t tk_xml_element_w(int64_t tag, int64_t content, int64_t attrs) {
    const char *t = tag     ? (const char *)(intptr_t)tag     : "";
    const char *c = content ? (const char *)(intptr_t)content : "";
    const char *a = attrs   ? (const char *)(intptr_t)attrs   : "";
    return (int64_t)(intptr_t)xml_element(t, c, a);
}

/* xml.parse(xml) — returns opaque pointer to XmlNodeArray */
int64_t tk_xml_parse_w(int64_t xml) {
    if (!xml) return 0;
    const char *s = (const char *)(intptr_t)xml;
    XmlNodeArray *arr = (XmlNodeArray *)malloc(sizeof(XmlNodeArray));
    if (!arr) return 0;
    *arr = xml_parse(s);
    return (int64_t)(intptr_t)arr;
}

/* 131.48: xml.get / xml.attr are declared `:str` (stdlib/xml.tki).  The C
 * core answers NULL for "no such path / attribute"; a toke str is never
 * NULL, and handing one back made the next `==` or interpolation read
 * address 0.  A miss is the empty string, as json.getval's is. */
static int64_t xml_str_or_empty(const char *r) {
    return (int64_t)(intptr_t)(r ? r : "");
}

/* xml.get(xml, path) — text content at a dotted tag path, "" if none */
int64_t tk_xml_get_w(int64_t xml, int64_t tag) {
    if (!xml || !tag) return xml_str_or_empty(NULL);
    const char *s = (const char *)(intptr_t)xml;
    const char *t = (const char *)(intptr_t)tag;
    return xml_str_or_empty(xml_get(s, t));
}

/* xml.attr(element, attrname) — attribute value, "" if absent */
int64_t tk_xml_attr_w(int64_t element, int64_t attrname) {
    if (!element || !attrname) return xml_str_or_empty(NULL);
    const char *e = (const char *)(intptr_t)element;
    const char *a = (const char *)(intptr_t)attrname;
    return xml_str_or_empty(xml_attr(e, a));
}

/* xml.escape(s) */
int64_t tk_xml_escape_w(int64_t s) {
    if (!s) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)xml_escape((const char *)(intptr_t)s);
}

/* xml.cdata(s) */
int64_t tk_xml_cdata_w(int64_t s) {
    if (!s) return xml_str_or_empty(NULL);
    return xml_str_or_empty(xml_cdata((const char *)(intptr_t)s));
}

/* xml.declaration(version, encoding) */
int64_t tk_xml_declaration_w(int64_t version, int64_t encoding) {
    const char *v = version  ? (const char *)(intptr_t)version  : "1.0";
    const char *e = encoding ? (const char *)(intptr_t)encoding : NULL;
    return (int64_t)(intptr_t)xml_declaration(v, e);
}

/* xml.comment(text) */
int64_t tk_xml_comment_w(int64_t text) {
    if (!text) return xml_str_or_empty(NULL);
    return xml_str_or_empty(xml_comment((const char *)(intptr_t)text));
}

/* xml.elementns(ns, tag, content, attrs) */
int64_t tk_xml_elementns_w(int64_t ns, int64_t tag, int64_t content, int64_t attrs) {
    const char *n = ns      ? (const char *)(intptr_t)ns      : "";
    const char *t = tag     ? (const char *)(intptr_t)tag     : "";
    const char *c = content ? (const char *)(intptr_t)content : "";
    const char *a = attrs   ? (const char *)(intptr_t)attrs   : "";
    return (int64_t)(intptr_t)xml_element_ns(n, t, c, a);
}
