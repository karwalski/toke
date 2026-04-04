# Contributing to toke-spec

## How specification changes work

The toke specification uses a structured amendment process.

**For clarifications** (wording, examples, non-normative sections):
- Open an issue using the Clarification template
- Small clarifications may go directly to a PR with a clear rationale

**For normative changes** (grammar rules, type rules, error codes,
stdlib signatures):
- Open a Spec Amendment issue first
- Describe the problem the change solves
- Propose the change and its impact on existing implementations
- Allow 30 days for community comment before a PR is raised

**For new error codes:**
- Error codes are assigned sequentially within their series range
- Never reassign or renumber an existing code
- Every new code requires a prose description, an example, and
  a fix field specification if a mechanical fix is possible

## Pull request requirements

- Changes to `spec/grammar.ebnf` must include a parser generator
  validation showing zero ambiguity warnings
- Changes to `spec/errors.md` must include at least one example
  program that triggers the new or changed diagnostic
- Changes to `spec/stdlib-signatures.md` must be reflected in
  toke-stdlib and toke-benchmark in the same release

## Developer Certificate of Origin

All contributions require a DCO sign-off:
`Signed-off-by: Your Name <your@email.com>`

Add `-s` to your git commit command.
