# std.html

Programmatic HTML document construction: create documents, add elements, and render to string.

## Types

```
type htmldoc  { id: u64 }                                  -- opaque
type htmlnode { tag: str, attrs: [[str]], content: str }
```

## Functions

```
f=doc():htmldoc
f=title(d:htmldoc;text:str):void
f=style(d:htmldoc;css:str):void
f=script(d:htmldoc;js:str):void
f=div(attrs:[[str]];children:[htmlnode]):htmlnode
f=p(text:str):htmlnode
f=h1(text:str):htmlnode
f=h2(text:str):htmlnode
f=table(headers:[str];rows:[[str]]):htmlnode
f=append(d:htmldoc;node:htmlnode):void
f=render(d:htmldoc):str
f=escape(s:str):str
```

## Semantics

- `doc` creates an empty HTML5 document (`<!DOCTYPE html>`).
- `title` sets the `<title>` in `<head>`. `style` appends a `<style>` block. `script` appends a `<script>` block.
- `div`, `p`, `h1`, `h2` construct element nodes. `div` takes attribute key-value pairs and child nodes.
- `table` builds a `<table>` with `<thead>` from `headers` and `<tbody>` from `rows`.
- `append` adds a node to the document `<body>`.
- `render` serializes the full document to an HTML string.
- `escape` performs HTML entity escaping: `&` `<` `>` `"` `'`.
- `htmlnode.attrs` is a list of key-value pairs (e.g., `[["class", "main"], ["id", "app"]]`).

## Dependencies

None.
