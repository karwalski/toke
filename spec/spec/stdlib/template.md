# std.template

Template compilation, rendering with variable substitution, HTML element construction, and HTML escaping.

## Types

```
type tmpl     { id: u64, src: str }                -- opaque compiled template
type tmplvars { entries: @($str:$str) }             -- variable bindings map
type tmplerr  { msg: str, pos: u64 }                -- error with byte offset
```

## Functions

```
f=tpl.compile(src:str):$tmpl!$tmplerr
f=tpl.render(t:$tmpl;vars:$tmplvars):$str!$tmplerr
f=tpl.vars(pairs:@($str:$str)):$tmplvars
f=tpl.html(tag:$str;attrs:@($str:$str);children:@$str):$str
f=tpl.escape(s:$str):$str
f=tpl.renderfile(path:$str;vars:$tmplvars):$str!$tmplerr
```

## Semantics

- `compile` parses a string containing `{{varname}}` slots into an opaque template handle. Returns `tmplerr` on malformed slot syntax. Slots survive compress/decompress.
- `render` substitutes each `{{name}}` with the corresponding value from `vars`. Returns `tmplerr` if a slot has no binding.
- `vars` constructs a `tmplvars` binding map from string key-value pairs.
- `html` builds an HTML element string from a tag name, attribute pairs, and pre-rendered child strings.
- `escape` performs HTML entity escaping: `&` `<` `>` `"` `'`.
- `renderfile` loads a template from disk, compiles, and renders in one call. Returns `tmplerr` on file read failure or invalid slots.

## Dependencies

None.
