# std.template -- HTML/JS/CSS Template Generation

## Overview

The `std.template` module provides typed template compilation and rendering for producing HTML, JS, and CSS content from toke programs. Templates use `{{varname}}` slot syntax for variable substitution. The module is focused on correctness, HTML safety, and composability from toke types.

Import with:

```toke
i=tpl:std.template;
```

## Template Slot Format

Template strings use `{{varname}}` as the slot syntax for variable substitution.

**Rules:**

- A slot is `{{` followed by one or more identifier characters (`[a-zA-Z_][a-zA-Z0-9_]*`) followed by `}}`.
- Slot names are case-sensitive: `{{Title}}` and `{{title}}` are distinct.
- Slots must be matched by an entry in the `$tmplvars` binding map at render time. An unmatched slot produces a `$tmplerr`.
- Literal `{{` in output is not supported; structure content via `tpl.html` or pre-escape with `tpl.escape`.

**Slot survival guarantee:** Slot atoms (`{{varname}}`) are treated as opaque during compress/decompress operations. The toke runtime preserves them byte-for-byte through any serialisation round-trip.

## Types

### $tmpl

An opaque compiled template handle. Holds the parsed slot positions and the original source string. Created by `tpl.compile`.

| Field | Type | Description |
|-------|------|-------------|
| `id`  | `u64` | Runtime handle identifier (opaque). |
| `src` | `$str` | Original template source string. |

### $tmplvars

A variable binding map associating slot names to their substitution values.

| Field | Type | Description |
|-------|------|-------------|
| `entries` | `@($str:$str)` | Array of key:value string pairs. |

### $tmplerr

A template error with a human-readable message and the byte offset in the template source where the error was detected.

| Field | Type | Description |
|-------|------|-------------|
| `msg` | `$str` | Human-readable error description. |
| `pos` | `u64`  | Byte offset in the template source (0-based). |

## Functions

### tpl.compile(src: $str): $tmpl!$tmplerr

Compiles a template string containing `{{varname}}` slots into an opaque `$tmpl` handle. Returns `$tmplerr` if the template contains malformed slot syntax (e.g., `{{` without a matching `}}`).

Compilation is a pure operation: the same `$tmpl` handle can be rendered multiple times with different `$tmplvars` bindings.

**Example:**
```toke
let t = tpl.compile("<p>{{content}}</p>");
let bad = tpl.compile("{{broken");  (* err($tmplerr{msg:"unclosed slot"; pos:0}) *)
```

### tpl.render(t: $tmpl; vars: $tmplvars): $str!$tmplerr

Renders compiled template `t` by substituting each `{{name}}` slot with the corresponding value from `vars`. Iterates slots left-to-right; substitution values are inserted verbatim (not re-escaped).

Returns `$tmplerr` if any slot name in the template has no entry in `vars`.

**Example:**
```toke
let t = tpl.compile("<h1>{{title}}</h1>");
let v = tpl.vars(@("title":"Hello"));
let out = tpl.render(t; v);  (* out = ok("<h1>Hello</h1>") *)
```

### tpl.vars(pairs: @($str:$str)): $tmplvars

Constructs a `$tmplvars` binding map from an array of string:string pairs. The resulting map is used with `tpl.render` and `tpl.renderfile`.

**Example:**
```toke
let v = tpl.vars(@("name":"Alice"; "role":"admin"));
```

### tpl.renderfile(path: $str; vars: $tmplvars): $str!$tmplerr

Loads a template from the file at `path`, compiles it, and renders with `vars` in a single call. Convenience wrapper around `tpl.compile` followed by `tpl.render`.

Returns `$tmplerr` if:
- The file does not exist or cannot be read.
- The file content contains malformed slot syntax.
- A slot name in the file has no entry in `vars`.

**Example:**
```toke
let page = tpl.renderfile("templates/index.html"; tpl.vars(@("title":"Home")));
```

### tpl.html(tag: $str; attrs: @($str:$str); children: @$str): $str

Constructs an HTML element inline without a template string. `tag` is the element name. `attrs` is an array of attribute name:value pairs (values are HTML-escaped automatically). `children` is an array of already-rendered child strings (not re-escaped).

Returns the complete element as a string.

**Example:**
```toke
let link = tpl.html("a"; @("href":"/home"; "class":"nav"); @("Home"));
(* link = "<a href=\"/home\" class=\"nav\">Home</a>" *)
```

For void elements (e.g., `<br>`, `<img>`), pass an empty children array:

```toke
let img = tpl.html("img"; @("src":"/logo.png"; "alt":"Logo"); @());
(* img = "<img src=\"/logo.png\" alt=\"Logo\">" *)
```

### tpl.escape(s: $str): $str

HTML-escapes a string for safe insertion into HTML attribute values or text content. Applies the following substitutions:

| Character | Escaped form |
|-----------|-------------|
| `&`       | `&amp;`     |
| `<`       | `&lt;`      |
| `>`       | `&gt;`      |
| `"`       | `&quot;`    |
| `'`       | `&#39;`     |

Use `tpl.escape` before inserting untrusted user data into HTML. Values inserted via `tpl.html` attribute slots are escaped automatically.

**Example:**
```toke
let safe = tpl.escape("<script>alert(1)</script>");
(* safe = "&lt;script&gt;alert(1)&lt;/script&gt;" *)
```

## HTML Escaping Policy

`tpl.render` and `tpl.renderfile` do **not** automatically HTML-escape substitution values. This is intentional: templates often compose nested HTML fragments where double-escaping would corrupt the output.

Callers are responsible for escaping untrusted values before inserting them into a template:

```toke
let safe_name = tpl.escape(user_input);
let v = tpl.vars(@("name":safe_name));
```

For inline element construction, `tpl.html` **does** escape attribute values automatically.

## Usage Example

Building a full HTML page using compile, render, and escape together:

```toke
i=tpl:std.template;

M=page;

F=render_page(title: $str; body_html: $str): $str!$tmplerr {
  let tmpl_src = "<!DOCTYPE html><html><head><title>{{title}}</title></head><body>{{body}}</body></html>";
  let t = tpl.compile(tmpl_src)?;
  let v = tpl.vars(@("title":tpl.escape(title); "body":body_html));
  <tpl.render(t; v)
};
```

Composing nested templates by compiling once and rendering many times:

```toke
i=tpl:std.template;

M=list;

F=render_items(items: @$str): $str!$tmplerr {
  let item_tpl = tpl.compile("<li>{{text}}</li>")?;
  let list_tpl = tpl.compile("<ul>{{items}}</ul>")?;
  let parts = @();
  let i = 0;
  loop {
    if i >= items.len { break };
    let rendered = tpl.render(item_tpl; tpl.vars(@("text":tpl.escape(items[i]))))?;
    parts = parts ++ @(rendered);
    i = i + 1;
  };
  let body = str.join(parts; "");
  <tpl.render(list_tpl; tpl.vars(@("items":body)))
};
```

## Notes

- Template compilation is pure: compile once, render many times with different bindings.
- `$tmpl` handles are not serialisable across process boundaries in this version. Compile templates at startup and cache them.
- For large CSS or JS blocks, prefer `tpl.renderfile` over embedding long template strings inline.
- `tpl.html` is best for constructing individual elements; use `tpl.compile`/`tpl.render` for page-level templates with multiple slots.
