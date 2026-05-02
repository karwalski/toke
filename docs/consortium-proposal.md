# Consortium Proposal: toke Language Standardisation and Enterprise Adoption

**Version:** 0.1 (Draft)
**Date:** Post Gate 1
**Author:** Matt Watt ([@karwalski](https://github.com/karwalski))
**Status:** Story 4.5.2 — Draft for review

---

## 1. Executive Summary

toke is a compiled programming language designed as a code generation target for large language models (LLMs). It uses a 55-character alphabet, 13 keywords, and a strict LL(1) grammar to produce programs that are shorter, cheaper, and more likely to compile correctly on the first pass than equivalent programs in Python, C, or Java.

The toke project has completed its first validation gate. At Gate 1, the language demonstrated 12.5% token reduction versus GPT-4's cl100k_base tokenizer and 63.7% first-pass compilation accuracy on 1,000 held-out tasks using a fine-tuned 7B model. These results exceeded the pre-registered go/no-go thresholds.

This document proposes the formation of a **toke Language Consortium** to govern the language specification, steward its evolution toward version 1.0, establish a conformance certification program, and coordinate enterprise adoption. The consortium provides a neutral home for the specification and tooling, ensures no single organisation controls the language's future, and gives adopters confidence that their investment in toke integration is protected by transparent governance and compatibility guarantees.

---

## 2. The Opportunity

### 2.1 Market Context

Code generation is the single largest use case for large language models. Every major AI company -- OpenAI, Anthropic, Google, Meta, Mistral -- ships or integrates coding assistants, and enterprise adoption of AI-assisted development is accelerating. The market for AI code generation tools is projected to grow substantially through 2030 as organisations embed LLM-driven automation into their development pipelines.

Every token an LLM generates costs compute time and API dollars. At scale -- thousands or millions of generation requests per day -- the cost of verbose target languages compounds into a material line item.

### 2.2 The Token Efficiency Value Proposition

Current target languages (Python, TypeScript, Go, Java) were designed for human authors. They carry structural overhead that LLMs pay for on every generation pass: verbose keywords, optional syntax, whitespace significance, ambiguous grammar, and human-readable error messages that models must parse as English prose.

toke eliminates this overhead at the language level:

- **40--75% fewer tokens** than equivalent Python, C, or Java programs when documentation standards are applied.
- **63.7% first-pass compilation accuracy** (Pass@1) measured at Gate 1 -- reducing the costly generate-compile-repair loop.
- **Structured JSON diagnostics** with stable error codes enable mechanical repair without parsing English prose.
- **No comment syntax** -- documentation lives outside source files, so token cost is fixed regardless of coding standards.

### 2.3 Enterprise Pain Points

Organisations building AI code generation pipelines face three recurring problems:

1. **Cost at scale.** Token-per-request costs multiply across high-volume generation pipelines. A language that produces equivalent programs in fewer tokens directly reduces this cost.
2. **Reliability.** First-pass compilation failures trigger expensive retry loops. A deterministic grammar with one valid interpretation at every syntactic position reduces the failure rate.
3. **Vendor risk.** Adopting a language controlled by a single individual or company creates dependency risk. A consortium with transparent governance, open specification, and multiple stakeholders mitigates this.

---

## 3. What the Consortium Provides

### 3.1 Governance

The toke project has a published governance document (see [governance.md](governance.md)) that defines the current BDFL model and the transition path to a Technical Steering Committee (TSC). The consortium formalises this transition:

- The TSC shall have 3--7 members drawn from founding consortium members.
- Decisions follow the existing process: Architecture Decision Records (ADRs) for design changes, gate reviews for milestones, and lazy consensus (72-hour window) for day-to-day decisions.
- The BDFL model continues until the specification reaches 1.0 and a second independent implementation passes the conformance suite, at which point governance transfers to the TSC.

### 3.2 Specification Stewardship

The consortium owns and maintains the toke language specification. Responsibilities include:

- **Versioning.** Semantic versioning (MAJOR.MINOR.PATCH). The specification is currently at v0.2 (draft). Version 1.0 requires: conformance suite passing for two independent implementations, ecosystem proof complete, all deferred sections resolved.
- **Compatibility guarantees.** After 1.0: source syntax stable across minor versions. Error codes never reused. Deprecation cycles of at least two minor releases or six months. Migration guides for major-version breaking changes.
- **Change process.** Specification changes require an ADR, review by the TSC, and voting by contributing members.

### 3.3 Conformance Certification Program

The consortium establishes and operates a conformance certification program for toke implementations:

- A conformance test suite (currently 600+ tests, targeting comprehensive coverage of the specification).
- Certified implementations may use the "toke Certified" mark.
- Annual recertification against the latest specification version.
- Public registry of certified implementations.

### 3.4 Reference Implementation Maintenance

The consortium funds and coordinates maintenance of the reference compiler (`tkc`) and standard library (38 modules with C runtime backing). This includes:

- Bug fixes and security patches.
- New specification feature implementation.
- Cross-platform support (currently x86-64 Linux, ARM64 Linux, ARM64 macOS).
- SAST scanning (cppcheck, clang-tidy) and fuzz testing.

### 3.5 Training Corpus and Model Sharing

The consortium maintains and expands the validated training corpus:

- Currently 46,754 validated programs generated across 4 stages with differential testing against 3 models.
- Compiler-in-the-loop validation ensures every program in the corpus compiles and produces correct output.
- Shared corpus enables member organisations to fine-tune their own models without duplicating the expensive generation and validation pipeline.
- Purpose-built BPE tokenizer (vocabulary size 8K, 70.2% utilisation at Gate 1) shared across members.

### 3.6 Enterprise Support Tiers

The consortium coordinates (but does not exclusively provide) enterprise support:

- **Community tier.** GitHub Issues and Discussions. Best-effort response.
- **Standard tier.** Guaranteed triage within 48 hours. Access to consortium mailing lists and working groups.
- **Premium tier.** Dedicated support contact. Priority bug fixes. Pre-release access to specification drafts. Direct channel to TSC members.

Support delivery may be provided by the consortium directly, by founding members, or by certified partners.

---

## 4. Membership Structure

### 4.1 Founding Members

- **Seats on the Technical Steering Committee** (one seat per founding member, up to the TSC maximum of seven).
- Binding vote on specification changes, governance amendments, and consortium budget.
- Logo placement on consortium materials and the toke website.
- Early access to specification drafts and roadmap decisions.
- Founding member status is available only during the formation phase (Phase 1).

### 4.2 Contributing Members

- **Voting rights on specification changes** (non-binding advisory vote during BDFL phase; binding vote after TSC formation).
- Participation in working groups (specification, tooling, training, conformance).
- Access to shared training corpus and pre-release tooling.
- Listed on the consortium member directory.

### 4.3 Adopter Members

- **Conformance certification** for their toke implementations or integrations.
- Use of the "toke Certified" and consortium member marks.
- Access to the conformance test suite and certification process.
- Listed on the consortium member directory.

### 4.4 Individual Contributors

- Open participation in specification discussions and code contributions.
- No membership fee. Subject to the existing contribution process (Apache 2.0 licence, DCO sign-off, CONTRIBUTING.md rules).
- Individual contributors do not hold consortium voting rights but may be nominated to working groups.

---

## 5. Technical Deliverables

The consortium stewards the following technical assets, all of which exist today in various stages of maturity:

### 5.1 Language Specification

The toke specification (currently v0.2, draft) is a comprehensive document covering 30 sections: character set, lexical rules, formal EBNF grammar (65 productions), type system, error model, memory model, module structure, standard library interface, compiler requirements, structured diagnostics specification, tooling protocol, conformance criteria, and security requirements.

**Path to 1.0:** Resolve all deferred items. Achieve conformance suite passage by two independent implementations. Complete ecosystem proof. Estimated timeline: 12--18 months after consortium formation, subject to Gate 2 completion.

### 5.2 Reference Compiler (tkc)

A C99 compiler with an LLVM backend producing native binaries for x86-64 and ARM64. Single self-contained binary with no runtime dependencies. Passes the conformance suite at 100%. SAST-scanned and fuzz-tested.

### 5.3 Conformance Test Suite

600+ tests covering lexical analysis, parsing, type checking, code generation, error handling, and standard library behaviour. The conformance suite is the normative definition of language behaviour where the specification is ambiguous.

### 5.4 Training Corpus

46,754 validated programs generated through a 4-stage compiler-in-the-loop pipeline with differential testing against 3 models (GPT-4o, Claude 3.5 Sonnet, Qwen 2.5 Coder 7B). Every program compiles, runs, and produces verified output.

### 5.5 Purpose-Built Tokenizer

BPE tokenizer trained on the toke corpus. 8K vocabulary, 70.2% utilisation, fertility 0.374 at Gate 1. Retrain on default (56-character) syntax corpus is planned for Gate 2.

### 5.6 MCP Server for IDE Integration

Model Context Protocol (MCP) server providing toke language support to IDE extensions and AI coding assistants. Published at [toke-mcp](https://github.com/karwalski/toke-mcp).

### 5.7 Additional Tooling

- **Tree-sitter grammar** for editor syntax highlighting.
- **WebAssembly playground** ([toke-ooke](https://github.com/karwalski/toke-ooke)) for interactive experimentation.
- **Evaluation harness** ([toke-eval](https://github.com/karwalski/toke-eval)) for benchmarking model performance on toke generation tasks.

---

## 6. Intellectual Property

### 6.1 Licensing

All toke project source code is released under the **Apache 2.0 licence**. This permits commercial use, modification, distribution, and embedding in proprietary products, with standard attribution obligations.

The specification document itself is available under a permissive licence allowing free use, reproduction, and derivative works with attribution.

### 6.2 Patent Non-Assertion Covenant

Consortium members agree to a mutual patent non-assertion covenant: no member shall assert patent claims against any implementation of the toke specification, whether by another member or by a non-member. This covenant is irrevocable and survives membership termination.

### 6.3 Trademark Management

The consortium manages the "toke" wordmark and associated marks ("toke Certified", the toke logo). Usage guidelines:

- "toke" is always lowercase.
- Third parties may refer to the language but shall not imply official consortium endorsement.
- Derivative compilers shall not be named "tkc" (reserved for the reference implementation).
- The "toke Certified" mark requires passing the conformance certification program.

---

## 7. Revenue Model

The consortium operates as a non-profit or fiscally sponsored entity. Revenue sources:

### 7.1 Membership Dues (Tiered)

| Tier | Annual Dues | Benefits |
|------|-------------|----------|
| Founding Member | To be determined during formation | TSC seat, binding vote, logo placement, early access |
| Contributing Member | Scaled by organisation size | Voting rights, working group participation, corpus access |
| Adopter Member | Scaled by organisation size | Certification, mark usage, test suite access |
| Individual Contributor | Free | Participation in discussions and code contributions |

Specific fee amounts will be set during the formation phase based on the number and size of founding members. The guiding principle: fees should cover consortium operations (specification work, infrastructure, certification program) without creating a barrier to participation.

### 7.2 Conformance Certification Fees

Per-implementation certification fees cover the cost of test suite maintenance, certification review, and mark administration. Certification is optional -- the test suite itself is open source and freely available for self-assessment.

### 7.3 Training and Support Services

Revenue from premium support tiers, training workshops, and consulting engagements. These may be delivered by the consortium or by certified partners under a revenue-sharing arrangement.

### 7.4 No Licence Fees

toke is and will remain open source under Apache 2.0. There are no licence fees, royalties, or per-seat charges for using the language, compiler, or standard library. This is a foundational principle, not a business decision subject to change.

---

## 8. Timeline

### Phase 1: Formation (Months 0--6)

- Draft and ratify consortium charter.
- Recruit founding members (target: 3--5 organisations).
- Establish legal entity or fiscal sponsorship arrangement.
- Formalise IP assignments and patent non-assertion covenant.
- Appoint initial Technical Steering Committee.
- Publish consortium website and member directory.

### Phase 2: Specification 1.0 (Months 6--24)

- Complete Gate 2 (default syntax implementation, model retraining on local compute).
- Resolve all deferred specification items.
- Support development of a second independent toke implementation.
- Expand conformance test suite to comprehensive specification coverage.
- Retrain purpose-built tokenizer on default syntax corpus.
- Publish specification 1.0 release candidate.

### Phase 3: Enterprise Adoption (Months 18--36)

- Launch conformance certification program.
- Establish enterprise support tiers with certified partners.
- Publish integration guides for major AI coding platforms.
- Expand training corpus to 100,000+ validated programs.
- Begin regular specification release cadence (minor quarterly, major as accumulated).
- Conduct independent benchmark studies with member organisations.

---

## 9. Current Project Status -- Honest Assessment

This proposal is aspirational but grounded. Transparency about where the project stands today:

**What exists:**
- A working compiler that passes 600+ conformance tests at 100%.
- A formal specification at v0.2 (draft) covering 30 sections.
- A validated training corpus of 46,754 programs.
- Gate 1 results (12.5% token reduction, 63.7% Pass@1) that exceeded pre-registered thresholds.
- Published governance, contribution, security, and licensing documents.
- 38 standard library modules with C runtime implementations.
- An MCP server, tree-sitter grammar, web playground, and evaluation harness.

**What does not yet exist:**
- A second independent implementation of the language (required for spec 1.0).
- A purpose-built tokenizer retrained on the default (56-character) syntax.
- Gate 2 completion (on hold, waiting for local compute hardware).
- Any enterprise adopter or production deployment.
- A package ecosystem or third-party library registry.

**What this means for consortium members:**
Founding members are joining early. The specification will change. The language is pre-1.0. The value of early membership is influence over the specification's direction, access to shared training infrastructure, and a seat at the table as the governance model transitions from BDFL to TSC. The risk is that the project does not reach 1.0 -- mitigated by the go/no-go gate system, which forces the project to pivot or stop if metrics do not improve.

---

## 10. Call to Action

The toke Language Consortium is seeking founding members from the following categories:

**AI companies** building code generation products or infrastructure. If your models generate code, toke reduces the cost of every generation call. Founding membership gives you influence over the language design and access to shared training data.

**Cloud and infrastructure providers** serving AI workloads. If your customers run code generation pipelines on your platform, a more token-efficient target language reduces their compute costs -- and yours.

**Enterprise software companies** integrating AI-assisted development. If your products embed code generation (internal tooling, low-code platforms, automation), toke provides a compilation target optimised for the use case.

**Research institutions** studying code generation, programming language design, or LLM efficiency. The toke project provides a controlled experimental surface -- language, tokenizer, corpus, and evaluation harness co-designed for measurement.

### How to Express Interest

1. Review the project: [github.com/karwalski/toke](https://github.com/karwalski/toke)
2. Read the specification: [toke-spec-v02.md](spec/toke-spec-v02.md)
3. Run the conformance suite: `make conform`
4. Contact: matt@tokelang.dev

We are looking for organisations willing to invest in the formation phase -- not organisations waiting for a finished product. The consortium exists to build that product together.

---

## Appendix A: Related Documents

| Document | Description |
|----------|-------------|
| [governance.md](governance.md) | Project governance model, decision-making, versioning policy |
| [toke-spec-v02.md](spec/toke-spec-v02.md) | Language specification (v0.2, draft) |
| [enterprise.md](about/enterprise.md) | Enterprise value proposition and integration guide |
| [why.md](about/why.md) | Project thesis and design rationale |
| [competitive-matrix.md](about/competitive-matrix.md) | Competitive positioning analysis |
| [progress.md](../toke/docs/progress.md) | Project story tracker and gate status |

## Appendix B: Key Metrics Summary

| Metric | Value | Source |
|--------|-------|--------|
| Token reduction vs cl100k_base | 12.5% | Gate 1 evaluation |
| Pass@1 (7B model, 1,000 tasks) | 63.7% | Gate 1 evaluation |
| Tokenizer fertility | 0.374 | Phase 1 BPE tokenizer |
| Tokenizer vocabulary utilisation | 70.2% | 8K vocab, Phase 1 |
| Conformance tests | 600+ | Reference compiler (tkc) |
| Training corpus size | 46,754 programs | 4-stage compiler-in-the-loop pipeline |
| Standard library modules | 38 | C runtime backed |
| Keywords | 12 | Language specification |
| Character set | 56 | Language specification |
| Specification sections | 30 | v0.2 draft |
