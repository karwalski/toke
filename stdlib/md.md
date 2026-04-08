# std.md -- CommonMark Markdown Rendering

## Overview

The `std.md` module renders CommonMark Markdown to HTML. It is implemented via FFI to [cmark](https://github.com/commonmark/cmark), the CommonMark reference implementation (MIT licence), vendored at `stdlib/vendor/cmark/`.

`md.render` is infallible: cmark never signals a parse error, so invalid or malformed Markdown degrades gracefully and always produces some HTML output. Raw HTML in the source is passed through unchanged (`CMARK_OPT_UNSAFE`).

`md.renderfile` reads a file from disk and then renders its content. It returns a `Str!MdErr` result to propagate file-system errors (file not found, seek failure, allocation failure).

A common use is inside toke templates to render body content stored as Markdown:

```toke
import std.md;

(* In a template context: *)
{= body | md.render =}
```

## Functions

### md.render(src: Str): Str

Renders the CommonMark Markdown string `src` to an HTML string. Always succeeds — malformed or empty Markdown degrades gracefully and never produces an error.

**Example:**
```toke
import std.md;

let html = md.render("# Hello\n\nWorld.");
(* html = "<h1>Hello</h1>\n<p>World.</p>\n" *)

let empty = md.render("");
(* empty = "" *)
```

### md.renderfile(path: Str): Str!MdErr

Reads the file at `path` and renders its Markdown content to HTML. Returns `MdErr` if the file cannot be opened or read.

**Example:**
```toke
import std.md;

match md.renderfile("docs/intro.md") {
  ok(html) => print(html);
  err(e)   => print(str.concat("error: "; e.msg));
}
```

## Error Types

### MdErr

Returned by `md.renderfile` when the file cannot be read.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable description of the file error |

Possible `msg` values: `"file not found"`, `"seek failed"`, `"ftell failed"`, `"allocation failed"`, `"null path"`.
