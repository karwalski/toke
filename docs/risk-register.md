# docs/risk-register.md
## toke — Living Risk Register

**Last updated:** 2026-04-04
**Owner:** Matthew Watt
**Review cadence:** Monthly

---

## Summary

### Risk heat map

|              | **Negligible (1)** | **Minor (2)** | **Moderate (3)** | **Major (4)** | **Catastrophic (5)** |
|--------------|:------------------:|:-------------:|:-----------------:|:-------------:|:--------------------:|
| **Almost certain (5)** | | | | | |
| **Likely (4)** | | | | R005 | |
| **Possible (3)** | | | R009, R012, R015 | R001, R003, R008, R014 | |
| **Unlikely (2)** | | | R010, R013, R016 | R002, R004, R007 | R006 |
| **Rare (1)** | | | | R011 | |

### Top 5 risks

| Rank | ID | Risk | Score | Status |
|------|-----|------|-------|--------|
| 1 | R005 | Single-developer bus factor | 16 | accepted |
| 2 | R001 | Negative result: toke not more efficient than constrained decoding | 12 | open |
| 3 | R003 | Training data contamination invalidating benchmarks | 12 | mitigated |
| 4 | R008 | Benchmark tasks not representative of real-world usage | 12 | open |
| 5 | R014 | Spec/implementation divergence blocking Gate 2 | 12 | mitigated |

### Overall risk posture

**Moderate.** The project passed Gate 1 with a 12.5% token reduction and 63.7% Pass@1, providing early evidence that the core thesis holds. The highest residual risk is the single-developer bus factor, which is accepted given the project's research nature. Technical and research risks are actively managed through conformance testing, held-out evaluation sets, and incremental gating. Cloud cost exposure is bounded by an MLX-first hardware strategy with cloud as contingency only.

---

## Risk register

Risks are ordered by risk score (highest first), then by ID.

