# CI Security Checks + Recurring Dependency/Secret Scanning

**Story:** 120.24 (Epic 120)
**Date:** 2026-07-02
**Depends on:** 120.22 (fuzz-target wiring), Epic 1.7 (nightly fuzz), Epic 3.7 (syft SBOM)

This document specifies three additions to the existing CI, matching the
conventions already in `Makefile` and `.github/workflows/`:

1. A **fast SAST + gitleaks secret pass** folded into `make ci`.
2. An **extension of the nightly fuzz job** with the 120.22 targets, run under
   ASAN/UBSAN.
3. A **recurring dependency-CVE recheck + SBOM refresh**.

It provides paste-ready `Makefile` target text and a paste-ready GitHub Actions
workflow (`.github/workflows/security-nightly.yml`, created new by this story).

> **Note:** This story does **not** edit `Makefile` or `progress.md`. The
> Makefile blocks below are specifications to be applied under a follow-up
> change; the committed `security-nightly.yml` references the `make` targets
> defined here and becomes fully green once those targets are merged.

---

## Current baseline (what already exists)

| Piece | Where | Notes |
|-------|-------|-------|
| `lint` | `Makefile:110` | `$(CC) $(CFLAGS) --analyze $(SRCS)` (clang static analyzer) |
| `ci` | `Makefile:154` | `ci: lint conform conform-check check-tki` |
| cppcheck / clang-tidy | `ci.yml` `lint` job | run in CI only, not in `make ci` |
| Fuzz targets | `Makefile:534-552` | `fuzz-lexer`, `fuzz-parser`, `fuzz-http-parse`, `fuzz-url-route`; `FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -g` |
| Nightly fuzz | `nightly.yml` | 60 s lexer + 60 s parser, daily 02:00 UTC |
| SBOM | `release.yml:113-123` | `syft packages file:./tkc -o spdx-json` on release only |
| gitleaks config | `.gitleaks.toml` | `[extend] useDefault = true`; allowlists fixtures/examples/vendor |
| Dependabot | `docs/security/dependency-tracking.md` | npm (toke-mcp) + pip (toke-model) only; **vendored C libs checked manually** |
| CVE process | `docs/security/cve-process.md` | manual, GitHub Security Advisories |

Gaps this story closes: no secret scan in `make ci`, no automated SAST gate
beyond `--analyze`, nightly fuzz does not cover the parser/runtime-WAF
surfaces the 120 audit flagged, and there is **no recurring CVE recheck or
between-release SBOM refresh** for vendored/linked C libraries.

---

## 1. Fast SAST + gitleaks secret pass in `make ci`

Two new targets, `sast` and `secrets-scan`, plus a `ci-security` aggregate.
`make ci` gains both so every push/PR gets a secret + static-analysis gate.
Both are fast (seconds): `sast` reuses the analyzer already in `lint` and adds
a scoped cppcheck pass; `secrets-scan` is a single gitleaks invocation against
the committed `.gitleaks.toml`.

### Paste-ready Makefile text

```makefile
# ── Story 120.24: fast SAST + secret scanning (folded into `make ci`) ─────

# Fast static analysis: clang analyzer (via lint) + cppcheck on sources only.
# Kept intentionally quick for the inner-loop `make ci`; the deep cppcheck
# --enable=all and clang-tidy passes stay in the ci.yml `lint` job.
sast: lint
	@command -v cppcheck >/dev/null 2>&1 && \
	  cppcheck --quiet \
	           --enable=warning,portability \
	           --error-exitcode=1 \
	           --inline-suppr \
	           --template='{file}:{line}:{column}: {severity}: {message} [{id}]' \
	           src/ \
	  || echo "cppcheck not installed — skipping (CI enforces it)"

# Secret scan of the whole tree using the committed .gitleaks.toml.
# --no-git scans the working tree (works in CI checkouts and locally);
# swap to `gitleaks git` in scheduled jobs to scan full history.
secrets-scan:
	@command -v gitleaks >/dev/null 2>&1 && \
	  gitleaks dir . \
	           --config .gitleaks.toml \
	           --redact \
	           --exit-code 1 \
	  || echo "gitleaks not installed — skipping (CI enforces it)"

# Aggregate security gate; also usable standalone as `make ci-security`.
ci-security: sast secrets-scan
```

Then extend the existing `ci` target (line 154) so the security gate runs on
every push/PR:

```makefile
# was: ci: lint conform conform-check check-tki
ci: lint sast secrets-scan conform conform-check check-tki
```

And add the new targets to the `.PHONY` line (Makefile:66):

```makefile
.PHONY: ... sast secrets-scan ci-security
```

### Wiring into `ci.yml` (existing `lint` job — one step to add)

Because these tools must be *enforced* (the Makefile targets skip when a tool
is absent so local runs never hard-fail), add a gitleaks step to the existing
`lint` job in `ci.yml`. cppcheck is already installed there.

