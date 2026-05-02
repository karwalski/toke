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

Opaque handle to an HTML document being constructed.

### $htmlnode

| Field | Type | Meaning |
|-------|------|---------|
| tag | $str | HTML element tag name |
| attrs | @(@($str)) | Attribute key-value pairs |
| content | $str | Inner text content |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `html.doc` | | `$htmldoc` | Create a new empty HTML document |
| `html.title` | `doc: $htmldoc; title: $str` | `void` | Set the document title |
| `html.style` | `doc: $htmldoc; css: $str` | `void` | Add an inline `<style>` block |
| `html.script` | `doc: $htmldoc; js: $str` | `void` | Add an inline `<script>` block |
| `html.div` | `attrs: @(@($str)); children: @($htmlnode)` | `$htmlnode` | Create a `<div>` with attributes and children |
| `html.p` | `text: $str` | `$htmlnode` | Create a `<p>` element |
| `html.h1` | `text: $str` | `$htmlnode` | Create an `<h1>` element |
| `html.h2` | `text: $str` | `$htmlnode` | Create an `<h2>` element |
| `html.table` | `headers: @($str); rows: @(@($str))` | `$htmlnode` | Create an HTML table from headers and row data |
| `html.append` | `doc: $htmldoc; node: $htmlnode` | `void` | Append a node to the document body |
| `html.render` | `doc: $htmldoc` | `$str` | Render the document to an HTML string |
| `html.escape` | `s: $str` | `$str` | HTML-escape a string |

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
