# PROJECT_STATUS.md
## toke — Live Project Status (high-level dashboard)

> **Source of truth:** `docs/progress.md` (detailed per-story tracker). This file is
> a **regenerated high-level view** — do not hand-edit it independently; update
> `docs/progress.md` and summarise here. (Governance: one authoritative tracker;
> dashboards are views, not parallel state.)

**Last updated:** 2026-09-18
**Current phase:** v0.4 language foundation + pre-from-scratch-training hardening
**Current milestone:** M3 — post-Gate-2 functional-correctness sprint; week-12 GO/NO-GO mid-August
**Language version:** **v0.4** — Epic 116 shipped the breaking `=`/`==` split, expression-`if`/`match`, `&&`/`||`, and a backtrack-free grammar. The earlier "v0.3 LOCKED / no breaking changes" freeze was superseded; v1.0 RFC pending.

---

## Gate Status

| Gate | Month | Criterion | Status |
|------|-------|-----------|--------|
| Gate 1 | 8 | >10% token reduction AND Pass@1 ≥ 60% | **PASS** (2026-04-03: 12.5% reduction, 63.7% Pass@1) |
| Gate 2 | 14 | Extended features retain efficiency AND model beats baseline | **PASS on the *compile* criterion** (2026-05-22): 100% compile-Pass@1 on the curated hidden+eval set, cloud-trained Qwen 2.5 Coder 7B + QLoRA (37h, A10G). **Open weakness: functional correctness 55.6%** (272/489); a full-local re-audit (101.R1, v0.3.9) found 1093/1748 COMPILE_FAIL. The Gate-2 model was **v0.3-trained** — corpus/tokenizer/model need refresh for v0.4. See `docs/metrics-baseline.md`. |
| Gate 3 | 26 | Two+ model families ≥70% Pass@1 AND self-improvement loop | not reached |
| Gate 4 | 32 | All benchmarks met, spec complete, consortium proposal | not reached |

---

## Current focus (detail in `docs/progress.md`)

- **Epic 131 (filed 2026-09-18, next up):** token-efficient-by-default patterns — measured pattern catalogue (token proxy + runtime micro-bench, perf a co-equal hard gate), `--lint` pattern rules → judge → validate gate → syntax card v2, corpus + library rewrite waves (reopens the 2026-08-19 freeze with a snapshot fallback; re-freeze as `AUDIT_131.md`), tokenizer Phase 0–1 absorbed. Supersedes 129.8, 126.2/126.4, 118.3, 118.7-ingest.
- **Epic 132 (filed 2026-09-18):** awareness & discoverability — canonical facts block as single source of truth, home/console copy fixed to honest numbers (55.6%), `/toke-programming-language` article + schema.org + llms.txt, registry descriptions aligned, Search Console baseline + 30/60/90 reviews. Gated on the August gate write-up, Fibonacci side-by-side and the toke-eval drift decision (132.0).
- **Epic 133 (filed 2026-09-18):** competitive review + standing benchmark vs KERN / KERN-py (third party publishes toke at 29/60 on "60 JSON-CLI pairs", −52% tokens; its 16K tokenizer trained on 25,953 programs = our v0.3 corpus size — provenance to check). 133.1 review is P0, no compute gate.
- **Runnable now (laptop):** Epic 123 (foundation, root-of-trust, quality — in progress); Epic 119 (docs compile-health — done, gate green); Epics 120–122 (security audit done; remediation + ADR ratification pending); v0.4 uplift 116.13 / 117 (ooke migrated) / 118 (corpus 84% compile).
- **Compute-gated (~Oct local hardware):** tokenizer retrain (116.9), from-scratch training (116.12), corpus regeneration (116.8), idiom re-measure (118.3/118.4), website deploy (117.6 — also owner-approval-gated).
- **Awaiting owner decision:** ADR-0010 (ambient authority), ADR-0011 (injection/auto-escape), ADR-0012 (spatial safety), ADR-0013 (crypto agility); generics investigation (ADR-0014).

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
