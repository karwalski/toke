---
title: std.xml
slug: xml
section: reference/stdlib
order: 55
---

**Status: Implemented** -- C runtime backing (`src/stdlib/xml.c`).

The `std.xml` module builds XML text and reads values out of it. Every
function takes and returns `$str`; there is no document tree type. The
normative signatures are in
[stdlib-signatures](/docs/spec/stdlib-signatures/#stdxml) (131.48).

The builders insert `content` and `attrs` verbatim. Text that may contain
`<`, `&` or quotes must go through `xml.escape` (or `xml.cdata`) first.

## Functions

### xml.element(tag: $str; content: $str; attrs: $str): $str

Returns `<tag attrs>content</tag>`, or `<tag>content</tag>` when `attrs` is
empty. `attrs` is raw attribute text such as `href="/" id='7'`.

### xml.elementns(ns: $str; tag: $str; content: $str; attrs: $str): $str

As `xml.element`, with the tag written `ns:tag`. An empty `ns` gives an
unprefixed tag.

### xml.escape(s: $str): $str

Replaces `&`, `<`, `>`, `"` and `'` with their entity references.

### xml.cdata(s: $str): $str

Returns `<![CDATA[s]]>`.

### xml.comment(text: $str): $str

Returns `<!-- text -->`.

### xml.declaration(version: $str; encoding: $str): $str

Returns `<?xml version="version" encoding="encoding"?>`.

### xml.get(xml: $str; path: $str): $str

Parses `xml` and returns the text content of the first element at the dotted
tag path (`"root.name"` for `<root><name>...</name></root>`) whose text is
not empty. Returns `""` when no element matches, so a missing element and an
empty one read the same. Entity references in the text are decoded.

### xml.attr(attrs: $str; name: $str): $str

Reads attribute `name` from a raw attribute string (single, double or no
quotes) and returns its decoded value, or `""` when it is absent.

### xml.parse(xml: i64): i64

An ABI helper (`"gen":"abi"` in `xml.tki`): it returns an opaque node-array
handle, and no export reads or frees one yet. Use `xml.get`.

## Example

```toke
m=example;
i=io:std.io;
i=xml:std.xml;

f=main():i64{
  let doc=xml.element("user";xml.element("name";xml.escape("Ann & Bo");"");"id=\"7\"");
  io.println(doc);
  io.println(xml.get(doc;"user.name"));
  let missing=xml.get(doc;"user.email");
  if(missing==""){io.println("no email")};
  <0
};
```
