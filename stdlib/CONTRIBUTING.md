# Contributing to the toke standard library

## Rules

- Every stdlib function signature is normative. Do not change a signature
  without first raising a spec amendment issue (see spec/).
- All .tk source files must compile cleanly with `tkc --check` before commit.
- Each module file has a companion .md documentation file. Keep them in sync.
- Stdlib implementations may use C FFI internally. FFI calls must be
  commented with the C signature and a rationale.

## Adding a new module

1. Raise a spec amendment issue defining the module's
   normative interface (see spec/)
2. Wait for the spec amendment to be accepted
3. Implement the module in `std/<name>.tk`
4. Write the documentation in `std/<name>.md`
5. Add a CI test that compiles the module with `tkc --check`

## Licence note

The stdlib is MIT licensed so that compiled toke programs carry no
licence obligation. Keep it that way — do not add code under a more
restrictive licence.
