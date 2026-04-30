# toke specification (redirect)

The canonical specification now lives at **`tk/docs/spec/`** (currently `toke-spec-v0.3.md`).

All normative spec content (grammar, semantics, errors, stdlib signatures) and
decision records (ADRs, gate decisions) have been consolidated into `tk/docs/`.

## What remains here

- `rfc/` — the toke RFC document (canonical location)
- `.github/ISSUE_TEMPLATE/` — GitHub issue templates for the spec repo
- `examples/` — example program placeholders
- `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, `LICENSE`, `NOTICE` — repo-level legal/governance files

## Canonical locations

| Content | Location |
|---------|----------|
| Language spec | `tk/docs/spec/toke-spec-v0.3.md` |
| Formal grammar | `tk/docs/spec/grammar.ebnf` |
| Semantics | `tk/docs/spec/semantics.md` |
| Error registry | `tk/docs/spec/errors.md` |
| Stdlib signatures | `tk/docs/spec/stdlib-signatures.md` |
| Decision records | `tk/docs/decisions/` |
| Stdlib module docs | `tk/docs/stdlib/` |

## Archived duplicates

Previous copies of spec and decision files that lived here were moved to
`tk/archive/docs/spec-repo-duplicates/` on 2026-04-25. The `tk/docs/` versions
are authoritative (they contain YAML frontmatter for the website and have
received updates not present in the old copies).

## Licence

MIT. See [LICENSE](LICENSE).
