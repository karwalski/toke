---
title: std.html
slug: html
section: reference/stdlib
order: 16
---

**Status: Implemented** -- C runtime backing.

The `std.html` module provides an API for building HTML documents programmatically. Create a document, add elements (headings, paragraphs, divs, tables), attach styles and scripts, then render to a string.

## Types

### $htmldoc

Opaque handle to an HTML document being constructed. Its one field, `id` (u64), is a number unique within the process.

### $htmlnode

| Field | Type | Meaning |
|-------|------|---------|
| tag | $str | HTML element tag name |
| attrs | @(@($str)) | Attribute key-value pairs, each `@(name; value)`, in the order they were set |
| content | $str | Inner text content; `""` for an element with none (e.g. a table) |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `html.doc` | | `$htmldoc` | Create a new empty HTML document |
| `html.title` | `doc: $htmldoc; title: $str` | `void` | Set the document title |
| `html.style` | `doc: $htmldoc; css: $str` | `void` | Add an inline `<style>` block |
| `html.script` [quarantined](/docs/reference/tki-quarantine/) | `doc: $htmldoc; js: $str` | `void` | Add an inline `<script>` block |
| `html.div` [quarantined](/docs/reference/tki-quarantine/) | `attrs: @(@($str)); children: @($htmlnode)` | `$htmlnode` | Create a `<div>` with attributes and children |
| `html.p` [quarantined](/docs/reference/tki-quarantine/) | `text: $str` | `$htmlnode` | Create a `<p>` element |
| `html.h1` | `text: $str` | `$htmlnode` | Create an `<h1>` element |
| `html.h2` [quarantined](/docs/reference/tki-quarantine/) | `text: $str` | `$htmlnode` | Create an `<h2>` element |
| `html.table` | `headers: @($str); rows: @(@($str))` | `$htmlnode` | Create an HTML table from headers and row data |
| `html.append` | `doc: $htmldoc; node: $htmlnode` | `void` | Append a node to the document body |
| `html.render` | `doc: $htmldoc` | `$str` | Render the document to an HTML string |
| `html.escape` [quarantined](/docs/reference/tki-quarantine/) | `s: $str` | `$str` | HTML-escape a string |

## Escaping

Text passed to the node constructors (`html.p`, `html.h1`, `html.h2`,
`html.div` content, table headers and cells) is **markup, not text**: it is written
into the document as given, so that a caller can nest elements. Pass anything
that did not come from your own program through `html.escape` first.

What the module escapes for you:

- attribute values (a `div`'s class, for example) are HTML-escaped, and a
  `javascript:`-style URL in a URL attribute is dropped;
- the document title is HTML-escaped;
- `html.style` and `html.script` content cannot close its own element early
  (a `</style>` or `</script>` inside it is neutralised).

The C-level string helpers that are not part of the toke surface
(`html_textarea`, `html_pre`, `html_code`, `html_button`, `html_label`,
`html_ul`/`html_ol`) follow the same rule for their content: it is markup, by
design, not escaped (122.13).

## Usage

```toke
m=example;
i=html:std.html;

f=main():i64{
  let doc = html.doc();
  html.title(doc; "Report");
  html.style(doc; "body { font-family: sans-serif; }");
  html.append(doc; html.h1("Sales Report"));
  html.append(doc; html.table(
    @("Quarter"; "Revenue");
    @(@("Q1"; "$100k"); @("Q2"; "$200k"))
  ));
  let out = html.render(doc);
  <0;
}
```

## Dependencies

None.
