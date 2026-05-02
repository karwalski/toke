# toke Documentation

Single source of truth for all toke language documentation. Served as the project website (tokelang.dev) via ooke symlinks.

All content uses **default syntax** (v0.3-syntax-lock, 55-char set). All code samples pass `tkc --check` (370 tested, 364 pass, 6 intentional legacy skips).

## Structure

| Directory | Contents | Files |
|-----------|----------|-------|
| [guide/](guide/) | Getting started, tutorials, learning path | 15 |
| [reference/](reference/) | Language reference -- types, expressions, statements, modules, grammar | 15 |
| [stdlib/](stdlib/) | Standard library reference (41 modules) | 41 |
| [spec/](spec/) | Language specification (normative) -- grammar, semantics, error codes | 6 |
| [cookbook/](cookbook/) | Multi-module worked examples | 3 |
| [about/](about/) | Project info, design philosophy, changelog, ecosystem, loke, ooke | 10 |
| [decisions/](decisions/) | Architecture Decision Records and gate decisions | 4 |
| [compiler/](compiler/) | Compiler internals -- ABI, binary size, lint rules | 5 |

## Frontmatter

Every markdown file has YAML frontmatter for ooke compatibility:

```yaml
---
title: "Page Title"
slug: page-slug
section: reference
order: 1
---
```

The ooke website at `~/tk/archive/toke-website-new/` symlinks `content/docs/` to subdirectories of this folder.

## Testing

Run `/tmp/toke-web-tests/test_docs_code.py` to validate all toke code blocks against `tkc --check`.

## Conventions

- Code examples use default syntax: `m=`, `f=`, `t=`, `i=`, `@()`, `$str`
- No comments inside code blocks -- all commentary is in surrounding markdown
- Intentionally broken examples (error docs) use `text` fences, not `toke`
- Signature-only snippets use `text` fences
- `reference/phase2/` shows Phase 1 syntax intentionally for migration reference

Research and planning documents are in `~/tk/research/`.
