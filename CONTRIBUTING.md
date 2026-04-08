# Contributing to toke

## Before you start

- Read AGENTS.md — this is the operating specification for all development,
  including AI coding agents
- Read the language specification at spec/ (or github.com/karwalski/toke)
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

## Developer Certificate of Origin (DCO)

All commits must carry a `Signed-off-by` trailer. This certifies that you
wrote the code and have the right to contribute it under the project licence.

Sign every commit with:

    git commit -s -m "type(scope): your message"

The DCO check is enforced automatically on every pull request. PRs with
unsigned commits will be blocked from merge. If you have existing unsigned
commits on a branch, you can remediate with:

    git rebase --signoff HEAD~<N>

The full Developer Certificate of Origin text is at https://developercertificate.org.

## Branch naming

    feature/compiler-<description>
    fix/lexer-<description>
    test/typechecker-<description>
    refactor/parser-<description>

## Commit format (Conventional Commits)

    type(scope): message

Examples:

    feat(typechecker): add exhaustive match enforcement E4010
    fix(lexer): emit E1003 for out-of-set structural characters
    test(diagnostics): add D-series fix field tests for E4020

Valid types: `feat`, `fix`, `test`, `refactor`, `docs`, `chore`, `ci`.

## Running the test suite

    make conform

The conformance suite must pass at 100%. A non-zero exit from `make conform`
blocks merge.

## Running SAST locally

Install cppcheck, then:

    cppcheck --enable=all src/

Any error-level finding must be resolved before opening a PR. Warning-level
findings require a documented rationale (inline suppression or issue).

To run clang-tidy locally:

    clang-tidy src/*.c -- -std=c99 -Wall -Wextra -Wpedantic -Werror -g

## Running the fuzzer (coming in 1.7.2)

    make fuzz

## Pull request checklist

- [ ] `make conform` passes 100%
- [ ] `make lint` produces zero warnings
- [ ] `make build-all` succeeds on x86-64 and ARM64
- [ ] `cppcheck --enable=all src/` produces zero errors
- [ ] New conformance tests added for any new error codes
- [ ] `docs/progress.md` updated
- [ ] No external dependencies added
- [ ] All commits signed off (`git commit -s`)
