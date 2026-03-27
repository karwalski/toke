# Contributing to tkc

## Before you start

- Read AGENTS.md — this is the operating specification for all development,
  including AI coding agents
- Read the language specification at github.com/karwalski/toke-spec
- Run `make conform` to confirm the baseline conformance suite is green
  on your machine before making any change

## Rules that are not negotiable

- Never change an existing error code number or meaning
- Never populate the `fix` field in a diagnostic with a fix that is
  incorrect for any valid program triggering that error
- Never add an external C library dependency to the compiler frontend
  (anything in `src/`) without raising an issue and getting explicit approval
- Every change to a compiler stage requires conformance tests
- `make conform` must pass at 100% before any PR is merged

## Branch naming

    feature/compiler-<description>
    fix/lexer-<description>
    test/typechecker-<description>
    refactor/parser-<description>

## Commit format

    feat(typechecker): add exhaustive match enforcement E4010
    fix(lexer): emit E1003 for out-of-set structural characters
    test(diagnostics): add D-series fix field tests for E4020

## Pull request checklist

- [ ] `make conform` passes 100%
- [ ] `make lint` produces zero warnings
- [ ] `make build-all` succeeds on x86-64 and ARM64
- [ ] New conformance tests added for any new error codes
- [ ] `docs/progress.md` updated
- [ ] No external dependencies added

## Developer Certificate of Origin

Sign your commits: `git commit -s`
