# ADR-0002: Source File Extension

**Date:** 2026-04-01
**Status:** proposed
**Deciders:** M. Karwalski

## Context

toke source files currently use the `.tk` extension. The spec (v02, Section 2)
states: "File extension: .tk" and defines a source file as "a UTF-8 encoded
text file with extension `.tk`". The compiler (`tkc`) accepts any filename
without extension validation, but all test fixtures, corpus tooling, and
documentation use `.tk` consistently.

Before the extension becomes load-bearing in package registries, editor plugins,
and the corpus (target: ~500,000 files), the choice needs an explicit audit
for conflicts and a recorded decision.

## Conflict Analysis for `.tk`

### GitHub Linguist

GitHub Linguist maps `.tk` to **Tcl** in `languages.yml`:

```yaml
Tcl:
  extensions:
  - ".tcl"
  - ".adp"
  - ".tm"
  - ".tk"
```

This means any `.tk` file pushed to GitHub will be auto-detected as Tcl. GitHub
repository language statistics will misreport toke repos as Tcl-heavy. Syntax
highlighting will apply Tcl rules, not toke rules.

This is fixable per-repo via `.gitattributes` overrides:

```
*.tk linguist-language=Toke
```

However, this requires every toke repository to carry the override, and
"Toke" would not be a recognised Linguist language until a PR is accepted
upstream (requires 200+ repos or significant adoption).

### Tcl/Tk toolkit

Tcl/Tk has used `.tk` for GUI scripts since 1991. While the RFC (Section 3.2)
assessed the *naming* conflict as low-risk (different case, different domain),
the *file extension* conflict is more concrete: editors with Tcl plugins will
apply Tcl syntax highlighting to toke files, and file managers may associate
`.tk` with Tcl interpreters.

### Other uses

- **Unity game engine:** Uses `.tk` as an asset container format (binary).
- **WinTek 3D / embroidery:** Uses `.tk` for model/pattern data.
- **TeKton3D:** Uses `.tk` for architectural project files.

None of these are programming languages, so tooling confusion is limited to
file manager associations, not editor/IDE conflicts.

## Options Considered

### Option A: Keep `.tk`

- **Pro:** Already used everywhere in the project (spec, compiler tests, corpus
  tooling, RFC). Short, memorable, consistent with the `tk` shorthand pattern
  (language=toke, files=.tk, compiler=tkc, interface=.tki).
- **Pro:** The compiler does not enforce the extension, so the convention is
  soft and could be changed later without compiler modifications.
- **Con:** GitHub Linguist classifies it as Tcl. Every repo needs a
  `.gitattributes` workaround until Linguist adds toke.
- **Con:** Editors with Tcl support will mis-highlight toke files by default.
- **Migration cost:** Zero (status quo).

### Option B: `.toke`

- **Pro:** Completely unambiguous. No known conflicts in any file extension
  database. Immediately searchable and self-documenting.
- **Pro:** GitHub Linguist has no mapping for `.toke`, so it would be
  classified as "Other" until a Linguist PR is accepted (better than being
  misclassified as Tcl).
- **Con:** Longer (5 chars vs 3). Breaks the Go-style brevity pattern.
- **Con:** Inconsistent with `.tki` (interface files) unless those also change.
- **Migration cost:** Moderate. Requires updating: spec, all test fixtures
  (~10 files in tkc), corpus tooling (compiler.py, loop.py, runner.py),
  conformance scripts, documentation. No compiler source change needed (no
  extension validation).

### Option C: `.tok`

- **Pro:** Short (4 chars). Closer to `.tk` in brevity.
- **Con:** Conflicts with Borland C++ external token files (`.tok`).
  While Borland C++ is largely obsolete, the association persists in file
  extension databases and some tooling.
- **Con:** Also used by Pro/ENGINEER CAD token files and WordPerfect
  linguistic data files.
- **Migration cost:** Same as Option B.

### Option D: `.tke`

- **Pro:** Short (4 chars), no major conflicts found.
- **Con:** TKE is the name of an existing code editor (tke.sourceforge.io),
  written in Tcl/Tk. While it does not own the `.tke` extension, the name
  collision adds confusion.
- **Con:** Does not obviously derive from "toke" -- looks like an abbreviation
  of something else.
- **Migration cost:** Same as Option B.

## Decision

**Keep `.tk`** (Option A), with the following mitigations:

1. All toke repositories SHALL include a `.gitattributes` file with
   `*.tk linguist-language=Toke` to prevent Tcl misclassification on GitHub.

2. When toke reaches the adoption threshold for GitHub Linguist inclusion
   (200+ repositories or equivalent community evidence), a Linguist PR SHALL
   be submitted to register toke with `.tk` as an extension, including a
   heuristic rule to disambiguate from Tcl (toke files begin with a `module`
   declaration; Tcl `.tk` files typically begin with `package require Tk` or
   `#!/usr/bin/wish`).

3. Editor/IDE plugins (future work) SHALL register for `.tk` files and use
   the `module` declaration as a detection heuristic.

4. The `.tki` interface extension is unaffected and has no known conflicts.

## Rationale

The `.tk` extension is deeply embedded in the project identity: the language
is toke, the shorthand is tk, the compiler is tkc, interface files are .tki,
and source files are .tk. This mirrors the Go pattern (language=Go,
files=.go) and provides a cohesive naming system.

The Tcl/Tk file extension conflict is real but manageable. Tcl's own primary
extension is `.tcl`, and `.tk` is its fourth-listed Linguist extension.
Tcl/Tk usage has been declining for decades, and new Tcl projects
overwhelmingly use `.tcl`. The disambiguation heuristic (toke files always
start with `module`) is robust and machine-checkable.

Switching to `.toke` would eliminate the conflict entirely but would sacrifice
the naming consistency that makes the toke ecosystem feel cohesive. Given that
the conflict is solvable with `.gitattributes` in the short term and a
Linguist PR in the medium term, the cost of keeping `.tk` is low.

## Consequences

- Every toke repository must carry a `.gitattributes` override until Linguist
  recognises toke natively. This is a one-line file and can be templated.
- Users who also write Tcl/Tk may experience editor confusion until toke
  editor plugins exist with proper detection heuristics.
- If the Linguist PR is rejected (unlikely given a robust heuristic and
  sufficient adoption), this decision should be revisited in favour of
  Option B (`.toke`).

## References

- GitHub Linguist languages.yml: Tcl entry includes `.tk`
  (https://github.com/github-linguist/linguist/blob/main/lib/linguist/languages.yml)
- toke spec v02, Section 2: "File extension: .tk"
- toke RFC, Section 3.2: namespace conflict analysis
- Linguist overrides documentation:
  https://github.com/github-linguist/linguist/blob/main/docs/overrides.md
