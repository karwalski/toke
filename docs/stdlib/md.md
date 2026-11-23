---
title: std.md -- CommonMark Markdown Rendering
slug: md
section: reference/stdlib
order: 26
---

## Overview

The `std.md` module renders CommonMark Markdown to HTML. It is implemented via FFI to [cmark](https://github.com/commonmark/cmark), the CommonMark reference implementation (MIT licence), vendored at `stdlib/vendor/cmark/`.

`md.render` is infallible: cmark never signals a parse error, so invalid or malformed Markdown degrades gracefully and always produces some HTML output.

Raw HTML in the source is not passed through. cmark runs in its default safe mode (`CMARK_OPT_DEFAULT`, story 124.3, ADR-0011), so each raw HTML tag or block -- `<b>`, `<script>`, `<div onclick=...>`, a hand-written `<table>` -- is replaced by the comment `<!-- raw HTML omitted -->`. A link or image whose URL has a dangerous scheme gets an empty URL (`href=""`, `src=""`): `javascript:`, `vbscript:`, `file:`, and `data:` other than `data:image/png`, `data:image/gif`, `data:image/jpeg` and `data:image/webp`, compared case-insensitively. Links inside pipe tables get the same rule (121.16), and the scheme check skips leading spaces and control characters and ignores tabs and line breaks inside the scheme, as a browser does. Text is HTML-escaped as CommonMark specifies.

Security: this protects against script injection when the Markdown comes from an untrusted author, but it is not a full HTML sanitizer. Other schemes (`https:`, `mailto:`, relative URLs) are kept, and images load from wherever their URL points. Markdown that needs real HTML in the output cannot get it through `md.render`; build that part with `std.html` instead.

```toke
m=example;
i=md:std.md;
i=io:std.io;

f=main():i64{
  io.print(md.render("hi <b>x</b>\n\n<script>alert(1)</script>\n\n[a](javascript:alert(2))\n"));
  (* prints:
     <p>hi <!-- raw HTML omitted -->x<!-- raw HTML omitted --></p>
     <!-- raw HTML omitted -->
     <p><a href="">a</a></p>                                        *)
  < 0
};
```

`md.renderfile` reads a file from disk and then renders its content. It returns a `$str!$mderr` result to propagate file-system errors (file not found, seek failure, allocation failure).

## Functions

### md.render(src: $str): $str

Renders the CommonMark Markdown string `src` to an HTML string. Always succeeds -- malformed or empty Markdown degrades gracefully and never produces an error.

**Example:**
```toke
m=example;
i=md:std.md;

f=demo():i64{
  let html = md.render("# Hello\n\nWorld.");
  (* html = "<h1>Hello</h1>\n<p>World.</p>\n" *)

  let empty = md.render("");
  (* empty = "" *)
  < 0
};
```

### md.renderfile(path: $str): $str!$mderr

Reads the file at `path` and renders its Markdown content to HTML. Needs the `fs.read` capability. Returns `$mderr` if the file cannot be opened or read.

**Example:**
```toke
m=example;
i=md:std.md;
i=io:std.io;
i=str:std.str;

f=demo():i64{
  let result = md.renderfile("docs/intro.md");
  mt result {
    $ok:html io.println(html);
    $err:e   io.println(str.concat("error: "; e.msg))
  };
  < 0
};
```

## Error Types

### $mderr

Returned by `md.renderfile` when the file cannot be read.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the file error |

Possible `msg` values: `"file not found"`, `"seek failed"`, `"ftell failed"`, `"allocation failed"`, `"md.renderfile: null path"`. `e.msg` is readable in the `$err` arm (113.B.7; until then reading it stopped the program with `RT005`).
