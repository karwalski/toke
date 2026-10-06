# Contributing to the toke standard library

## Rules

- Every stdlib function signature is normative. Do not change a signature
  without first raising a spec amendment (see `docs/spec/stdlib-signatures.md`).
- A module is exactly three gated surfaces: its interface `stdlib/<m>.tki`
  (`make check-tki`, `make check-tki-names`), its implementation in
  `src/stdlib/` (C, built by `make`), and its documentation
  `docs/stdlib/<m>.md` (`make check-docs`). Nothing else describes a module:
  `make check-parallel-stdlib` rejects a `stdlib/<m>.tk` or a `stdlib/<m>.md`
  beside them (135.11, 135.14).
- C glue calls must be commented with the toke signature they implement.

## Adding a new module or function

1. Amend `docs/spec/stdlib-signatures.md` with the normative interface, and
   wait for the amendment to be accepted.
2. Declare it in `stdlib/<m>.tki` and implement the glue in `src/stdlib/`.
3. Document it in `docs/stdlib/<m>.md`.
4. Regenerate the derived files: `python3 scripts/gen_stdlib_decls.py` and
   `make render-stdlib-internal`.
5. Add a conformance test under `test/conform/`.

## Licence note

The stdlib is MIT licensed so that compiled toke programs carry no
licence obligation. Keep it that way — do not add code under a more
restrictive licence.