```yaml
      - name: gitleaks secret scan
        uses: gitleaks/gitleaks-action@v2
        env:
          GITLEAKS_CONFIG: .gitleaks.toml
```

> Do **not** create a new workflow for item 1 — it belongs in the existing
> `ci.yml` `lint` job to keep the push/PR gate in one place.

---

## 2. Extend the nightly fuzz job with the 120.22 targets under ASAN/UBSAN

The 120 audit produced a prioritized fuzz-target list. `FUZZ_FLAGS` already
compiles with `-fsanitize=address,undefined,fuzzer`, so ASAN + UBSAN are on
for free — we only need harness targets and a nightly runner.

Targets to add (from `audit-120/parsers.md:156-172` and
`audit-120/runtime-waf.md:143`):

| Target | Entry points | Audit finding |
|--------|--------------|---------------|
| `fuzz-json` | `json_dec`, `json_str/keys/len/at/arr` | PAR-03, PAR-04 |
| `fuzz-yaml` | `yaml_from_json`, `yaml_to_json` | PAR-01 |
| `fuzz-toon` | `toon_from_json`, `toon_to_json`, `toon_arr` | PAR-02, PAR-06, PAR-07 |
| `fuzz-md`   | `md_render` (HTML/JS-injection corpus) | PAR-05, PAR-08 |
| `fuzz-xml`  | `xml_parse`, `xml_attr` | tag-stack `MAX_DEPTH` accounting |
| `fuzz-security` | `security_validate_uri`, `security_check_json_depth`, `security_check_sqli`, `security_check_xss` | runtime-WAF string-skip / pattern scan |

> `toml_load` is a thin FFI wrapper over vendored `tomlc99`; per the audit,
> fuzz the vendored parser upstream rather than adding a wrapper harness.

The harness `.c` files (one `LLVMFuzzerTestOneInput` each) are authored under
120.22 and live in `test/fuzz/` next to the existing ones. This story wires the
build/run targets and the nightly runner.

### Paste-ready Makefile text

```makefile
# ── Story 120.24 / 120.22: parser + runtime-WAF fuzz targets ──────────────
# Harness sources authored in 120.22 under test/fuzz/. FUZZ_FLAGS already
# enables ASAN+UBSAN (-fsanitize=address,undefined,fuzzer).

fuzz-json: test/fuzz/fuzz_json.c src/stdlib/json.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-json $^

fuzz-yaml: test/fuzz/fuzz_yaml.c src/stdlib/yaml.o src/stdlib/json.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-yaml $^

fuzz-toon: test/fuzz/fuzz_toon.c src/stdlib/toon.o src/stdlib/json.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-toon $^

fuzz-md: test/fuzz/fuzz_md.c src/stdlib/md.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-md $^ -lcmark

fuzz-xml: test/fuzz/fuzz_xml.c src/stdlib/xml.o src/stdlib/str.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-xml $^

fuzz-security: test/fuzz/fuzz_security.c src/stdlib/security.o src/stdlib/str.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-security $^

# Build all 120.22 targets.
fuzz-120-22: fuzz-json fuzz-yaml fuzz-toon fuzz-md fuzz-xml fuzz-security

# Build + run each for 120 s against its seed corpus, ASAN halting on the
# first fault. Crash inputs are written as crash-<sha> for CI upload.
fuzz-120-22-run: fuzz-120-22
	./fuzz-json     -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/json/
	./fuzz-yaml     -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/yaml/
	./fuzz-toon     -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/toon/
	./fuzz-md       -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/md/
	./fuzz-xml      -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/xml/
	./fuzz-security -max_total_time=120 -artifact_prefix=crash- test/fuzz/corpus/security/
```

Add to `.PHONY`:

```makefile
.PHONY: ... fuzz-json fuzz-yaml fuzz-toon fuzz-md fuzz-xml fuzz-security \
	fuzz-120-22 fuzz-120-22-run
```

The runner for these lives in the new `security-nightly.yml` (Section 4),
alongside the existing lexer/parser fuzzers so all fuzzing is scheduled from
one place. Leave `nightly.yml` untouched (do not edit existing workflows).

---

## 3. Recurring dependency-CVE recheck + SBOM refresh

Today the SBOM is generated only at release (`release.yml`), and vendored C
libs (`cmark`, `tomlc99`) are "checked manually before each Gate/release"
(`dependency-tracking.md`). This closes the between-release gap with a weekly
job that:

1. **Refreshes the SBOM** from a freshly built `tkc` using the same
   `syft ... -o spdx-json` command as `release.yml` (identical tool/flags so
   outputs are comparable across releases).
2. **Rechecks for CVEs** by running `grype` (anchore, same ecosystem as syft)
   against that SBOM, failing on High/Critical to match the
   `dependency-tracking.md` SLA (Critical: 48 h, High: 1 week).

