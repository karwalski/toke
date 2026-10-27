---
title: std.xml
slug: xml
section: reference/stdlib
order: 67
---

**Status: Implemented, partial** -- C runtime backing (`src/stdlib/xml.c`, `src/stdlib/xml_glue.c`).

The `std.xml` module builds XML strings and reads simple values back out of them. It is not a validating parser and does not handle namespaces, entities beyond the five predefined ones, or mixed content in general; see the notes on each reader.

The interface (`stdlib/xml.tki`) is generated from the C glue (`"gen":"abi"`): arity is checked, every parameter is declared `i64`. The types in the headings are the ones the implementation reads. Four functions are declared to return `i64` although they return a string handle (or `0`): `xml.get`, `xml.attr`, `xml.cdata` and `xml.comment`. Passing their result to a `$str` parameter works; interpolating it prints a number. That and the `0` on a miss are tracked by 131.48.

## Building

### xml.element(tag: $str; content: $str; attrs: $str): $str

Returns `<tag attrs>content</tag>`, or `<tag>content</tag>` when `attrs` is empty. **Neither `content` nor `attrs` is escaped** -- `content` is placed verbatim so elements nest (`xml.element("root";xml.element("name";"John";"");"")`). Escape text with `xml.escape` first. An empty `content` gives `<tag></tag>`, not a self-closing tag.

### xml.elementns(ns: $str; tag: $str; content: $str; attrs: $str): $str

Returns `<ns:tag attrs>content</ns:tag>`; with an empty `ns`, the same as `xml.element`. It does not add an `xmlns` declaration -- put one in `attrs` if needed.

### xml.escape(s: $str): $str

Replaces `&`, `<`, `>`, `"` and `'` with `&amp;`, `&lt;`, `&gt;`, `&quot;` and `&apos;`.

### xml.cdata(s: $str): i64

Returns `<![CDATA[s]]>`. `s` is not checked for `]]>`, which would end the section early.

### xml.comment(text: $str): i64

Returns `<!-- text -->` (with the spaces). `text` is not checked for `--`.

### xml.declaration(version: $str; encoding: $str): $str

Returns `<?xml version="VERSION" encoding="ENCODING"?>`, or `<?xml version="VERSION"?>` when `encoding` is empty.

## Reading

### xml.get(xml: $str; path: $str): i64

Returns the text content of the first element whose **dotted path from the root** equals `path` -- `xml.get("<a><b>1</b><c>2</c></a>";"a.c")` is `"2"`, and a root element is just its name (`"name"`). Entities in the text are decoded. Returns `0` when no element matches or the element has no text.

The text recorded for an element is what lies between the previous tag and its closing tag; text before a child element is not part of it. A `CDATA` section also counts as text. Whitespace-only text is ignored. The whole string is re-scanned on every call.

### xml.attr(attrs: $str; name: $str): i64

Returns the value of attribute `name` from an **attribute string** such as `href="/x" id='7'` (single or double quotes; entities decoded), or `0` when it is absent. It does not take a whole element or document: pass the attribute text.

### xml.parse(xml: $str): i64

Scans the document into a flat list of nodes in C memory and returns a handle to it. **No exported function reads that handle**, and none frees it, so from toke the result is unusable; use `xml.get` instead.

## Example

```toke
m=example;
i=io:std.io;
i=xml:std.xml;

f=main():i64{
  let name=xml.element("name";xml.escape("Tom & Jerry");"");
  let doc=xml.element("root";name;"");
  io.println(doc);
  <0
};
```

Prints `<root><name>Tom &amp; Jerry</name></root>`.
