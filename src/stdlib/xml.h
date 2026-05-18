#ifndef TK_STDLIB_XML_H
#define TK_STDLIB_XML_H

/*
 * xml.h — C interface for the std.xml standard library module.
 *
 * Type mappings:
 *   XmlNode      = struct { tag, content, attrs }
 *   XmlNodeArray = struct { nodes, count }
 *
 * All returned strings are heap-allocated; caller owns them.
 * No external XML library dependencies — pure libc implementation.
 *
 * Story: 88.6.1 / 88.6.2
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Builder API — Story 88.6.1                                          */
/* ------------------------------------------------------------------ */

/* xml_element: build <tag attrs>content</tag>.
 * If attrs is NULL or "", no attributes are emitted.
 * Returns heap-allocated string. */
const char *xml_element(const char *tag, const char *content, const char *attrs);

/* xml_element_self_closing: build <tag attrs />.
 * If attrs is NULL or "", emits <tag />. */
const char *xml_element_self_closing(const char *tag, const char *attrs);

/* xml_escape: escape XML special characters: < > & " '
 * Returns heap-allocated string. */
const char *xml_escape(const char *s);

/* xml_cdata: wrap content in <![CDATA[...]]>.
 * Returns heap-allocated string. */
const char *xml_cdata(const char *s);

/* xml_declaration: build <?xml version="..." encoding="..."?>.
 * If encoding is NULL, omits the encoding attribute. */
const char *xml_declaration(const char *version, const char *encoding);

/* xml_comment: build <!-- text -->.
 * Returns heap-allocated string. */
const char *xml_comment(const char *text);

/* xml_element_ns: build <ns:tag attrs>content</ns:tag>.
 * If ns is NULL or "", behaves like xml_element. */
const char *xml_element_ns(const char *ns, const char *tag, const char *content, const char *attrs);

/* ------------------------------------------------------------------ */
/* Parser API — Story 88.6.2                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *tag;      /* tag path e.g. "Envelope.Body.Response" */
    const char *content;  /* text content (heap-allocated) */
    const char *attrs;    /* raw attribute string (heap-allocated) */
} XmlNode;

typedef struct {
    XmlNode *nodes;
    int      count;
} XmlNodeArray;

/* xml_parse: parse XML string into a flat list of nodes.
 * Nested elements are flattened with dotted tag paths.
 * Returns XmlNodeArray; caller must call xml_free(). */
XmlNodeArray xml_parse(const char *xml);

/* xml_get: get text content of first element matching tag.
 * Returns heap-allocated string, or NULL if not found. */
const char *xml_get(const char *xml, const char *tag);

/* xml_attr: extract attribute value from an element's attribute string.
 * Returns heap-allocated string, or NULL if not found. */
const char *xml_attr(const char *element, const char *attr_name);

/* xml_free: free all nodes and their strings. */
void xml_free(XmlNodeArray *arr);

#endif /* TK_STDLIB_XML_H */
