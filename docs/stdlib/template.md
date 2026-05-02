---
title: std.template
slug: template
section: reference/stdlib
order: 35
---

**Status: Implemented** -- C runtime backing.

The `std.template` module compiles template strings with `{{varname}}` slots, renders them with variable bindings, and provides helper functions for constructing HTML elements and escaping content.

## Types

### $tmpl

An opaque compiled template handle. Slots are `{{varname}}` atoms that survive compress/decompress.

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | Opaque compiled template handle |
| src | $str | Original template source |

### $tmplvars

Variable bindings map: string key to string value.

| Field | Type | Meaning |
|-------|------|---------|
| entries | @($str:$str) | Variable bindings map: key to value |

### $tmplerr

Template error with human-readable message and byte offset in source.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable error message |
| pos | u64 | Byte offset in source where the error occurred |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `tpl.compile` | `src: $str` | `$tmpl!$tmplerr` | Compile a template string with `{{slot}}` placeholders |
| `tpl.render` | `t: $tmpl; vars: $tmplvars` | `$str!$tmplerr` | Render a compiled template with variable bindings |
| `tpl.vars` | `pairs: @($str:$str)` | `$tmplvars` | Construct a variable bindings map from key-value pairs |
| `tpl.html` | `tag: $str; attrs: @($str:$str); children: @($str)` | `$str` | Build an HTML element string from tag, attributes, and children |
| `tpl.escape` | `s: $str` | `$str` | HTML-escape a string (`&`, `<`, `>`, `"`, `'`) |
| `tpl.renderfile` | `path: $str; vars: $tmplvars` | `$str!$tmplerr` | Load, compile, and render a template file |

## Usage

```toke
m=app;
i=tpl:std.template;
i=log:std.log;

f=main():i64{
  let t=tpl.compile("Hello, {{name}}!")|{$ok:t t;$err:e $tmpl{id:0;src:""}};
  let vars=tpl.vars(@("name":"world"));
  let out=tpl.render(t;vars)|{$ok:s s;$err:e ""};
  log.info(out;@());
  <0
};
```

## Combined Example

```toke
m=app;
i=tpl:std.template;
i=file:std.file;
i=log:std.log;

f=main():i64{
  let src="<html><body><h1>{{title}}</h1><p>{{body}}</p></body></html>";
  let t=tpl.compile(src)|{$ok:t t;$err:e $tmpl{id:0;src:""}};

  let userinput="<script>alert('xss')</script>";
  let safe=tpl.escape(userinput);

  let vars=tpl.vars(@("title":"Welcome";"body":safe));
  let page=tpl.render(t;vars)|{$ok:s s;$err:e ""};

  file.write("out.html";page)|{$ok:v log.info("page written";@());$err:e log.warn("write failed";@())};
  <0
};
```

## Dependencies

None.
