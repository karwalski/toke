# T-Series: Type Checking Cases

Conformance cases for the type checker (Stage 4), in the YAML format of
`test/diagnostics/`. `test/run_conform.sh` finds every `*.yaml` under `test/`,
so these run in `make conform` (and `make conform-check`); run the series alone
with `SERIES=T bash test/run_conform.sh`. Populated by story 123.6 — this
directory held only this README from Epic 1 until 2026-09-25.

Two kinds of case:

- **valid** (`expected_exit_code: 0`, no codes): the program must check clean.
  A false diagnostic on a correct program is worse than a missing one.
- **invalid** (`expected_exit_code: 1`, one code): the program must be
  rejected with that code. When these cases were written each one was also
  confirmed to emit exactly that one error and nothing else; the runner itself
  only checks that the code is present.

Coverage: casts between widths, error unions and `!` (E3020), structs over 16
fields (127.190), typed-empty arrays (127.164), interpolation, sum-type
matches (E4010), receiver members (E4026/E4035/E4036), immutability (E4070),
map literals (E4043), the missing-`<` tail (E4071), composite interpolation
(E4032), type-as-value (E4033), variant ambiguity (E4072), nested
interpolation (E4073), and the E4031 family (operands, arguments, returns,
annotations, element stores).

Rules the checker does not yet enforce, although the spec states them, are
NOT encoded here as passing programs — they are defects, reported to the
tracker instead (see story 123.6's result).
