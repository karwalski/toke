# toke specification (redirect)

The canonical specification now lives at **[`docs/spec/`](../docs/spec/)** in this repository.
The normative text is [`docs/spec/toke-spec-v0.4.md`](../docs/spec/toke-spec-v0.4.md);
`toke-spec-v0.3.md` beside it is historical and superseded.

All normative spec content (grammar, semantics, errors, stdlib signatures) and
decision records (ADRs, gate decisions) have been consolidated into `docs/`.

## What remains here

- `rfc/` — the toke RFC document (canonical location)
- `.github/ISSUE_TEMPLATE/` — GitHub issue templates for the spec repo
- `examples/` — example program placeholders
- `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, `LICENSE`, `NOTICE` — repo-level legal/governance files

## Canonical locations

| Content | Location |
|---------|----------|
| Language spec | `docs/spec/toke-spec-v0.4.md` |
| Formal grammar | `docs/spec/grammar.ebnf`, `docs/spec/toke.gbnf` |
| Semantics | `docs/spec/semantics.md` |
| Error registry | `docs/spec/errors.md` |
| Stdlib signatures | `docs/spec/stdlib-signatures.md` |
| Decision records | `docs/decisions/` |
| Stdlib module docs | `docs/stdlib/` |

## Archived duplicates

Previous copies of spec and decision files that lived here were moved to
the local workspace archive (`~/tk/archive/docs/spec-repo-duplicates/`) on 2026-04-25. The `docs/` versions
are authoritative (they contain YAML frontmatter for the website and have
received updates not present in the old copies).

## Licence

MIT. See [LICENSE](LICENSE).
