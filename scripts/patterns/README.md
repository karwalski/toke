# scripts/patterns — pattern-catalogue tooling (Epic 131)

Normative rules live in `docs/spec/patterns-protocol-v0.4.md`; the catalogue is `patterns/catalogue.json`,
fixtures are `patterns/<id>/<form>.tk` (or `.blocked.tk` for compiler-broken preferred forms).

| script | story | purpose |
|---|---|---|
| `validate_catalogue.py` | 131.1 | schema + verdict-consistency check (`--strict` once entries are measured) |
| `recheck_caveats.py` | 131.26 | re-check `bug_caveats` after a 127.x closure; `--stale` lists entries measured on an older tkc |

Tests: `python3 -m pytest scripts/patterns/tests -q` (stdlib + pytest only; tkc is mocked).

## recheck_caveats.py (131.26) — bug-close auto-recheck

Every catalogue entry may carry `bug_caveats: [{issue: "127.N", effect, preferred_when_fixed}]`. When a
127.x story is closed in `docs/progress.md` (status cell contains `DONE`, bold/dates tolerated), run:

```sh
python3 scripts/patterns/recheck_caveats.py                 # human table, all caveats
python3 scripts/patterns/recheck_caveats.py --issue 127.6   # only caveats naming that issue
python3 scripts/patterns/recheck_caveats.py --json          # machine-readable
python3 scripts/patterns/recheck_caveats.py --fail-on-action   # CI: exit 1 if anything needs unblocking
python3 scripts/patterns/recheck_caveats.py --stale [--json]   # entries whose measured_at.tkc_sha != git HEAD
```

For each caveat whose issue is closed it takes the `preferred_when_fixed` fixture (usually a `.blocked.tk`),
runs `tkc --check`, `tkc -O2 -o`, executes it with `PAT_N=1000` (`--pat-n` to change) and compares stdout
with the entry's canonical form at the same `PAT_N`. Per caveat it reports `issue`, `closed`,
`preferred_form`, `compiles`, `builds`, `runs`, `output_matches` and an `action`:

| action | meaning | follow-up |
|---|---|---|
| `unblock-and-remeasure` | fixture compiles, builds, runs and matches the canonical output | scoped story: rename `.blocked.tk` → `.tk`, re-run the 131.5 bench for that entry only, re-derive the verdict — never a full re-sweep |
| `still-broken` | issue closed but the fixture still fails (or prints a different checksum) | reopen / comment on the 127.x story with the `detail` column |
| `n/a` | issue still open, not found in progress.md, or `preferred_when_fixed` is null | nothing |

Exit status is always 0 (it is a report) except with `--fail-on-action`. `TKC` env or `--tkc` selects the
compiler (default `<repo>/tkc`); per-fixture timeout comes from `patterns/<id>/bench.json:timeout_s`
(default 30 s). Nothing is written to the catalogue or to `progress.md`.
