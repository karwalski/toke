# PROJECT_STATUS.md
## toke — Live Project Status (high-level dashboard)

> **Source of truth:** `docs/progress.md` (detailed per-story tracker). This file is
> a **regenerated high-level view** — do not hand-edit it independently; update
> `docs/progress.md` and summarise here. (Governance: one authoritative tracker;
> dashboards are views, not parallel state.)

**Last updated:** 2026-07-03
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
