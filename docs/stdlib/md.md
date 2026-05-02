---
title: std.md -- CommonMark Markdown Rendering
slug: md
section: reference/stdlib
order: 26
---

## Overview

The `std.md` module renders CommonMark Markdown to HTML. It is implemented via FFI to [cmark](https://github.com/commonmark/cmark), the CommonMark reference implementation (MIT licence), vendored at `stdlib/vendor/cmark/`.

`md.render` is infallible: cmark never signals a parse error, so invalid or malformed Markdown degrades gracefully and always produces some HTML output. Raw HTML in the source is passed through unchanged (`CMARK_OPT_UNSAFE`).

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
  -- html = "<h1>Hello</h1>\n<p>World.</p>\n"

  let empty = md.render("");
  -- empty = ""
  < 0
};
```

### md.renderfile(path: $str): $str!$mderr

Reads the file at `path` and renders its Markdown content to HTML. Returns `$mderr` if the file cannot be opened or read.

**Example:**
```toke
m=example;
i=md:std.md;
i=io:std.io;
i=str:std.str;

f=demo():i64{
  let result = md.renderfile("docs/intro.md");
  result|{
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

Possible `msg` values: `"file not found"`, `"seek failed"`, `"ftell failed"`, `"allocation failed"`, `"null path"`.
