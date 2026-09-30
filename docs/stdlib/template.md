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
| id | u64 | A number unique within the process (`tpl.compile` numbers from 1) |
| src | $str | Original template source. `tpl.render` compiles this, so a `$tmpl{id:0;src:"..."}` literal renders the same as `tpl.compile`'s result |

### $tmplvars

Variable bindings map: string key to string value.

| Field | Type | Meaning |
|-------|------|---------|
| entries | @($str:$str) | Variable bindings map: key to value. Build a `$tmplvars` with `tpl.vars` (or a literal); `tpl.render` and `tpl.renderfile` do not take the bare map |

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
| `tpl.render` | `t: $tmpl; vars: $tmplvars` | `$str!$tmplerr` | Render a compiled template with variable bindings, each value escaped for its position (see Escaping) |
| `tpl.vars` | `pairs: @($str:$str)` | `$tmplvars` | Construct a variable bindings map from key-value pairs |
| `tpl.html` [quarantined](/docs/reference/tki-quarantine/) | `tag: $str; attrs: @($str:$str); children: @($str)` | `$str` | Build an HTML element string from tag, attributes, and children |
| `tpl.escape` | `s: $str` | `$str` | HTML-escape a string (`&`, `<`, `>`, `"`, `'`). `tpl.render` already escapes slot values, so a value passed through both is escaped twice |
| `tpl.renderfile` | `path: $str; vars: $tmplvars` | `$str!$tmplerr` | Load, compile, and render a template file, escaping as `tpl.render` does |

## Escaping

`tpl.render` and `tpl.renderfile` escape every `{{slot}}` value for the place in the HTML where the slot sits (story 121.37, ADR-0011). The template's own text is never changed. The position is read from the HTML produced so far, so a slot inside `{{#if}}`, `{{#each}}` or a partial is escaped for the position where it ends up.

| Where the slot is | What happens to the value |
|---|---|
| Text, an HTML comment, `<title>`, `<textarea>` | `&` `<` `>` `"` `'` become entities |
| A quoted attribute value | the same entities |
| An unquoted attribute value | every character other than a letter or digit becomes `&#N;`, so the value cannot end the attribute or add another one |
| The start of a URL attribute (`href`, `src`, `action`, `formaction`, `srcset`, …) | a scheme other than `http`, `https` or `mailto` (`javascript:`, `data:`, `vbscript:`, …) replaces the value with `#ZtokeZ`; otherwise characters not allowed in a URL are percent-encoded |
| A URL attribute after `?` or `#` | percent-encoded as a query or fragment component (`&` → `%26`) |
| `<script>` or an `on*` attribute, inside a JavaScript string or comment | everything but letters, digits, space and `_,.` becomes `\uXXXX` |
| `<script>` or an `on*` attribute, in code | the value becomes a quoted JavaScript string, `"…"`, never code |
| `<style>` or a `style` attribute, inside a CSS string or comment | everything but letters, digits and space becomes `\HH ` |
| `<style>` or a `style` attribute, in code | kept if it is only letters, digits, space and `#%.,_-`; otherwise `ZtokeZ` |
| A tag or attribute name (`<div {{a}}>`) | kept if it is letters, digits, `-` and `_` and does not name an event handler, `style` or a URL attribute; otherwise `ZtokeZ` |

`ZtokeZ` in the output marks a value that was refused. `{{name|raw}}` writes the value unchanged, for HTML you produced yourself and trust; it is the only way to do so.

Two things the escaper does not read: character references written in the template text inside an attribute (`onclick="f(&quot;{{v}}&quot;)"`), and JavaScript regular-expression literals. Where they could mislead it, the JavaScript and CSS escapers' output still contains no quote, slash, asterisk, newline or angle bracket, so the value can come out wrong but not as code.

## Usage

```toke
m=app;
i=tpl:std.template;
i=log:std.log;

f=main():i64{
  let t=mt tpl.compile("Hello, {{name}}!") {$ok:t t;$err:e $tmpl{id:0;src:""}};
  let vars=tpl.vars(@("name":"world"));
  let out=mt tpl.render(t;vars) {$ok:s s;$err:e ""};
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
  let t=mt tpl.compile(src) {$ok:t t;$err:e $tmpl{id:0;src:""}};

  let userinput="<script>alert('xss')</script>";

  let vars=tpl.vars(@("title":"Welcome";"body":userinput));
  let page=mt tpl.render(t;vars) {$ok:s s;$err:e ""};

  mt file.write("out.html";page) {$ok:v log.info("page written";@());$err:e log.warn("write failed";@())};
  <0
};
```

## Dependencies

None.
