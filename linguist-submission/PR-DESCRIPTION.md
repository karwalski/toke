# Add toke programming language

## Language details

| Field | Value |
|-------|-------|
| **Name** | toke |
| **Type** | programming |
| **Extension** | `.tk` |
| **Color** | `#c792ea` |
| **TextMate scope** | `source.toke` |
| **Ace mode** | `text` |
| **Language ID** | 847291 (placeholder -- will run `script/update-ids`) |

## What is toke?

toke is a statically typed, compiled programming language designed for token-efficient source code. It compiles to native binaries via LLVM and targets systems programming, HTTP services, and data processing. The language uses a deliberately compact syntax (56-character default alphabet) with explicit error handling, exhaustive pattern matching, and arena-based memory management.

- Language specification: https://github.com/karwalski/toke/blob/main/docs/spec/toke-spec-v0.3.md
- Language guide and tour: https://github.com/karwalski/toke/blob/main/docs/guide/tour.md
- Grammar (EBNF): https://github.com/karwalski/toke/blob/main/docs/spec/grammar.ebnf

## Tree-sitter grammar

A tree-sitter grammar exists at:
https://github.com/karwalski/toke/tree/main/tree-sitter-toke

The grammar defines `source.toke` as the TextMate scope and `.tk` as the file type. It is licensed under ISC.

## Repositories using toke

- https://github.com/karwalski/toke -- main language repository (compiler, spec, stdlib, docs)
- https://github.com/karwalski/toke-ooke -- companion AI training corpus in toke
- https://github.com/karwalski/loke -- toke standard library and runtime

## Extension conflicts

The `.tk` extension does not appear to be claimed by any existing language in `languages.yml`.

## Sample code

Five sample `.tk` files are included in this PR under `samples/toke/`:

| File | Description |
|------|-------------|
| `data-pipeline.tk` | CSV file reader with field parsing, filtering, and report generation |
| `rest-api.tk` | HTTP JSON service with file I/O, error matching, and multi-worker serving |
| `shapes.tk` | Sum types (tagged unions) with exhaustive pattern matching and array iteration |
| `collections.tk` | Array construction, map operations, filtering, and dynamic collection building |
| `file-errors.tk` | Error union types, nested match expressions, and file processing pipeline |

All samples are drawn from the official toke documentation (cookbook and language guide) and represent real-world usage patterns, not trivial hello-world examples.

## Sample code licensing

All sample code is from the toke project documentation, which is MIT-licensed. Compatible with Linguist's license.

## Checklist

- [x] Added entry to `languages.yml`
- [x] Provided tree-sitter grammar (ISC license)
- [x] Included 5 representative sample files
- [x] Extension `.tk` has no conflicts
- [x] Linked to language spec and repositories
- [ ] Run `script/add-grammar` to vendor the grammar
- [ ] Run `script/update-ids` to assign final `language_id`
- [ ] Verify `.tk` file count meets threshold (2000+ indexed files excluding forks)

## Important note on adoption threshold

Linguist requires at least **200 unique `.tk` files** (or 2000 for common extensions) indexed on GitHub in the last year, excluding forks. Before submitting this PR, verify the count with:

```
https://github.com/search?q=extension%3Atk+NOT+fork%3Atrue&type=code
```

If the threshold is not yet met, this PR should be held until sufficient `.tk` files exist across public repositories.
