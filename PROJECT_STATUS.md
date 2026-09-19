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
| Gate 1 | 8 | >10% token reduction AND Pass@1 ≥ 60% | **RE-OPENED** (recorded PASS 2026-04-03 on 12.5% reduction + 63.7% Pass@1; the Pass@1 was corrected to **58.8%** on 2026-09-19 — 588 of 1,000 generated, not 588/923, which had dropped non-compiling solutions from the denominator. 58.8% is below the ≥60% criterion. Verdict not re-decided; story 128.19) |
| Gate 2 | 14 | Extended features retain efficiency AND model beats baseline | **PASS on the *compile* criterion** (2026-05-22): 100% compile-Pass@1 on the curated hidden+eval set, cloud-trained Qwen 2.5 Coder 7B + QLoRA (37h, A10G). **Open weakness: functional correctness 55.6%** (272/489); a full-local re-audit (101.R1, v0.3.9) found 1093/1748 COMPILE_FAIL. The Gate-2 model was **v0.3-trained** — corpus/tokenizer/model need refresh for v0.4. See `docs/metrics-baseline.md`. |
| Gate 3 | 26 | Two+ model families ≥70% Pass@1 AND self-improvement loop | not reached |
| Gate 4 | 32 | All benchmarks met, spec complete, consortium proposal | not reached |

---

## Current focus (detail in `docs/progress.md`)

- **Epic 131 (in execution since 2026-09-18; 19/29 stories done + script halves of 4 more, 2026-09-19):** protocol + validator, 46-entry measured pattern catalogue (tokens measured; runtime pass running), 6 lint pattern rules + `--fix` rewrite mode (found the 129 lint gate was vacuous), judge/validate gates, corpus conformance sweep (rewrite backlog: 60 + 64 agent batches, 33 auto, 283 regen — far below the 200–400 estimate), freeze-129 snapshot + reopen, tokenizer Phase 0 baseline (shipped 8k tokenizer is +15.4% vs cl100k), curation script, must-merge list, provenance rule, `std.fmt`, tkc pinning. 20+ compiler bugs fixed under Epic 127 on the way; ~30 new stories filed (127.11–127.38, 131.30–131.44). Next: card v2 → auto wave → LLM waves → re-freeze `AUDIT_131.md`.
- **Epic 132 (filed 2026-09-18):** awareness & discoverability — canonical facts block as single source of truth, home/console copy fixed to honest numbers (55.6%), `/toke-programming-language` article + schema.org + llms.txt, registry descriptions aligned, Search Console baseline + 30/60/90 reviews. Gated on the August gate write-up, Fibonacci side-by-side and the toke-eval drift decision (132.0).
- **Epic 133 (filed 2026-09-18):** competitive review + standing benchmark vs KERN / KERN-py (third party publishes toke at 29/60 on "60 JSON-CLI pairs", −52% tokens; its 16K tokenizer trained on 25,953 programs = our v0.3 corpus size — provenance to check). 133.1 review is P0, no compute gate.
- **Epic 134 (filed 2026-09-19):** ooke uplift — it declares `mintoke 2.0.0` against a 2.8.0 compiler, still has `:void` returns, 19 of 30 files fail a standalone check, and the idiom pass (117.3) never ran. Runs through to fixing the unreproducible website docs build (132.20), relaunching the site on the uplifted ooke, and packaging a release ooke for loke/moke.
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
