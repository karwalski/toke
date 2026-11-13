# PROJECT_STATUS.md
## toke — Live Project Status (high-level dashboard)

> **Source of truth:** `docs/progress.md` (detailed per-story tracker). This file is
> a **regenerated high-level view** — do not hand-edit it independently; update
> `docs/progress.md` and summarise here. (Governance: one authoritative tracker;
> dashboards are views, not parallel state.) Every metric below is quoted from
> [`docs/metrics-baseline.md`](docs/metrics-baseline.md), which is the only source for
> a toke number.

**Last updated:** 2026-11-12, regenerated from `docs/progress.md` at `main` `f7d59452`
**Compiler:** toke 3.1.0 (`tkc --version`)
**Language version:** **v0.4** — [`docs/spec/toke-spec-v0.4.md`](docs/spec/toke-spec-v0.4.md)
is normative. Epic 116 shipped the breaking `=`/`==` split, expression-`if`/`match`,
`&&`/`||`, and a backtrack-free grammar; v1.0 RFC pending.
**Current phase (tracker header):** corpus rewrite and compiler hardening (Epics 127 and
131); the 2026-08-19 training-data freeze is reopened and will be re-declared by 131.27.
**Active milestone (tracker header):** Epic 127 to zero P0, then the 131 rewrite waves,
then `AUDIT_131`. Epic 127 has no open P0 row (table below).

**No v0.4-native model exists.** Every model number on this page, and in
`docs/metrics-baseline.md`, is from a v0.3-syntax model.

---

## Gate Status

| Gate | Criterion | Status |
|------|-----------|--------|
| Gate 1 | >10% token reduction AND Pass@1 ≥ 60% | **NOT PASSED** — re-opened 2026-09-19 and decided NOT PASSED by the owner on 2026-09-22 (story 128.19, [`docs/decisions/gate1-decision.md`](docs/decisions/gate1-decision.md)). The true Pass@1 is **58.8%** (588 of 1,000 generated; 923 compiled), below Gate 1's own 60% minimum. It was published as 63.7%, which was 588/923 and dropped non-compiling solutions from the denominator. The token-reduction criterion stands as met. |
| Gate 2 | Extended features retain efficiency AND model beats baseline | **ON HOLD — waiting for local compute.** The last model evaluation on record is still Gate 2 of 2026-05-22: **100% compile Pass@1 and 55.6% functional (272/489)** on the curated 500-hidden + 200-eval set, Qwen 2.5 Coder 7B + QLoRA, v0.3 syntax. The full-local re-audit (101.R1, v0.3.9) is the honest floor: **37.5% compile (655/1748), ~2.2% fully correct (38 PASS)**. The tracker records the Gate 2 milestone as superseded by 128.1's pre-registered protocol (BENCH-128), under which no Gate 2 number is a target; the Epic 128 training stories are `planned` and compute-gated, and GPU provisioning (128.4) is blocked on an owner decision ([`docs/owner-decisions-2026-10-01.md`](docs/owner-decisions-2026-10-01.md) §4). |
| Gate 3 | Two+ model families ≥70% Pass@1 AND self-improvement loop | not reached |
| Gate 4 | All benchmarks met, spec complete, consortium proposal | not reached |

Never quote the Gate 2 100% without the curated set it was measured on.

**Token efficiency, measured:** under one shared tokenizer (cl100k_base) toke costs
**1.34× [1.22, 1.48]** the tokens of equivalent Python on the 60 Gate-1 tasks (N = 60,
2026-09-19) — more, not fewer. Those 60 programs are hand-written (27 pure `--migrate`
output, 33 hand-repaired), not model output. On canonical v0.4 text the shipped 8K
tokenizer needs **15.4% more** tokens than cl100k_base (N = 2,000). See
`docs/metrics-baseline.md`.

---

## Open epics

Every epic in `docs/progress.md` with at least one story not `done` and not closed
(`superseded`, `cancelled`, `withdrawn` and the like). The table is rendered from the
tracker by `scripts/status/open_epics.py --write`, never by hand, and
`make check-open-epics` (part of `make ci`) fails when it no longer matches
`docs/progress.md`: refresh it in the same commit as a tracker edit. A story is counted
under the epic its ID names (rows for Epic 127 sit under several headings), once per
unique ID, from story tables only (the rows `scripts/check_progress.py` checks). "Open
P0/P1" counts only rows whose priority cell states one.

