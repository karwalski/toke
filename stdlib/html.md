# std.html -- HTML Document Builder

## Overview

The `std.html` module provides a structured DOM-like API for building complete HTML documents in toke programs. Functions construct `htmlnode` values that are appended to an `htmldoc` handle. `html.render()` emits a complete HTML5 string. Use `std.dashboard` for a higher-level dashboard composition layer.

## Types

### htmldoc

An opaque document builder handle. Created by `html.doc()`.

### htmlnode

A single HTML element.

| Field | Type | Meaning |
|-------|------|---------|
| tag | Str | Element tag name (e.g., `"div"`, `"p"`) |
| attrs | @(@Str) | Key-value attribute pairs |
| content | Str | Inner content (text or serialised child nodes) |

## Functions

### html.doc(): htmldoc

Creates an empty HTML document builder.

**Example:**
```toke
let d = html.doc();
```

### html.title(d: htmldoc; t: Str): void

Sets the document `<title>`.

### html.style(d: htmldoc; css: Str): void

Appends a `<style>` block with the given CSS to the document `<head>`.

### html.script(d: htmldoc; js: Str): void

Appends a `<script>` block with the given JavaScript to the document `<head>`.

### html.div(attrs: @(@Str); children: @htmlnode): htmlnode

Creates a `<div>` element with the given attributes and child nodes.

**Example:**
```toke
let section = html.div(
  @(@("class"; "card"); @("id"; "main"));
  @(html.h2("Title"); html.p("Body text."))
);
```

### html.p(text: Str): htmlnode

Creates a `<p>` paragraph element.

### html.h1(text: Str): htmlnode

Creates an `<h1>` heading element.

### html.h2(text: Str): htmlnode

Creates an `<h2>` heading element.

### html.table(headers: @Str; rows: @(@Str)): htmlnode

Creates a `<table>` element with a `<thead>` row and `<tbody>` rows.

**Example:**
```toke
let tbl = html.table(
  @("Name"; "Score");
  @(@("Alice"; "42"); @("Bob"; "37"))
);
```

### html.append(d: htmldoc; node: htmlnode): void

Appends `node` to the document `<body>`.

### html.render(d: htmldoc): Str

Emits the complete HTML5 document as a string, including `<!DOCTYPE html>`, `<head>`, and `<body>`.

**Example:**
```toke
let d = html.doc();
html.title(d; "My Report");
html.append(d; html.h1("Hello"));
let out = html.render(d);
file.write("/tmp/report.html"; out);
```

### html.escape(s: Str): Str

HTML-escapes `s`, replacing `&`, `<`, `>`, `"`, and `'` with their entity equivalents.

**Example:**
```toke
let safe = html.escape("<script>alert('xss')</script>");
(* safe = "&lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;" *)
```