| ID | Category | Description | L | I | Score | Mitigations | Residual | Owner | Status |
|----|----------|-------------|---|---|-------|-------------|----------|-------|--------|
| R005 | Community | **Single-developer bus factor.** All design, implementation, research, and ops knowledge resides with one person. Loss of availability halts the project entirely. | 4 | 4 | 16 | [x] Comprehensive docs (spec, AGENTS.md, progress tracker) [x] All code in version control [x] CONTRIBUTING.md and DCO policy [x] Automated CI/CD reduces manual steps | Medium (12) | Matthew Watt | accepted |
| R001 | Research | **Negative result: toke does not reduce tokens vs constrained decoding.** Gate 2 may show that constrained decoding on standard languages achieves equivalent or better token efficiency, invalidating the thesis. | 3 | 4 | 12 | [x] Gate 1 showed 12.5% reduction (above 10% threshold) [x] Incremental gating allows early pivot [x] Falsification framing means negative result is still publishable | Medium (8) | Matthew Watt | open |
| R003 | Training | **Training data contamination invalidating benchmarks.** Held-out evaluation tasks could leak into training data through shared pipelines or accidental inclusion, making Pass@1 results unreliable. | 3 | 4 | 12 | [x] Held-out tasks gitignored [x] Separate generation scripts [x] Secret scanning CI (gitleaks) [x] Corpus pipeline threat model (Epic 1.8) | Low (6) | Matthew Watt | mitigated |
| R008 | Research | **Benchmark tasks not representative of real-world usage.** The 1000-task evaluation set may not reflect actual programming workloads, limiting external validity of results. | 3 | 4 | 12 | [x] 339 task templates across multiple categories [x] Differential test harness with 3 reference languages [x] Quality grades (A/B/C/D) in corpus [x] Plan to expand task diversity in Phase 2 | Medium (8) | Matthew Watt | open |
| R014 | Technical | **Spec/implementation divergence blocking Gate 2.** As the spec evolves in Phase 2, the compiler may drift from the formal grammar, causing silent correctness bugs. | 3 | 4 | 12 | [x] 62/62 conformance tests pass [x] EBNF grammar frozen at M0 [x] Spec review story (1.1.6) resolved 7 blocking issues [x] Phase 1 integration review (1.10.1) logged 6 TD items | Low (6) | Matthew Watt | mitigated |
| R002 | Technical | **Compiler bugs producing incorrect code silently.** Backend codegen stubs (cast, field-offset, nested-break) or type checker gaps could produce wrong output accepted by tests. | 2 | 4 | 8 | [x] Conformance suite (62 tests) [x] SAST (cppcheck + clang-tidy) [x] libFuzzer on lexer/parser with ASAN/UBSAN [x] Known stubs documented in progress notes | Medium (6) | Matthew Watt | open |
| R004 | Infrastructure | **Cloud costs exceeding budget for hosted MCP and training.** Gate 2-4 projected at $680-880; unexpected GPU hours or API calls could exceed this. | 2 | 4 | 8 | [x] Compute budget published (toke-spec/docs/compute-budget.md) [x] MLX-first strategy (Mac Studio M4 Max) [x] Cloud as contingency only [x] Cost table with per-gate projections | Low (4) | Matthew Watt | mitigated |
| R007 | Training | **Model fine-tuning not converging.** LoRA fine-tuning on the toke corpus may fail to produce a model that generates syntactically valid toke, especially with the constrained grammar. | 2 | 4 | 8 | [x] embed_tokens in modules_to_save (story 10.7.1) [x] Model card with evaluation contract (story 10.7.3) [x] Incremental checkpointing [x] Gate 1 Pass@1 at 63.7% shows feasibility | Medium (6) | Matthew Watt | open |
| R009 | Technical | **Spec too complex for community adoption.** 65 EBNF productions, 12 keywords, symbol disambiguation rules, and two profiles may be too much for new users to learn. | 3 | 3 | 9 | [x] Plain-language README (story 1.1.7) [x] Getting-started guide [x] Profile 1 as simplified entry point [x] Character set limited to 80 chars | Medium (6) | Matthew Watt | open |
| R012 | Legal/IP | **Dependency licensing conflicts.** C stdlib dependencies, Python tooling (pip packages), or LLM API terms could restrict distribution or academic publication. | 3 | 3 | 9 | [x] SBOM generation (syft, story 3.7.1) [x] Dependabot alerts enabled [x] pip-audit CI [x] Self-contained crypto (no OpenSSL dependency) | Low (4) | Matthew Watt | mitigated |
| R015 | Research | **Evaluation validity challenged by reviewers.** Methodology (task selection, token counting, model choice) may not withstand peer scrutiny from research review teams (T1-T8). | 3 | 3 | 9 | [x] 8 review teams lined up (T1-T8) [x] Gate criteria with audit-grade precision (story 10.10.2) [x] Reproducibility criteria defined (story 10.10.1) [x] 30-day reproducibility deadline | Medium (6) | Matthew Watt | open |
| R006 | Infrastructure | **Security vulnerability in sandbox allowing code execution escape.** Sandboxed execution of generated toke programs could be bypassed, allowing arbitrary code execution on build/eval hosts. | 2 | 5 | 10 | [x] sandbox-exec on macOS (story 1.8.2) [x] Docker fallback [x] 10s timeout [x] Clean environment [x] Threat model documented (story 1.8.1) [x] SECURITY.md and CVE process (Epic 2.6) | Low (4) | Matthew Watt | mitigated |
| R010 | Infrastructure | **CI/CD pipeline fragility.** 8 repos with independent CI workflows; a breaking change in one repo can cascade. GitHub Actions runner changes could break builds. | 2 | 3 | 6 | [x] Per-repo CI isolation [x] Pinned dependencies (pyproject.toml) [x] Reproducible builds (story 3.7.3) [x] Release signing (cosign keyless) | Low (3) | Matthew Watt | mitigated |
| R013 | Community | **Documentation quality insufficient for external contributors.** CONTRIBUTING.md, spec docs, and inline comments may not be enough for someone unfamiliar with the project. | 2 | 3 | 6 | [x] AGENTS.md with hard rules [x] Progress tracker with per-story detail [x] DCO policy [x] README with getting-started | Medium (4) | Matthew Watt | open |
| R016 | Legal/IP | **Academic integrity concerns with LLM-generated corpus.** Using LLM-generated code (Haiku, GPT-4.1-mini, Grok) as training data may raise originality or plagiarism questions in academic contexts. | 2 | 3 | 6 | [x] Multi-model generation (no single-source dependency) [x] Differential test harness validates correctness [x] Quality grading (A/B/C/D) [x] Corpus pipeline fully documented | Medium (4) | Matthew Watt | open |
| R011 | Technical | **LLVM IR backend portability issues.** Three targets via clang, but untested platforms or LLVM version changes could break codegen. | 1 | 4 | 4 | [x] Text IR output (portable) [x] 3 targets tested [x] E9002/E9003 diagnostics for backend errors | Low (3) | Matthew Watt | accepted |

---

## Glossary

- **L** — Likelihood (1 = rare, 2 = unlikely, 3 = possible, 4 = likely, 5 = almost certain)
- **I** — Impact (1 = negligible, 2 = minor, 3 = moderate, 4 = major, 5 = catastrophic)
- **Score** — L x I (range 1-25)
- **Residual** — Risk level after mitigations are applied (High / Medium / Low)

## Change log

| Date | Change |
|------|--------|
| 2026-04-04 | Initial register created with 16 risks. |
