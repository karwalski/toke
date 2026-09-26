# C-Series: Code Generation Cases

Full programs that are compiled, run, and checked against a pinned expected
result. They test what the emitted code DOES, which `--check` cannot see.
Populated by story 123.6 (this directory held only this README from Epic 1
until 2026-09-25).

## Two gates read this directory

| Gate | What it pins | Build |
|---|---|---|
| `make test-codegen` (`run_codegen.sh`) | exact stdout (`<name>.expected`), exit code, and the trap line on stderr | `-O0` and `-O2` |
| `make diff-codegen` | a hash of stdout and the exit code, against `test/codegen-baseline.json` | default (`-O2`) |

`diff-codegen` catches any change of behaviour; `test-codegen` says what the
behaviour must be. `diff-codegen` cannot tell RT003 from RT004 (both exit 1 with
empty stdout, and it discards stderr) and never builds at `-O0`, which is where
an optimiser-dependent result shows up.

## Case format

- `<name>.tk` + `<name>.expected` (exact stdout).
- Header comment lines `expect-exit: N` (default 0) and
  `expect-stderr: TEXT` (some stderr line must start with TEXT).
- Programs check themselves too: a wrong value prints a line starting `FAIL`,
  which `diff-codegen` treats as unhealthy. Every expected value is derived by
  hand, never copied from the compiler's output.
- `multimod/<case>/` — a multi-module program; all `*.tkm` files are passed to
  one `tkc` call in sorted order, expected stdout is `expected`, header lines go
  in `main.tkm`. They are `.tkm` so that `diff-codegen`, which builds one `.tk`
  at a time, does not pick them up.

## Known defects

A case listed in `test/codegen-quarantine.txt` is a witness for a filed defect.
Its expected output is the CORRECT one. It reports `XFAIL` while the defect
stands and fails the run when it starts passing, so the quarantine line has to
be removed with the fix.

## Coverage

| Area | Cases |
|---|---|
| RT003 bounds (ADR-0012 a) | `cg_trap_bounds_{high,neg,loop,struct}`, `cg_trap_set_oob` (127.217) |
| RT004 division (ADR-0012 b) | `cg_trap_div`, `cg_trap_rem`, `cg_trap_div_intmin` (121.36, fixed 127.217) |
| RT005 nil (ADR-0012 d) | `cg_trap_nil` |
| error unions, `!`, let-bound / mut-assigned | `cg_err_union`, `cg_err_letbound_payload` (127.103, fixed da7be68) |
| interpolation of scalars | `cg_interp_scalars`, `cg_unsigned_as_str`, `cg_interp_byte` (127.217), `cg_interp_bool_shapes` (127.194, fixed by 127.52), witness `cg_unsigned` |
| `j.print` of a str at the i64 ABI (127.215) | `cg_json_print_str` |
| maps | `cg_map` |
| arrays, in-place vs copying | `cg_array_paths` |
| typed-empty arrays (127.164) | `cg_typed_empty` |
| structs over 16 fields (127.190) | `cg_wide_struct` |
| receiver dispatch | `cg_recv_dispatch`, witness `cg_recv_field_receiver` |
| multi-module (127.76) | `multimod/geom`, `multimod/boolfn` (127.217) |