| Epic | Subject | Open | In progress | Review | Blocked | Planned | Backlog | Other open | Open P0 | Open P1 |
|---|---|---|---|---|---|---|---|---|---|---|
| 71 | MCP service, console and production deployment | 4 | 0 | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 81 | 1B purpose-built model | 4 | 0 | 0 | 0 | 0 | 4 | 0 | 0 | 0 |
| 99 | Publishing and distribution | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| 100 | Application requirements catalogue | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| 102 | From-scratch training architecture (v2) | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| 103 | Compiler stubs and incomplete implementations | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| 108 | Worker fleet management | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 |
| 116 | toke-for-LLM foundations | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 1 | 1 |
| 118 | Library corpus: v0.4 rebuild | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 1 |
| 121 | Security fix backlog (from Epic 120) | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 1 |
| 122 | Extended security hardening, PQC | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 1 |
| 127 | Compiler and diagnostic findings | 41 | 0 | 7 | 6 | 9 | 19 | 0 | 0 | 4 |
| 128 | Training strategy reset | 13 | 0 | 0 | 3 | 10 | 0 | 0 | 7 | 4 |
| 130 | Workspace audit and cleanup | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 1 |
| 131 | Token-efficient-by-default toke, corpus rewrite | 29 | 2 | 2 | 2 | 21 | 2 | 0 | 4 | 18 |
| 132 | Awareness and discoverability | 12 | 1 | 1 | 7 | 1 | 2 | 0 | 1 | 5 |
| 133 | Competitive review vs KERN / KERN-py | 3 | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 1 |
| 134 | ooke uplift and website relaunch | 2 | 0 | 0 | 2 | 0 | 0 | 0 | 0 | 0 |
| 135 | Document and image libraries | 2 | 0 | 1 | 0 | 0 | 1 | 0 | 0 | 0 |
| 136 | The bindings surface loke depends on | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |
| 138 | loke's 2026-09-27 handoff | 9 | 0 | 0 | 4 | 2 | 3 | 0 | 2 | 1 |
| 139 | toke-ocr | 3 | 0 | 0 | 1 | 2 | 0 | 0 | 0 | 2 |
| **Total** | | **135** | **3** | **11** | **43** | **47** | **31** | **0** | **15** | **40** |

"Other open" is the status `open`, which the tracker's status list does not define;
`make check-progress` rejects it (132.139), and the two Epic 128 rows that carry it are
baselined there until the tracker gives them a defined status.

---

## Current focus (detail in `docs/progress.md`)

- **Epic 131 — corpus rewrite.** The largest open epic. Its rewrite waves and the
  `AUDIT_131` re-freeze are the active milestone; most of what remains is `planned`.
- **Epic 127 — compiler and diagnostic findings.** No open P0. Recently closed on
  `main`: router `:param`/`*` dispatch (127.343), SIGPIPE guards on the plain-HTTP and
  WebSocket clients (127.341, 127.347). The Linux HTTPS client guard (127.346) is at
  `review`, waiting for a Linux CI run. The S1 sealed-strings conversion (127.295) is
  `planned` and proceeds in tranches.
- **Recently landed on `main`:** `tkc pkg` package commands, the resolver in the import
  path and git-based fetch (76.1.3b–d, ADR-0004); `tkc --python-view` (96.7);
  `--from-tkir` / `--read-tkir` and `.tkir` 1.2 (76.1.6c); an explicit hybrid
  post-quantum key-exchange group list on every `std.http` and `std.llm` TLS context
  (122.15). The PQC migration roadmap (122.5) stays blocked.
- **Next on the compiler:** the compact `[byte]` ABI (135.18), still `planned` on
  `main`.
- **Epic 128 — training reset.** Compute-gated; holds the most open P0 rows.
- **Epic 132 — awareness and discoverability.** Mostly done; what remains is largely
  blocked on owner actions (registry publishing, the site deploy, the Homebrew tap).

---

## Honest-metrics rule

External / efficiency / academic claims MUST use the numbers + methodology in
`docs/metrics-baseline.md` (the `--min` token basis, functional-correctness %, and
the v0.3-era caveat) — **not** the headline "Gate 2 PASS 100%", which is compile-only
on a curated set.

---

## Governance & top risk

BDFL (single maintainer) until spec 1.0 + a second conformant compiler, or a 2+ org
consortium adopts the spec (→ TSC). **#1 accepted risk: single-developer bus factor
(R005).** See `docs/governance.md`, `docs/risk-register.md`.
