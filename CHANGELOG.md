# Changelog — toke

All notable changes to the toke language and its reference compiler `tkc`.
The version printed by `tkc --version` is defined in `src/main.c` (`VERSION`)
and mirrored in the repository-root `VERSION` file; those two are the only
places a downstream should read it from.

This project uses semantic versioning **for the language and the compiler CLI
together**. A major bump means source that compiled under the previous major
may no longer compile.

---

## 3.0.0 — the v0.4 language boundary

**Status: cut in-repo. Not tagged, not released, not published.** See
"Publishing" at the end of this entry.

### Why this is a major version

`VERSION` read `2.8.0` from `f1e30b3` (2026-06-21) until this change. The
entire v0.4 language break shipped under that one unchanged number, so `2.8.0`
names two mutually incompatible compilers: the one released on 2026-06-21, and
every commit since. Released 2.8.0 **cannot build current ooke at all** — it
rejects `store.tk:148` with a hard E2002 on expression-`if`.

The practical consequence was that no downstream could pin toke by semver.
ooke had to invent three keys — `mintoke`, `mintokedate` and `mintokecommit`
(`ooke.toml [requires]`, `toke-ooke/docs/versioning.md`) — to express a floor
that a version number should have carried. loke and every other consumer are
in the same position. 3.0.0 gives them a semver to pin instead of a commit
hash.

### Breaking changes since 2.8.0 (`f1e30b3`, 2026-06-21)

Each row names the commit that introduced the break, so a consumer can still
bisect if it needs to.

| # | Change | Introduced | Effect on 2.8.0-era source |
|---|---|---|---|
| 1 | `=` is assignment, `==` is equality (Epic 116.3 / A3) | `335113d`, 2026-07-01 | v0.3 source using `=` for equality changes meaning; 2.8.0 warns W2021 on every `==` |
| 2 | Expression-oriented `if` — `if`/`el` yields a value (Epic 116.1 / A1) | `3bc1dbb`, 2026-07-01 | 2.8.0 rejects expression-`if` with E2002; new source is not backward compatible |
| 3 | `@(...)` array and map literals; `[]` removed (Epic 116 syntax decision) | Epic 116 | `[]` literals no longer parse |
| 4 | Capability broker, deny-by-default (ADR-0010) | `8c073a6`, 2026-07-06 | `stdlib/capabilities.c` becomes a link dependency; binaries are subject to `CAP001` |
| 5 | **`tkc.toml [capabilities]` manifest parsed and baked (ADR-0010)** | `b66bd8e`, 2026-07-06 | **the hard floor.** Any earlier tkc aborts before parsing any toke: `tkc: tkc.toml:9: expected key = value` |
| 6 | Escape-by-default template surface (ADR-0011) | Epic 124.3 | stdlib HTML-escape semantics changed |
| 7 | `mut.` mutable bindings | Epic 116 | reassignment now requires a `mut` binding |
| 8 | No-underscore naming for config keys and stdlib call names (113.2a) | Epic 113 | Profile-1 excludes `_`; stdlib call names renamed |
| 9 | `std.fmt` added; lint rules v1 (12 rules, 6 of them the 131.9 pattern rules) | Epic 131.9 | new diagnostics on previously silent source |
| 10 | The Epic 127.x compiler fixes | throughout | several changed observable behaviour, not only failure modes |

**Earliest toke that builds current ooke: `b66bd8e` (2026-07-06)** — verified by
building tkc at that commit and running a full ooke build against it
(`toke-ooke/docs/versioning.md`). That commit is the semantic content of the
3.0.0 floor.

### Pinning guidance for downstreams

- Require `>= 3.0.0` for anything written in v0.4 syntax.
- `2.x` means pre-v0.4 and will not compile v0.4 source. There is no forward
  compatibility and none is planned.
- The commit/date fallbacks (`mintokedate`, `mintokecommit`) can be retired
  once 3.0.0 is tagged and published; until then they remain the only precise
  floor.

### Spec

Spec level is **v0.4** (`docs/spec/toke-spec-v0.4.md`). It does not change with
this version cut; 3.0.0 is the first compiler version number that names v0.4.

### Not changed

Historical measurement records that name `toke 2.8.0` as the compiler under
test are left alone on purpose — `patterns/catalogue.json`, `bench/patterns/`
results, `docs/metrics-baseline.md` measurement rows, `docs/spec/patterns-v0.4.md`
"Measured at" lines, `docs/reference/combinator-status.md` and the reviews under
`docs/about/`. They record what was measured and on what; rewriting them would
make them false. They are re-stamped only by a re-measurement.

### Publishing

**Nothing here is published.** Cutting the number in the repository is
reversible; tagging and publishing is not. Still outstanding, for the owner:

1. Rebuild `tkc` so the binary actually prints `toke 3.0.0` (the checked-in
   binary still reports 2.8.0 until then).
2. Tag `v3.0.0` — `.github/workflows/release.yml` derives the artefact version
   from `GITHUB_REF_NAME`, so the tag name *is* the release version.
3. Fill the real SHA256s into the homebrew formulae and correct their declared
   versions (story 132.27; `homebrew-toke/Formula/tkc.rb` still declares
   `0.3.0`, `ooke.rb` still declares `0.1.0`, and both carry
   `PLACEHOLDER_SHA256_*`). Do not publish the tap before both are real.
4. Bump ooke's `ooke.toml [requires] mintoke` to `3.0.0` and retire
   `mintokedate` / `mintokecommit`.

---

## 2.8.0 — 2026-06-21 (`f1e30b3`)

Stdlib interface fidelity (113.B.1 / .2 / .3). The last release before the v0.4
break. Pre-v0.4 syntax: `=` is equality, `[]` literals, statement-only `if`, no
capability manifest.

## 2.7.0 — 2026-04-30 (`30b132a`)

`sc` / `spawn` keywords; W8001 unsafe diagnostic.

## 2.6.0 — 2026-04-30 (`f7fe774`)

FFI ownership, closure runtime, `sc`/`spawn`, collections (`std.queue`,
`std.set`, `std.stack`).

## 2.5.0 — 2026-04-29 (`f7a9d40`)

`std.task`, array higher-order functions, `extern_c`, closure parser and name
resolution.

## 2.4.0 — 2026-04-29 (`56ea6b1`)

DWARF debug metadata; option type `$none`.

## 2.2.0 — 2026-04-29 (`f159394`)

`&name` function-reference syntax, replacing `f=name`.

## 2.1.0 — 2026-04-29 (`26eadd4`)

Function-reference syntax `f=name` in call arguments.