This complements — does not replace — Dependabot (npm/pip in the other repos)
and the manual openssl-announce/NVD monitoring already documented.

### Paste-ready Makefile text

```makefile
# ── Story 120.24: SBOM refresh + dependency CVE recheck ───────────────────
# Same syft invocation as release.yml so between-release SBOMs are comparable.

sbom: $(BIN)
	syft packages file:./$(BIN) -o spdx-json > tkc-sbom.spdx.json
	sha256sum tkc-sbom.spdx.json > tkc-sbom.spdx.json.sha256
	@echo "Wrote tkc-sbom.spdx.json (+ .sha256)"

# Scan the refreshed SBOM for CVEs. Fail the job on High/Critical to honour
# the dependency-tracking.md SLA; Medium/Low are reported, not gated.
cve-scan: sbom
	grype sbom:tkc-sbom.spdx.json \
	      --fail-on high \
	      --output table
```

Add to `.PHONY`:

```makefile
.PHONY: ... sbom cve-scan
```

When `grype` reports a High/Critical, follow `cve-process.md`
(draft advisory → patch → release → CVE request) and record the outcome in
`dependency-tracking.md`.

---

## 4. New workflow: `.github/workflows/security-nightly.yml`

Created new by this story (existing workflows are **not** edited). It carries
two jobs on two cron entries:

- `fuzz-120-22` — nightly (03:00 UTC, offset from `nightly.yml`'s 02:00), runs
  the Section 2 targets under ASAN/UBSAN and uploads any crash artifacts.
- `deps-cve-sbom` — weekly (Monday 04:00 UTC), runs the Section 3 SBOM refresh
  + `grype` CVE recheck and uploads the refreshed SBOM. Gated by
  `if github.event.schedule` so each cron only fires its own job; both stay
  runnable on demand via `workflow_dispatch`.

```yaml
name: Security nightly

on:
  schedule:
    - cron: '0 3 * * *'    # 03:00 UTC daily — 120.22 fuzzing
    - cron: '0 4 * * 1'    # 04:00 UTC Monday — dependency CVE + SBOM refresh
  workflow_dispatch:

permissions:
  contents: read

jobs:
  fuzz-120-22:
    name: 120.22 parser + WAF fuzzing (ASAN/UBSAN)
    runs-on: ubuntu-latest
    # Runs on the nightly cron and on manual dispatch, not on the weekly cron.
    if: github.event.schedule != '0 4 * * 1'
    steps:
      - uses: actions/checkout@v4

      - name: Install toolchain
        run: sudo apt-get update && sudo apt-get install -y clang zlib1g-dev libcmark-dev

      - name: Build 120.22 fuzz targets
        run: make CC=clang fuzz-120-22

      - name: Run 120.22 fuzzers (120s each, ASAN/UBSAN)
        run: make fuzz-120-22-run

      - name: Upload crash artifacts
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: fuzz-120-22-crashes
          path: crash-*

  deps-cve-sbom:
    name: Dependency CVE recheck + SBOM refresh
    runs-on: ubuntu-latest
    # Runs on the weekly cron and on manual dispatch, not on the nightly cron.
    if: github.event.schedule != '0 3 * * *'
    steps:
      - uses: actions/checkout@v4

      - name: Install build deps
        run: sudo apt-get update && sudo apt-get install -y zlib1g-dev llvm-dev clang

      - name: Build tkc
        run: make

      - name: Install syft
        run: |
          curl -sSfL https://raw.githubusercontent.com/anchore/syft/main/install.sh \
            | sh -s -- -b /usr/local/bin

      - name: Install grype
        run: |
          curl -sSfL https://raw.githubusercontent.com/anchore/grype/main/install.sh \
            | sh -s -- -b /usr/local/bin

      - name: Refresh SBOM
        run: make sbom

      - name: CVE recheck (fail on High/Critical)
        run: make cve-scan

      - name: Upload refreshed SBOM
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: tkc-sbom-${{ github.sha }}
          path: |
            tkc-sbom.spdx.json
            tkc-sbom.spdx.json.sha256
```

---

## Rollout checklist

- [ ] Apply Section 1 Makefile text; change `ci:` line 154; update `.PHONY`.
- [ ] Add the gitleaks step to `ci.yml`'s existing `lint` job.
- [ ] Land 120.22 harnesses in `test/fuzz/` + seed corpora under `test/fuzz/corpus/{json,yaml,toon,md,xml,security}/`.
- [ ] Apply Section 2 + Section 3 Makefile text; update `.PHONY`.
- [ ] Commit `.github/workflows/security-nightly.yml` (this story creates it).
- [ ] Verify `make ci-security`, `make fuzz-120-22`, and `make cve-scan` locally.
- [ ] Record any grype findings in `dependency-tracking.md` per `cve-process.md`.
```
