# toke — Epics and User Stories by Phase

**Project:** tokelang  
**Repository:** https://github.com/tokelang/spec  
**Story format:** As a [actor], I want [capability], so that [outcome].  
**Acceptance criteria** are listed under each story as verifiable conditions.

---

## Phase 1 — Falsification (Months 1–8)

**Phase objective:** Validate the core thesis with minimal implementation. Build the compiler, generate the first corpus, run the first benchmark. If tk shows no material advantage at Gate 1, the project stops.

---

### EPIC 1.1 — Language Specification Lock

*Lock the Phase 1 language definition completely before writing a single line of compiler code. Every subsequent epic depends on this being stable.*

---

**Story 1.1.1 — Character set finalisation**

As a compiler engineer, I want the complete 80-character set formally documented with every character's structural role assigned, so that the lexer has an unambiguous character acceptance table with no open decisions.

Acceptance criteria:
- All 80 characters are enumerated in a table with class (lowercase, uppercase, digit, symbol)
- Every symbol has a named primary role and, where applicable, a named secondary role
- String literal delimiter is explicitly assigned to one symbol slot
- Characters excluded from structural positions are listed with rationale
- Document is version-controlled and marked frozen at M0
- No character appears in two roles that conflict at the same syntactic position

---

**Story 1.1.2 — Keyword table lock**

As a language designer, I want the complete keyword list fixed with each keyword's syntactic role defined, so that the parser can be built without anticipating future keyword additions.

Acceptance criteria:
- All 12 keywords are listed with their exact lexical form
- Each keyword's production rule is identified
- Boolean literals true and false are documented as predefined identifiers, not keywords
- No two keywords share a prefix that would require lookahead to disambiguate
- Document is marked frozen at M0

---

**Story 1.1.3 — Symbol disambiguation rules**

As a parser implementer, I want every symbol's role disambiguated by syntactic position rather than context-sensitive rules, so that the parser remains LL(1) with no state required to interpret a symbol.

Acceptance criteria:
- Every symbol with dual roles has an unambiguous position-based disambiguation rule documented
- The rule for each dual-role symbol can be expressed as "when preceded by X, this symbol means Y"
- No symbol requires knowledge of tokens more than one position ahead to interpret
- Examples are provided for each dual-role symbol demonstrating both usages

---

**Story 1.1.4 — Formal EBNF grammar**

As a compiler engineer, I want a complete formal EBNF grammar for toke Profile 1, so that the parser is built from a single authoritative document and conformance tests can be mechanically generated from it.

Acceptance criteria:
- Grammar covers all constructs: module, import, type, constant, function, all statement forms, all expression forms, all literal forms, all type expression forms
- Grammar is LL(1): no production requires more than one token of lookahead
- Grammar is unambiguous: validated by an automated parser generator with zero conflict warnings
- Grammar is committed to the spec/grammar.ebnf file in the repository
- An ANTLR4 grammar generated from the EBNF produces a working parser for all example programs

---

**Story 1.1.5 — Profile 2 transformation rules**

As a tooling developer, I want the Profile 1 to Profile 2 transformation rules documented precisely, so that a mechanical translator can convert any valid Profile 1 program to a valid Profile 2 program without human judgment.

Acceptance criteria:
- Uppercase type name to $ sigil rule is formally stated with no exceptions
- Array literal [] to @() rule is formally stated with no exceptions
- Array index a[n] to a.get(n) rule is formally stated
- Transformation is deterministic: one Profile 1 program maps to exactly one Profile 2 program
- A round-trip test exists: Profile 1 → Profile 2 → Profile 1 produces the original program
- At least 20 example pairs (Profile 1, Profile 2) are included in the spec

---

**Story 1.1.6 — Spec review and alignment**

As a compiler engineer, I want all Profile 1 spec documents reviewed for internal consistency and completeness before compiler work begins, so that the risk of mid-implementation spec changes is minimised.

Acceptance criteria:
- All five spec documents reviewed: character-set.md, keywords.md, symbol-disambiguation.md, grammar.ebnf, phase2-transform.md
- Every production in grammar.ebnf references only terminals defined in character-set.md and keywords.md
- Every disambiguation rule in symbol-disambiguation.md is present and correctly encoded in grammar.ebnf
- Every Profile 2 transformation in phase2-transform.md references valid grammar non-terminals from grammar.ebnf
- No contradictions between any two spec documents
- Any gap, ambiguity, or risk item that would require a compiler change post-implementation is documented
- A review report is committed to toke-spec/docs/spec-review-m0.md
- Any identified issues are either resolved in the spec or logged as a numbered risk item in the report

---

**Story 1.1.7 — Meta-repo README and project landing page**

As a new contributor or evaluator, I want a single public repository to land on that explains the toke project, links to all sub-repositories, and shows the current phase and gate status, so that I can orient myself without reading internal project docs.

Acceptance criteria:
- README.md is committed to the root of github.com/karwalski/toke
- Plain-language explanation of what toke is and why it exists (the thesis)
- Elevator pitch: one or two sentences suitable for a conference abstract
- Links to all 7 sub-repositories, each with a one-sentence description of its role
- Current phase and milestone status visible at a glance
- Getting-started section: minimum steps to build the compiler and run the conformance suite
- Licence and contributing section links to CONTRIBUTING.md and SECURITY.md in tkc
- No placeholder text; all content is accurate at the time of commit

---

### EPIC 1.2 — Reference Compiler Frontend

*Build the minimal compiler frontend in C: lexer, parser, name resolver, and type checker. Zero external dependencies. Structured error output from the first line of error-emitting code.*

---

**Story 1.2.1 — Lexer implementation**

As a compiler engineer, I want a C lexer that accepts a Profile 1 source file and produces a flat token stream with no whitespace tokens, so that the parser receives a clean token sequence independent of formatting.

Acceptance criteria:
- Lexer correctly classifies all 80 Profile 1 characters into their token classes
- Whitespace between tokens is silently consumed and produces no tokens
- Two programs differing only in whitespace between tokens produce identical token streams
- String literal content is captured verbatim including escape sequences
- Invalid escape sequences produce a structured E1001 diagnostic and halt
- Unterminated string literals produce a structured E1002 diagnostic and halt
- Characters outside the Profile 1 set in structural positions produce E1003
- Lexer processes a 200-token source file in under 1ms on reference hardware
- Lexer source is under 300 lines of C

---

**Story 1.2.2 — Parser implementation**

As a compiler engineer, I want a C parser that accepts the token stream from the lexer and produces an AST, so that all downstream stages work from a single structured representation.

Acceptance criteria:
- Parser implements the complete Profile 1 EBNF grammar with no extensions
- Parser is LL(1): no production examines more than one token of lookahead
- Every grammar violation produces a structured diagnostic with error code, position, and context
- AST nodes carry source position information (byte offset, line, column)
- Parser correctly handles all constructs defined in Section 7 of the spec
- Declaration ordering violations produce E2001
- Parse time for a 200-token source file is under 2ms on reference hardware
- Parser source is under 400 lines of C

---

**Story 1.2.3 — Import resolver**

As a compiler engineer, I want an import resolver that validates all imports against available interface files before type checking begins, so that unresolved imports fail fast with a helpful diagnostic listing available modules.

Acceptance criteria:
- Resolver loads .tki interface files for all declared imports
- Unresolved imports produce E2030 with a list of available modules in the diagnostic
- Circular imports are detected before type checking and produce E2031
- Resolver handles the std.* module path prefix
- Resolver produces the symbol table consumed by name resolution
- Resolver runs correctly on a project with 50 interdependent modules

---

**Story 1.2.4 — Name resolver**

As a compiler engineer, I want a name resolver that maps every identifier reference to its declaration, so that the type checker works from fully resolved names with no ambiguity.

Acceptance criteria:
- Every identifier reference is resolved to exactly one declaration
- Unresolved identifiers produce E3011
- Ambiguous identifiers produce E3012
- Module-qualified names (alias.name) resolve correctly via the import table
- Predefined identifiers (true, false, built-in types) resolve without declaration
- Name resolution handles all scoping rules: module scope, function scope, block scope

---

**Story 1.2.5 — Type checker**

As a compiler engineer, I want a type checker that enforces all type rules defined in the specification, so that a program that passes type checking has strong correctness guarantees that reduce LLM repair iterations.

Acceptance criteria:
- All 10 type checking rules from Section 13.6 of the spec are enforced
- No implicit coercions are performed; E4031 is emitted for any implicit coercion
- Exhaustive match enforcement: non-exhaustive match emits E4010
- Match arm type uniformity: differing arm types emit E4011
- Error propagation type compatibility: incompatible ! target emits E3020
- Struct field access validation: unknown field emits E4025
- Arena escape detection: value escaping arena scope emits E5001
- Every type error emits a structured diagnostic with fix field where derivable
- Type checking a 50,000-file corpus completes in under 60 seconds

---

**Story 1.2.6 — Structured diagnostic emitter**

As a repair loop engineer, I want every compiler error to be emitted as a JSON-schema-conforming diagnostic record, so that automated repair systems can consume errors without parsing English prose. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- All diagnostics conform to the schema defined in Section 9.1 of the spec
- schema_version is "1.0" for all Profile 1 diagnostics
- fix field is populated for all mechanically derivable corrections
- fix field is omitted (not null, not empty string) when no mechanical fix exists
- Diagnostics are emitted to stderr, one JSON object per line
- Human-readable text output is available via --diag-text flag
- JSON output is the default when stdout is not a terminal
- Every error code in the E1xxx through E5xxx ranges has at least one test case

---

**Story 1.2.7 — Interface file emitter**

As a build system developer, I want the compiler to emit a .tki interface file for each compiled module, so that dependent modules can be type-checked against the interface without recompiling the source.

Acceptance criteria:
- .tki file is emitted alongside the binary for every successful compilation
- .tki file contains module path, exported symbol names, kinds, and signatures
- .tki file is machine-readable (JSON or equivalent)
- A module can be type-checked using only its dependencies' .tki files
- .tki files support incremental compilation: changing an implementation file that does not change exported signatures does not require recompilation of dependents

---

**Story 1.2.8 — LLVM IR backend**

As a compiler engineer, I want a backend that lowers toke IR to LLVM IR and invokes LLVM to produce a native binary, so that toke programs run as self-contained native executables with no runtime dependency.

Acceptance criteria:
- LLVM IR is generated for all constructs defined in the Profile 1 grammar
- Native binaries are produced for x86-64 Linux, ARM64 Linux, and ARM64 macOS
- Produced binaries have no external runtime dependency beyond libc
- Binary overhead for a minimal toke program is under 100KB
- Hello world program compiles and executes correctly on all three targets
- Compilation of a 200-token source file to binary completes in under 50ms

---

**Story 1.2.9 — CLI interface**

As a developer, I want a standard command-line interface for tkc, so that the compiler can be invoked consistently from scripts, CI pipelines, and repair loop automation.

Acceptance criteria:
- tkc [flags] <source-files> invocation works
- --target flag accepts arch-os strings
- --out flag specifies binary output path
- --emit-interface flag triggers .tki emission
- --check flag runs type checking only without code generation
- --profile1 and --profile2 flags select character set profile
- --diag-json and --diag-text flags select diagnostic format
- Exit codes are: 0 (success), 1 (compilation error), 2 (internal error), 3 (usage error)
- --version prints compiler version and exits 0

---

**Story 1.2.10 — Conformance test suite (Profile 1)**

As a compiler engineer, I want a conformance test suite covering all Profile 1 language features, so that any change to the compiler can be validated against expected behaviour for every construct.

Acceptance criteria:
- Test suite covers all L-series (lexical), G-series (grammar), N-series (name resolution), T-series (type checking), C-series (code generation), and D-series (diagnostic) categories
- Each test specifies: input source, expected exit code, expected error codes if any, expected fix field contents, expected program output for execution tests
- Tests are runnable with a single command
- All tests pass on the initial compiler build
- Test suite is committed to tkc/test/ in the repository
- CI runs the full test suite on every commit

---

### EPIC 1.3 — Standard Library Core

*Implement the minimal stdlib required to generate useful programs: http, db, json, file. These are interfaces that the type checker validates; their implementations can be stubs initially.*

**Sequencing note:** std.str MUST be complete before Phase A corpus generation begins (Milestone M5). All other modules MUST be complete before Phase C corpus generation begins. std.str is a dependency of all other stdlib modules and MUST be worked first. The remaining five modules may be developed in parallel once std.str is done.

---

**Story 1.3.1 — std.str implementation**

As a program author, I want the std.str module available with all signatures defined in the spec, so that string manipulation is possible in generated programs from the earliest corpus phase.

Dependencies: none (must be done first)

Acceptance criteria:
- All functions listed in Section 17.1 of the spec are implemented
- All functions have correct type signatures enforced by the type checker
- String interpolation \(expr) syntax is handled by the compiler
- Functions are tested against known inputs and outputs
- .tki interface file is emitted and accurate

---

**Story 1.3.2 — std.http interface**

As a corpus engineer, I want the std.http module available with route registration working end-to-end, so that Phase C corpus programs can generate working HTTP handlers.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions listed in Section 17.2 of the spec are defined
- http.GET, http.POST, http.PUT, http.DELETE, http.PATCH route registration compiles
- Route pattern parameters (:id, :name) are parsed and accessible via req.param()
- A minimal HTTP server accepting a request and returning a response runs end-to-end
- .tki interface file is emitted and accurate

---

**Story 1.3.3 — std.db interface**

As a corpus engineer, I want the std.db module available with query functions typed correctly, so that database access patterns can appear in corpus programs from Phase B.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.3 of the spec are defined
- db.one, db.many, db.exec accept typed parameters
- Row accessor functions (row.str, row.u64, row.i64, row.f64, row.bool) type-check correctly
- DbErr sum type is defined and exhaustively matchable
- .tki interface file is emitted and accurate

---

**Story 1.3.4 — std.json interface**

As a corpus engineer, I want the std.json module available, so that JSON encoding and decoding appears in corpus programs and HTTP handlers.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.4 of the spec are defined
- json.dec returns Json!JsonErr and is correctly typed
- json.enc accepts Str and returns Str (generic version deferred)
- Field accessor functions type-check correctly
- .tki interface file is emitted and accurate

---

**Story 1.3.5 — std.file interface**

As a corpus engineer, I want the std.file module available, so that file I/O patterns can appear in Phase C corpus programs.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.5 of the spec are defined
- FileErr sum type is defined and exhaustively matchable
- file.read and file.write compile and execute end-to-end
- .tki interface file is emitted and accurate

---

**Story 1.3.6 — std.net interface**

As a corpus engineer, I want the std.net module available with TCP socket primitives, so that network communication patterns can appear in Phase C corpus programs beyond what std.http provides.

Dependencies: 1.3.1 (std.str), 1.3.5 (std.file)

Acceptance criteria:
- All types and functions in Section 17.6 of the spec are defined
- Socket type is defined with fd field
- NetErr sum type is defined (Connect, Read, Write, Timeout variants) and exhaustively matchable
- net.connect(host:Str;port:u64):Socket!NetErr is implemented
- net.read and net.write operate on Socket handles
- net.close(s:Socket):bool is implemented
- .tki interface file is emitted and accurate

---

### EPIC 1.4 — Mac Studio Setup and Local Pipeline

*Configure the hardware and establish the local inference and validation pipeline before any corpus generation begins.*

---

**Story 1.4.1 — Mac Studio configuration**

As a project lead, I want the Mac Studio M4 Max configured with all required development and ML tooling, so that local inference and training can begin without cloud dependency.

Acceptance criteria:
- macOS is updated to the latest stable release
- Xcode command line tools installed
- MLX and mlx-lm installed and tested
- Qwen 2.5 Coder 32B downloaded in 4-bit quantisation
- Qwen 2.5 Coder 7B downloaded in 4-bit quantisation
- Qwen 2.5 Coder 32B generates tokens at ≥20 tok/s sustained
- tkc compiles and runs on the Mac Studio
- Python environment with all corpus generation dependencies installed
- Claude API key configured and tested

---

**Story 1.4.2 — Local inference pipeline**

As a corpus engineer, I want a pipeline that sends generation tasks to Qwen locally and falls back to Claude API on failure, so that API costs are minimised without sacrificing corpus quality.

Acceptance criteria:
- Pipeline sends task + spec to Qwen 32B first (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)
- Generated code is passed to tkc for validation
- On compile success: entry is saved to corpus
- On compile failure: structured error is appended to prompt and retried locally (max 2 retries)
- On local retry exhaustion: task is escalated to Claude Haiku 4.5 batch API
- On API success: entry is saved to corpus
- On API failure after 3 attempts: task is logged to failures file for review
- Escalation rate per phase is tracked and logged
- Pipeline processes at least 100 tasks per hour on Phase A tasks

---

**Story 1.4.3 — Corpus storage schema**

As a corpus engineer, I want a consistent storage schema for all corpus entries, so that training data is structured, queryable, and reproducible.

Acceptance criteria:
- Each entry is stored as a JSON lines record with all metadata fields defined in Section 12.2
- task_id is a stable unique identifier derived from the task description hash
- Source code is stored verbatim with no post-processing
- Error traces are stored for all entries that required correction
- Corpus is stored on local NVMe with daily backups
- A script can regenerate corpus statistics (count by phase, mean token count, mean attempts) in under 60 seconds

---

### EPIC 1.5 — Phase A Corpus Generation

*Generate and validate 50,000 single-function programs using differential testing across four languages.*

---

**Story 1.5.1 — Task curriculum generator**

As a corpus engineer, I want a programmatic task generator that produces Phase A task descriptions covering all primitive operation categories, so that the curriculum is comprehensive without manual task authorship.

Acceptance criteria:
- Generator produces tasks covering all 6 Phase A categories from Section 13.2
- Each task includes a precise description, expected input/output specification, and type context
- Tasks are parameterised: the same template produces 100+ distinct variants
- Generated tasks are deduplicated before dispatch
- Generator produces at least 60,000 tasks (50,000 target + 20% buffer for failures)
- Task descriptions are concise enough that the spec + task fits in under 2,000 tokens (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

---

**Story 1.5.2 — Four-language parallel generation**

As a corpus engineer, I want every task dispatched to four language generators simultaneously, so that differential testing can validate tk correctness against reference implementations.

Acceptance criteria:
- tk, Python, C, and Java generations are dispatched in parallel for every task
- Test input generation runs as a fifth parallel call
- All five results are collected before validation begins
- Timeouts are handled gracefully: a slow language generation does not block the others
- Individual language failures are logged without discarding the entire task

---

**Story 1.5.3 — Differential test harness**

As a corpus engineer, I want a test harness that compiles and runs all four implementations against generated test inputs and applies majority vote, so that tk correctness is established without manually authored test cases.

Acceptance criteria:
- All four implementations are compiled using their respective compilers
- Compiled programs are executed against at least 50 distinct test inputs per task
- Majority vote logic correctly identifies: all agree (accept tk), tk differs from 3 (tk bug), one reference differs from 3 (reference bug, discard that language), all differ (task ambiguous, discard task)
- Majority vote results are stored in corpus entry metadata
- Discard reasons are logged for review
- Harness runs 100 task cycles per hour including compilation and execution

---

**Story 1.5.4 — Corpus quality metrics**

As a project lead, I want automated metrics on corpus quality updated daily, so that generation problems are detected early without manual audit.

Acceptance criteria:
- Daily report shows: total entries by phase, mean token count per language, mean attempts to success, failure rate by category, escalation rate to API
- Token density ratio (tk_tokens / baseline_tokens) is tracked per phase and category
- Declining first-pass compile rate triggers an alert
- Excessive failure rate in a task category triggers a review flag
- Report is generated automatically and committed to the repository

---

### EPIC 1.6 — Gate 1 Benchmark

*Measure and report the token efficiency and correctness results that determine whether the project proceeds.*

---

**Story 1.6.1 — Held-out benchmark task set**

As a validation engineer, I want a held-out set of 500 benchmark tasks that never appeared in the training corpus, so that Gate 1 results reflect generalisation not memorisation.

Acceptance criteria:
- 500 tasks spanning all Phase A categories
- Tasks are generated and locked before corpus generation begins
- No held-out task hash appears in the corpus task_id set
- Tasks are stored in benchmark/hidden_tests/ and not accessible to the generation pipeline
- Each task has at least 100 test inputs and verified reference outputs

---

**Story 1.6.2 — Token efficiency measurement**

As a validation engineer, I want precise token count measurements for all four languages on the benchmark task set using the cl100k_base tokenizer, so that Gate 1 has an empirical token efficiency result to compare against the 10% threshold.

Acceptance criteria:
- Token counts are measured for tk, Python, C, and Java on all 500 benchmark tasks
- Measurement uses cl100k_base tokenizer for all four languages
- Results include mean, median, p10, and p90 token counts per language
- tk_ratio (tk / Python baseline) is computed for each task and across the set
- Results are committed to benchmark/results/gate1_tokens.json

---

**Story 1.6.3 — Pass@1 measurement**

As a validation engineer, I want Pass@1 measurements for tk on the benchmark task set using a general-purpose LLM with the toke spec in context, so that Gate 1 has an empirical correctness result before any fine-tuning. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- Each benchmark task is attempted 10 times using Claude Haiku 4.5 with the full Profile 1 spec in the system prompt
- Pass@1 is computed as the fraction of tasks where at least 1 of 10 attempts compiles and passes all test inputs
- Mean repair iterations per task is recorded
- Results are committed to benchmark/results/gate1_pass.json
- Gate 1 pass condition (>10% token reduction AND Pass@1 ≥ 60%) is evaluated and documented

---

**Story 1.6.4 — Gate 1 decision document**

As a project lead, I want a written decision document produced at Month 8 summarising Gate 1 results and stating the go/no-go decision with rationale, so that the decision is auditable and unambiguous.

Acceptance criteria:
- Document states the measured token reduction percentage
- Document states the measured Pass@1 percentage
- Document compares results against gate criteria (>10% reduction, ≥60% Pass@1)
- Document states the decision: proceed to project Phase 2 or pivot to IR approach
- Document is committed to the repository and tagged as gate1-decision
- Decision is made within 2 weeks of M5.5

### EPIC 1.7 — Compiler Security and SAST

*Establish static analysis, fuzz testing, and secret scanning from the first compiler commit so that security defects are caught before public release.*

---

**Story 1.7.1 — SAST on compiler C code**

As a security engineer, I want automated static analysis running on all C code in tkc/ on every pull request, so that memory safety issues, buffer overruns, and undefined behaviour are caught before they reach main.

Acceptance criteria:
- cppcheck or clang-analyzer runs in CI on every PR to the tkc repository
- Any error-level finding blocks merge
- Warning-level findings are reviewed and either fixed or documented with rationale
- CI step is added to .github/workflows/ci.yml in the tkc repository
- A baseline run on the initial lexer.c produces zero errors
- Results are reported as CI annotations on the PR, not only in logs

---

**Story 1.7.2 — Compiler input fuzzing**

As a security engineer, I want fuzz testing on the tkc lexer and parser so that malformed or adversarial toke source cannot crash the compiler or produce undefined behaviour.

Acceptance criteria:
- libFuzzer or AFL++ fuzz target is created for the lexer token stream
- A fuzz target is created for the parser consuming a token stream
- Fuzzer runs for at least 60 seconds as part of the nightly CI job
- Any crash or sanitizer finding is treated as a P0 bug and blocks the next release
- Fuzzer corpus of interesting inputs is committed to tkc/test/fuzz/
- AddressSanitizer and UndefinedBehaviourSanitizer are enabled during fuzzing runs
- make fuzz target is documented in CONTRIBUTING.md

---

**Story 1.7.3 — Secret scanning in corpus pipeline**

As a security engineer, I want automated secret scanning on all commits to the corpus generation and model training repositories so that API keys, credentials, and tokens are never committed to git.

Acceptance criteria:
- GitHub secret scanning is enabled on the corpus and model repositories
- gitleaks or truffleHog runs in CI on every PR to those repositories
- .gitignore files in both repositories exclude .env files; this is verified as part of this story
- Pre-commit hook template is documented in CONTRIBUTING.md for local use
- Any secret detection finding blocks merge with zero exceptions
- The Claude API key and Qwen access credentials are documented as environment variable names only and are never hardcoded in any file

---

**Story 1.7.4 — Dependency vulnerability scanning**

As a security engineer, I want automated vulnerability scanning on all Python dependencies in the corpus, tokenizer, model, and benchmark repositories so that known CVEs in third-party libraries are detected.

Acceptance criteria:
- GitHub Dependabot is enabled on all four Python repositories
- pip-audit or safety runs in CI on every PR to those repositories
- Any critical or high severity CVE blocks merge
- pyproject.toml in each repository pins dependency versions
- Weekly automated dependency update PRs are enabled via Dependabot
- A baseline scan on initial project setup produces zero critical findings

---

**Story 1.7.5 — Signed commits and DCO enforcement**

As a project maintainer, I want all contributions to require a Developer Certificate of Origin sign-off so that every contributor has attested they have the right to submit the code under the project licence.

Acceptance criteria:
- DCO check is enabled via DCO GitHub App or probot/dco on all repositories
- Any PR without Signed-off-by in all commits is blocked from merge
- CONTRIBUTING.md in every repository documents the sign-off requirement with the exact git commit -s command
- Each repository README includes a brief DCO notice
- Initial commits from the project author are covered via a .github/dco-signoffs/ retroactive attestation file

---

### EPIC 1.8 — Corpus Pipeline Threat Model

*Identify and mitigate the security risks specific to the generate-compile-execute pipeline before it runs at scale.*

---

**Story 1.8.1 — Corpus pipeline threat model document**

As a security engineer, I want a written threat model for the corpus generation pipeline so that known attack surfaces are documented and mitigations are in place before the pipeline runs 50,000 programs.

Acceptance criteria:
- Threat model document is created at docs/security/threat-model.md
- Document covers the following threat categories: (a) LLM prompt injection via task descriptions; (b) malicious code execution via the differential test harness compiling and running generated programs; (c) API key exfiltration via generated programs that make network calls; (d) corpus poisoning via adversarial task descriptions; (e) holdout task leakage from the benchmark hidden test directory
- Each threat has a documented likelihood, impact, and mitigation
- The differential test harness execution environment is sandboxed (covered by 1.8.2)
- Document is reviewed and approved before Phase A corpus generation begins

---

**Story 1.8.2 — Sandboxed execution of generated programs**

As a security engineer, I want all generated programs executed during differential testing to run in a sandboxed environment so that a malicious generated program cannot affect the host system or exfiltrate credentials.

Acceptance criteria:
- Generated binaries are executed inside a container or sandbox (Docker, gVisor, or macOS sandbox-exec)
- Sandbox restricts: outbound network access, filesystem writes outside a designated temporary directory, and process spawning
- Execution timeout is enforced (maximum 10 seconds per program)
- If a program exceeds the timeout or the sandbox reports a violation, the entry is discarded and logged rather than retried
- The sandbox setup is documented and reproducible on the Mac Studio
- At least one test confirms that a program attempting an outbound HTTP call is blocked by the sandbox

---

### EPIC 1.9 — Remote Monitoring Console

*Provide a web-based monitoring and control interface for the Mac Studio's long-running operations — corpus generation, model training, and benchmark runs — accessible remotely over VPN or port-forwarding.*

**Sequencing note:** Stories 1.9.1 and 1.9.2 have no hardware dependency and can be developed and tested on any machine. Full integration testing against live corpus/training jobs requires Epic 1.4 (Mac Studio) to be complete. The service lives in `toke-corpus/monitor/`.

---

**Story 1.9.1 — Job manager daemon**

As a project operator, I want a lightweight background service running on the Mac Studio that tracks all long-running jobs and exposes a JSON REST API, so that the status of corpus generation, training runs, and benchmarks is always observable from any machine on the network.

Dependencies: none (can develop on any machine; integration requires 1.4.1)

Acceptance criteria:
- Service is implemented in Python (Flask + psutil), lives at toke-corpus/monitor/server.py and monitor/jobs.py
- GET /api/jobs returns all jobs with id, name, state, start time, duration, and last 5 log lines
- POST /api/jobs enqueues a new job with name and shell command; job runs as a subprocess
- GET /api/jobs/{id} returns full job detail including last 200 log lines from monitor/logs/{id}.log
- POST /api/jobs/{id}/cancel cancels a running job
- GET /api/status returns cpu%, mem%, and disk% via psutil
- Job states: queued, running, done, failed, cancelled
- Job list persists to monitor/jobs.json on every state change; restart does not lose history
- Listens on 0.0.0.0:7700 by default; overridable via TK_MONITOR_PORT
- If TK_MONITOR_TOKEN env var is set, every request requires Authorization: Bearer <token>

---

**Story 1.9.2 — Web dashboard**

As a project operator, I want a browser-based dashboard served by the job manager daemon, so that I can monitor and control long-running jobs from any device without installing additional software.

Dependencies: 1.9.1

Acceptance criteria:
- Dashboard is served at GET / by the same Flask process as the REST API
- All jobs are displayed in a table with state colour-coding (running=green, failed=red, cancelled=grey, done=blue)
- Dashboard auto-refreshes job table and system status bar every 5 seconds via fetch()
- Clicking a job row expands to show the last 50 log lines inline
- System status bar (CPU%, mem%, disk%) is shown at the top and updates every 5 seconds
- A "New Job" form accepts a job name and shell command and submits via POST /api/jobs
- Running jobs have a "Cancel" button that calls POST /api/jobs/{id}/cancel
- Pure HTML + vanilla JS; no build step; no external CDN dependencies; inline all styles and scripts
- Dark theme

---

**Story 1.9.3 — Log streaming**

As a project operator, I want live log output from running jobs to stream to the browser in real time, so that I can watch corpus generation or training progress without polling.

Dependencies: 1.9.1, 1.9.2

Acceptance criteria:
- GET /api/jobs/{id}/stream returns a Server-Sent Events stream of new log lines as they are written
- Dashboard log panel subscribes to the SSE stream when a running job is expanded
- Stream closes automatically when the job transitions to done, failed, or cancelled
- If the browser disconnects and reconnects, the full log tail is sent before resuming the stream
- Implementation uses Python threading; no asyncio dependency

---

**Story 1.9.4 — Authentication and HTTPS**

As a project operator, I want the monitoring console secured with a bearer token and optionally served over HTTPS, so that it is safe to expose via port-forwarding without relying solely on VPN.

Dependencies: 1.9.1, 1.9.2

Acceptance criteria:
- Bearer token auth is enforced when TK_MONITOR_TOKEN is set (story 1.9.1 implements the mechanism; this story validates and documents it)
- If TK_MONITOR_CERT and TK_MONITOR_KEY env vars are set, the Flask server starts with TLS using those cert/key paths
- Setup instructions for generating a self-signed certificate are documented in monitor/README.md
- A curl example demonstrating authenticated access is included in the README
- The port-forwarding setup guide covers both VPN access and direct port-forward with token auth

---

## Phase 2 — Language Viability (Months 6–14)

**Phase objective:** Extend tk with generics (collection types), a basic concurrency model, C FFI, and module versioning. Train the first fine-tuned model. Validate that token efficiency is retained with the extended language.

---

### EPIC 2.1 — Language Extensions

---

**Story 2.1.1 — Generic collection types**

As a program author, I want built-in generic collection types [T] and Map<K;V> usable without generic annotation in source, so that common data structure patterns are expressible without the complexity of full parametric polymorphism.

Acceptance criteria:
- [T] array type is fully supported with .len, indexing, and typed element access
- Map<K;V> is available as a stdlib type with put, get, delete, contains operations
- Type checker correctly infers T from array literal contents
- No user-defined generic type parameters are exposed (deferred)
- Existing Phase A corpus programs remain valid with no changes

---

**Story 2.1.2 — Async task model (minimal)**

As a corpus engineer, I want a minimal async task model that allows spawning a concurrent task and awaiting its result, so that Phase C system interaction programs can express parallelism without requiring full concurrency semantics.

Acceptance criteria:
- spawn(fn) creates a concurrent task returning a handle
- await(handle) blocks until the task completes and returns its result
- Errors from spawned tasks are propagated as typed errors on await
- The model is single-executor (no thread safety guarantees in v0.1)
- Compiler emits correct LLVM IR for spawn and await
- At least 20 test programs exercise the async model

---

**Story 2.1.3 — Minimal C FFI**

As a systems programmer, I want extern declarations that allow calling C functions from toke, so that programs can access platform APIs and existing C libraries without rewriting them.

Acceptance criteria:
- extern declaration syntax is defined and parsed
- C functions with scalar parameter and return types are callable from toke
- Raw pointer types (*u8, *void) are available in FFI contexts only
- FFI calls are not subject to arena lifetime checks (documented unsafe boundary)
- At least 5 stdlib functions are implemented via FFI internally
- FFI declarations do not appear in generated corpus programs (FFI is an implementation detail of stdlib)

---

**Story 2.1.4 — Module versioning**

As a package consumer, I want import declarations to support version constraints, so that dependency management is possible for multi-module projects.

Acceptance criteria:
- Import syntax supports an optional version field: I=alias:module.path@1.2
- Version resolution picks the highest compatible version available
- Version conflicts produce a structured diagnostic
- Interface files include version metadata
- A minimal package manifest format (tokelang.json) is defined

---

### EPIC 2.2 — Profile 2 Tokenizer

---

**Story 2.2.1 — Profile 1 corpus preparation for tokenizer training**

As an ML engineer, I want the Profile 1 corpus prepared as a clean text dataset for BPE tokenizer training, so that the purpose-built tokenizer is trained on representative, high-quality toke source.

Acceptance criteria:
- All validated Profile 1 corpus entries are extracted as raw .toke source text
- String literal contents are stripped and replaced with placeholder tokens
- Dataset is deduplicated at the character level
- Dataset size is at least 500,000 source files
- Dataset is split: 90% train, 10% validation
- Preparation script runs in under 4 hours on Mac Studio

---

**Story 2.2.2 — BPE tokenizer training**

As an ML engineer, I want a purpose-built BPE tokenizer trained on the toke corpus with a 32,768-token vocabulary, so that Profile 2 achieves substantially lower token counts than cl100k_base.

Acceptance criteria:
- Tokenizer is trained using a standard BPE implementation (SentencePiece or tiktoken-compatible)
- Vocabulary size is exactly 32,768
- Training completes in under 12 hours on Mac Studio
- The 100 most common toke constructs are each represented as a single token
- Validation perplexity on held-out corpus is lower than cl100k_base
- Tokenizer is saved in a standard format loadable by the fine-tuning pipeline

---

**Story 2.2.3 — Tokenizer evaluation**

As a validation engineer, I want the purpose-built tokenizer evaluated against cl100k_base on the Gate 1 benchmark task set, so that the token density improvement is quantified before committing to project Phase 2.

Acceptance criteria:
- Token counts are measured for Profile 2 source using the purpose-built tokenizer
- Token counts are measured for Profile 1 source using cl100k_base
- Token density improvement ratio is computed for each benchmark task
- Mean improvement across the benchmark set is reported
- Target: at least 2x token density improvement over cl100k_base for Profile 2 source
- Results are committed to benchmark/results/tokenizer_eval.json

---

### EPIC 2.3 — First Fine-Tuned Model

---

**Story 2.3.1 — Training data preparation**

As an ML engineer, I want the Phase A corpus formatted as instruction-tuning examples for Qwen 2.5 Coder 7B, so that the fine-tuning run has clean, correctly formatted data. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- All Phase A corpus entries are converted to [INST]/[/INST] format
- Correction examples (broken code, error, fixed code) are formatted as three-turn conversations
- Training and validation splits are created (90/10)
- Data is formatted as JSONL compatible with the MLX training script
- At least 45,000 training examples and 5,000 validation examples are available
- Data preparation script runs in under 2 hours on Mac Studio

---

**Story 2.3.2 — QLoRA fine-tune run**

As an ML engineer, I want the Qwen 2.5 Coder 7B model fine-tuned on the Phase A corpus using QLoRA on Mac Studio, so that a toke-specialised model exists for evaluation.

Acceptance criteria:
- Fine-tune runs to completion on Mac Studio using MLX
- Training configuration matches the parameters in Section 15.2 of the RFC
- Validation loss decreases monotonically across 3 epochs
- Training completes within 24 hours
- Adapter weights are saved and loadable
- A merged model is saved to disk in a format usable for inference

---

**Story 2.3.3 — First model accuracy benchmark**

As a validation engineer, I want the fine-tuned 7B model benchmarked against the Gate 1 held-out task set, so that the training improvement is quantified.

Acceptance criteria:
- Pass@1 is measured for the fine-tuned 7B on all 500 Gate 1 benchmark tasks
- Mean repair iterations is measured
- Results are compared against the pre-fine-tune baseline from Gate 1
- Fine-tuned model must show improvement over baseline on Pass@1
- Results are committed to benchmark/results/phase2_model_eval.json

---

### EPIC 2.4 — Phase B and C Corpus

---

**Story 2.4.1 — Phase B corpus generation (data structures)**

As a corpus engineer, I want 30,000 validated multi-file data structure programs in the corpus, so that the model learns to work with type definitions, multiple files, and arena-aware allocation patterns.

Acceptance criteria:
- All Phase B categories from Section 13.2 are represented
- Each entry spans 2–4 source files
- Type definition files and function implementation files are separate
- Differential testing validates correctness across all entries
- Mean tk_ratio for Phase B entries is recorded
- Corpus storage schema is consistent with Phase A entries

---

**Story 2.4.2 — Phase C corpus generation (system interaction)**

As a corpus engineer, I want 20,000 validated system interaction programs in the corpus, so that the model learns to use network, file, and database stdlib modules correctly.

Acceptance criteria:
- All Phase C categories from Section 13.2 are represented
- HTTP server programs compile and serve responses correctly
- Database query programs compile and execute against a test database
- File I/O programs compile and read/write files correctly
- Differential testing is applied where possible; integration tests replace it where reference language equivalents are impractical
- Escalation rate to Sonnet 4.6 is tracked and does not exceed 70%

---

### EPIC 2.5 — Gate 2 Benchmark

---

**Story 2.5.1 — Phase B and C benchmark task set**

As a validation engineer, I want a held-out benchmark set of 200 Phase B and 200 Phase C tasks, so that Gate 2 can measure multi-file and system-level generation quality.

Acceptance criteria:
- 200 Phase B tasks (multi-file, data structures) are locked before Phase B corpus generation begins
- 200 Phase C tasks (system interaction) are locked before Phase C corpus generation begins
- No held-out task hash appears in any corpus entry
- Each task has reference implementations in all four languages where applicable

---

**Story 2.5.2 — Gate 2 evaluation and decision**

As a project lead, I want Gate 2 results quantifying extended-language token efficiency and trained model performance, so that the decision to proceed to Phase 3 is evidence-based.

Acceptance criteria:
- Token reduction measured on Phase B and C benchmark tasks with purpose-built tokenizer
- Fine-tuned 7B Pass@1 measured on Phase B and C benchmark tasks
- Fine-tuned 7B compared against unfine-tuned Qwen 7B on same tasks
- Gate 2 criteria evaluated: token efficiency retained AND fine-tuned model outperforms baseline
- Decision document committed to repository and tagged gate2-decision
- Decision made within 2 weeks of Month 14

### EPIC 1.10 — Phase 1 Integration and Conformance Review

*Verify end-to-end that everything built in Phase 1 matches its specification and runs without errors. Identify gaps, wire test infrastructure, and confirm the system is ready for corpus generation.*

---

**Story 1.10.1 — Phase 1 full integration and conformance review**

As a project lead, I want a structured verification pass over every Phase 1 component — compiler, standard library, monitoring console — that confirms each piece runs correctly, matches the spec, and is ready for corpus generation, so that known gaps are documented before corpus work begins.

Acceptance criteria:
- Conformance test harness (`make conform`) is wired and runs all 62 YAML test cases end-to-end; pass rate documented
- Every compiler pass (lexer, parser, import resolver, name resolver, type checker, diag emitter, interface emitter, LLVM backend, CLI) verified against its corresponding spec section and test series
- All 5 stdlib unit test suites pass (std.str, std.db, std.json, std.file, std.http); .tki interface files verified present and matching compiled signatures
- Monitoring console endpoints verified: GET /api/jobs, POST /api/jobs, GET /api/jobs/{id}, GET /api/status, POST /api/jobs/{id}/cancel, GET /api/jobs/{id}/stream
- All known tech-debt items (arena escape E5001 conservatism, LLVM cast stub, struct field GEP offset-0, nested break) are documented in a review report with Phase 2 priority
- Corpus pipeline readiness assessed: what runs now, what needs Mac Studio, what can be scaffolded
- Review report committed to tkc/docs/phase1-review.md

---

### EPIC 2.6 — Responsible Disclosure and CVE Process

*Establish a security vulnerability reporting process before the compiler and tools are publicly used.*

---

**Story 2.6.1 — Security policy and disclosure process**

As a project maintainer, I want a documented security policy and responsible disclosure process published before the first public release so that researchers know how to report vulnerabilities safely.

Acceptance criteria:
- SECURITY.md is added to every public repository
- SECURITY.md documents: (a) how to report a vulnerability (private email or GitHub private vulnerability reporting); (b) what information to include in a report; (c) expected response timeline: acknowledge within 72 hours, triage within 7 days; (d) the disclosure timeline: fix within 90 days, coordinated public disclosure thereafter; (e) what is in scope: the compiler, corpus pipeline, released models; (f) what is out of scope: third-party dependencies, user-written toke programs
- GitHub private vulnerability reporting is enabled on the compiler and corpus repositories
- A security contact email is established (security@tokelang.dev or equivalent)
- The policy is linked from each repository README

---

**Story 2.6.2 — CVE coordination process**

As a project maintainer, I want a documented process for issuing CVEs when significant compiler vulnerabilities are discovered so that downstream users are notified through established channels.

Acceptance criteria:
- Process document is created at docs/security/cve-process.md
- Document covers: how to request a CVE from MITRE or GitHub Advisory, how to notify major downstream users, how to publish advisories
- At least one point of contact with GitHub Advisory Database is established
- The process is referenced in SECURITY.md
- An internal test scenario (non-public) is run through the process to verify it works before a real vulnerability arrives

---

### EPIC 2.7 — Standard Library Expansion

*Extend the stdlib with modules required to validate that toke can express real-world application patterns beyond what Phase 1 corpus programs demonstrate.*

**Sequencing note:** These modules are needed for Gate 2 validation (language viability). They are not needed for Gate 1. Development begins after Gate 1 passes and the Phase 2 compiler extensions are stable.

---

**Story 2.7.1 — std.process**

As a program author, I want a std.process module that allows spawning and communicating with subprocesses, so that toke programs can orchestrate system tools without dropping to C FFI.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- process.spawn(cmd:[Str]):Handle!ProcessErr is implemented
- process.wait(h:Handle):i32!ProcessErr is implemented
- process.stdout(h:Handle):Str!ProcessErr is implemented
- process.kill(h:Handle):bool is implemented
- ProcessErr sum type covers: NotFound, Permission, IO variants
- Module compiles cleanly with tkc --check
- At least 10 corpus programs use std.process in Phase B-C generation

---

**Story 2.7.2 — std.env**

As a program author, I want a std.env module for reading environment variables and configuration, so that toke programs can be configured without hardcoded values.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- env.get(key:Str):Str!EnvErr is implemented
- env.get_or(key:Str;default:Str):Str is implemented
- env.set(key:Str;val:Str):bool is implemented
- EnvErr sum type covers: NotFound, Invalid variants
- Module compiles cleanly with tkc --check

---

**Story 2.7.3 — std.crypto**

As a program author, I want a std.crypto module providing hashing and HMAC primitives, so that toke HTTP handlers can implement authentication without depending on C libraries directly.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- crypto.sha256(data:[Byte]):[Byte] is implemented
- crypto.hmac_sha256(key:[Byte];data:[Byte]):[Byte] is implemented
- crypto.to_hex(data:[Byte]):Str is implemented
- Implementation uses C FFI to a standard crypto library internally
- Module compiles cleanly with tkc --check

---

**Story 2.7.4 — std.time**

As a program author, I want a std.time module for working with timestamps and durations, so that toke programs can implement rate limiting, caching, and logging without platform-specific calls.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- time.now():u64 returns Unix timestamp in milliseconds
- time.format(ts:u64;fmt:Str):Str formats a timestamp
- time.since(ts:u64):u64 returns milliseconds elapsed
- Module compiles cleanly with tkc --check

---

**Story 2.7.5 — std.test**

As a corpus engineer, I want a std.test module providing assertion primitives, so that toke test programs in the corpus can verify their own output without relying on external test runners.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- test.assert(cond:bool;msg:Str):bool is implemented
- test.assert_eq(a:Str;b:Str;msg:Str):bool is implemented
- test.assert_ne(a:Str;b:Str;msg:Str):bool is implemented
- Failed assertions emit a structured diagnostic to stderr
- Module compiles cleanly with tkc --check
- At least 20 Phase B-C corpus programs use std.test assertions

---

## Phase 3 — Ecosystem Proof (Months 14–26)

**Phase objective:** Build the full end-to-end ecosystem. Expand the corpus through all phases. Train 7B and 32B models. Establish the self-improvement loop. Demonstrate multi-model generation quality.

---

### EPIC 2.8 — LLVM Backend Correctness

---

**Story 2.8.1 — Fix LLVM IR emission for end-to-end compilation**

As a compiler engineer, I want `tkc` to emit valid LLVM IR for all Phase 1 and Phase 2 language constructs so that `clang` can assemble the output into a working binary.

Dependencies: 2.1 (Phase 2 Language Extensions — done)

Background: The front-end (lex → parse → names → types → interface) works correctly and passes 79 conformance tests. The LLVM IR backend (`llvm.c`, ~460 lines) emits `.ll` text that `clang` then assembles. However, several codegen bugs prevent end-to-end compilation. This story fixes all known defects so that a valid toke program compiles to a runnable binary.

Known defects:

1. **Parameter load uses wrong SSA name (P0).** `NODE_IDENT` emits `load i64, ptr %name` but function params are spilled to `%name.addr`. The load must use `%name.addr` for params (not locals). Fix: track which identifiers are params (e.g. emit `%name` directly without load, or reference `%name.addr`).

2. **`let` binding init reads wrong child index.** `emit_stmt` for `NODE_BIND_STMT` checks `child_count >= 3` and reads `children[2]`, but bindings have 2 children: `[0]=IDENT, [1]=expr`. Fix: check `child_count >= 2`, use `children[1]`.

3. **All function calls assume `i64` return type.** `NODE_CALL_EXPR` always emits `call i64 @fn(...)`, even for functions returning `void`, `f64`, `bool`, or `str`. Fix: look up the callee's return type and emit the correct LLVM type.

4. **Match arm indexes wrong child.** Match arm emission reads `arm->children[1]` as the body, but arms have 3 children: `[0]=pattern, [1]=binding, [2]=body`. Fix: emit `arm->children[2]`.

5. **`as` cast is identity stub.** Currently a no-op `add`. Fix: emit `sitofp`/`fptosi`/`trunc`/`zext`/`bitcast` as appropriate for the source and target types.

Acceptance criteria:
- `tkc --out binary input.tk` produces a runnable binary for programs using: arithmetic, function calls with params, let bindings, if/else, loops, return, string literals, struct literals, array literals
- All 5 known defects above are fixed
- At least 5 end-to-end tests: compile → run → check output (exit code or stdout via `puts`)
- Existing 79 conformance tests still pass (`make conform`)
- No new compiler warnings under `-Werror`

---

### EPIC 2.9 — Tokenizer Pipeline Scaffolding

---

**Story 2.9.1 — Corpus preparation script (prepare.py)**

As an ML engineer, I want the corpus preparation script fully implemented and tested with synthetic data, so that when the real corpus arrives it can be processed without delay.

Dependencies: None (synthetic test data)

Acceptance criteria:
- `prepare.py` accepts JSONL corpus entries and outputs a deduplicated text file for BPE training
- String literal contents are replaced with placeholder tokens
- Dataset is split 90/10 (train/validation)
- Unit tests cover: JSONL parsing, deduplication, placeholder replacement, splitting, edge cases (empty files, malformed entries)
- Script runs successfully on synthetic corpus of 100 generated toke programs
- `pyproject.toml` updated with runtime dependencies (sentencepiece or tokenizers)

---

**Story 2.9.2 — BPE training wrapper (train.py)**

As an ML engineer, I want the BPE training script fully implemented with SentencePiece integration, so that tokenizer training is a single command once the corpus is ready.

Dependencies: 2.9.1

Acceptance criteria:
- `train.py` wraps SentencePiece BPE training with configurable vocab size (default 32,768)
- Accepts prepared text file from 2.9.1
- Outputs a tokenizer model in a standard format (sentencepiece .model + .vocab)
- Unit tests verify: argument parsing, config validation, training on small synthetic corpus produces a loadable model
- Training completes on the synthetic corpus in under 60 seconds

---

**Story 2.9.3 — Tokenizer evaluation script (eval.py)**

As a validation engineer, I want the evaluation script fully implemented to compare any trained tokenizer against cl100k_base, so that token density improvement can be measured as soon as a tokenizer is trained.

Dependencies: 2.9.2

Acceptance criteria:
- `eval.py` loads a trained tokenizer and cl100k_base, tokenises the benchmark task set, and computes token count ratios
- Output is JSON: per-task token counts, mean/median improvement, summary statistics
- Loads benchmark tasks from toke-benchmark hidden_tests/ directory
- Unit tests cover: metric computation, JSON output schema, edge cases (empty programs, single-token programs)
- Script runs successfully using the synthetic tokenizer from 2.9.2 tests

---

### EPIC 2.10 — Benchmark Harness and Reference Implementations

---

**Story 2.10.1 — Benchmark evaluation harness (run.py, score.py, report.py)**

As a validation engineer, I want the benchmark evaluation harness fully implemented, so that model outputs can be scored as soon as a model produces them.

Dependencies: None

Acceptance criteria:
- `harness/run.py` accepts a model output directory and a task set, executes generated programs against test cases, and records results
- `harness/score.py` computes Pass@1, Pass@3, Pass@5 metrics; per-category breakdowns; token efficiency via cl100k_base
- `harness/report.py` generates a JSON summary and human-readable report
- Unit tests cover: scoring logic with mock outputs (all-pass, all-fail, partial), metric edge cases, report format
- Harness runs in dry-run mode without a model

---

**Story 2.10.2 — Phase A Python reference implementations (50+ tasks)**

As a validation engineer, I want Python reference implementations for at least 50 Phase A benchmark tasks, so that differential testing can begin as soon as corpus generation starts.

Dependencies: None (tasks already exist in toke-benchmark/hidden_tests/)

Acceptance criteria:
- At least 50 Python functions in `baselines/python/` corresponding to Phase A tasks
- Each function passes all test cases for its task (verified by a test script)
- Implementations are idiomatic Python, not generated
- A runner script executes all baselines against the hidden test YAML files and reports pass/fail

---

**Story 2.10.3 — Phase A C reference implementations (50+ tasks)**

As a validation engineer, I want C reference implementations for at least 50 Phase A benchmark tasks, so that the four-language differential testing pipeline has a C baseline.

Dependencies: None

Acceptance criteria:
- At least 50 C implementations in `baselines/c/` corresponding to Phase A tasks
- Each implementation compiles cleanly with `-Wall -Wextra -Werror` and passes all test cases
- A runner script compiles and executes all baselines against the hidden test YAML files

---

**Story 2.10.4 — Benchmark CI workflow**

As a validation engineer, I want CI that validates all benchmark tasks have correct schemas and all reference implementations pass their tests.

Dependencies: 2.10.2

Acceptance criteria:
- GitHub Actions workflow validates: task YAML schemas, unique task IDs, test case count per task
- Workflow runs reference implementation test suite
- Workflow fails if any reference implementation fails its test cases

---

### EPIC 2.11 — Corpus Pipeline Test Infrastructure

---

**Story 2.11.1 — Corpus pipeline unit tests**

As a corpus engineer, I want comprehensive unit tests for all corpus pipeline modules, so that pipeline correctness is verified before hardware arrives.

Dependencies: None

Acceptance criteria:
- `test_curriculum.py`: task generation determinism, variant coverage, ID uniqueness (curriculum.py is 1,285 lines — it needs tests)
- `test_validate.py`: tkc invocation mocking, diagnostic JSON parsing, error code extraction
- `test_validate_schema.py`: valid/invalid corpus entries against schema.json
- `test_compile.py`: compilation wrapper behaviour, timeout handling
- `test_vote.py`: majority voting edge cases (unanimous, 2-way tie, all disagree, single voter)
- All tests pass with `pytest` and no external dependencies (mock tkc, mock model)

---

**Story 2.11.2 — Corpus pipeline dry-run integration test**

As a corpus engineer, I want a dry-run integration test that exercises the full pipeline end-to-end with mock data, so that pipeline integration is verified without hardware.

Dependencies: 2.11.1

Acceptance criteria:
- Single command runs: generate task → generate code (dry-run) → validate → differential (mocked refs) → judge (mocked)
- Pipeline produces a valid corpus entry matching schema.json
- Test completes in under 30 seconds
- Test runs in CI

---

### EPIC 2.12 — Specification Completion

---

**Story 2.12.1 — Error code registry (spec/errors.md)**

As a repair loop engineer, I want the complete error code registry extracted from the compiler source and published in the spec, so that automated repair systems have a stable reference.

Dependencies: None (extract from tkc source)

Acceptance criteria:
- All E-series, W-series codes documented with: code, meaning, stage, example trigger, fix field value
- Extracted mechanically from tkc src/ (diag.h, parser.h, types.h, names.h, ir.h, llvm.h)
- Cross-referenced with conformance test IDs where applicable
- Published in toke-spec/spec/errors.md

---

**Story 2.12.2 — Formal semantics stub (spec/semantics.md)**

As a language standardiser, I want the type rules and scoping rules documented from the reference implementation, so that the semantics spec is started before the Phase 4 formalisation push.

Dependencies: None (extract from tkc types.c, names.c)

Acceptance criteria:
- All 10+ type checking rules documented in prose (formal inference rules deferred to 4.1.2)
- Scope chain rules documented: module, function, block, loop, match arm
- Predefined identifiers listed with their types
- Arena escape rules documented with examples
- Published in toke-spec/spec/semantics.md

---

**Story 2.12.3 — Standard library signatures (spec/stdlib-signatures.md)**

As a compiler implementer, I want all stdlib function signatures documented in one place, so that alternate implementations can be built without reading the C source.

Dependencies: None (extract from tkc stdlib/*.h and stdlib/*.tki)

Acceptance criteria:
- All 9 modules documented: str, json, file, process, env, crypto, time, test, log
- Each function: name, parameter types, return type, error type
- Each error sum type: variant names and meanings
- Published in toke-spec/spec/stdlib-signatures.md

---

### EPIC 2.13 — Standard Library Documentation

---

**Story 2.13.1 — Complete stdlib module documentation**

As a toke developer, I want each stdlib module fully documented with function signatures, examples, and error handling guidance in toke-stdlib.

Dependencies: None

Acceptance criteria:
- All 12 .md files in toke-stdlib/std/ completed (currently stubs)
- Each documents: module purpose, all function signatures, parameter descriptions, return values, error types and when they occur
- At least one usage example per function
- Cross-references to the .tki interface file

---

### EPIC 3.1 — Production Compiler

---

**Story 3.1.1 — Compiler hardening and performance**

As a compiler engineer, I want the compiler hardened to handle the full language spec robustly at production quality, so that the corpus generation pipeline and CI run without compiler bugs.

Acceptance criteria:
- Compiler handles all constructs from the extended Phase 2 language without crashing
- Internal compiler errors (exit code 2) do not occur on any valid or invalid toke source
- Compiler processes 1,000 files per minute in batch mode
- All conformance tests pass
- Memory usage during compilation of a 50,000-file corpus stays under 4GB

---

**Story 3.1.2 — Incremental compilation**

As a corpus engineer, I want incremental compilation where only changed files and their dependents are recompiled, so that the validation pipeline processes large corpora efficiently.

Acceptance criteria:
- Changed-file detection uses content hash, not modification time
- Only files whose transitive interface dependencies have changed are recompiled
- Incremental recompile of a changed leaf file in a 50-module project completes in under 5ms
- Incremental compile produces bit-identical output to full recompile

---

**Story 3.1.3 — Tooling protocol server**

As a repair loop engineer, I want the compiler exposed as a tooling protocol server accepting JSON operations over stdin/stdout, so that the repair loop can drive compilation without shell invocation overhead.

Acceptance criteria:
- All operations from Section 20.2 of the RFC are implemented: parse, type_check, compile, emit_interface, run, run_tests, emit_diagnostics
- Request and response schemas match the normative schemas in Appendix C of the spec
- Server handles concurrent requests via multiplexing
- Round-trip latency for a type_check operation on a 200-token file is under 20ms
- Server is exercised by the corpus generation pipeline

---

### EPIC 3.2 — Phase D and E Corpus

---

**Story 3.2.1 — Phase D corpus generation (applications)**

As a corpus engineer, I want 5,000 multi-module application programs generated and validated, so that the model learns to design coherent multi-file systems.

Acceptance criteria:
- All Phase D categories from Section 13.2 are represented
- Each entry spans 5–8 source files with sensible module boundaries
- Agent 3 (Claude Sonnet) architectural review is applied to 20% of entries
- Agent 2 (Qwen judge) reviews all entries that pass compilation
- Entries failing architectural review are either corrected or discarded
- tk_ratio and perf_ratio are recorded for all entries

---

**Story 3.2.2 — Phase E corpus generation (complex systems)**

As a corpus engineer, I want 500 complex system programs generated and human-reviewed, so that the training set has aspirational ceiling examples demonstrating full-system architecture.

Acceptance criteria:
- All Phase E categories from Section 13.2 are represented
- Every entry receives human architectural review by a senior developer
- Human review addresses: module design coherence, error handling completeness, idiomatic use of tk patterns
- Rejected entries are logged with review notes
- Accepted entries include the reviewer's notes as structured metadata
- Phase E corpus generation stays within the 80-person-hour human review budget

---

### EPIC 3.3 — Agent Review Pipeline

---

**Story 3.3.1 — Qwen judge agent**

As a corpus engineer, I want the Qwen 32B judge agent running locally with a structured evaluation rubric, so that corpus quality review is automated for Phases A through C without API cost.

Acceptance criteria:
- Judge agent evaluates: task match, module boundary quality, error propagation completeness, deduplication, edge-case coverage
- Judge produces pass, flag, or reject with structured JSON reasoning
- Flagged entries are reviewed by Agent 3 rather than discarded
- Judge runs at ≥50 evaluations per hour on Mac Studio
- Judge false positive rate (valid programs incorrectly rejected) is under 5% on a manually reviewed sample

---

**Story 3.3.2 — Architectural review agent integration**

As a corpus engineer, I want Claude Sonnet 4.6 integrated as Agent 3 for architectural review of Phase D and E entries, so that multi-module design quality is assessed at a level Qwen cannot reliably provide.

Acceptance criteria:
- Agent 3 is invoked for 20% of Phase D entries and 60% of Phase E entries that passed Agents 1 and 2
- Evaluation rubric covers: multi-module coherence, abstraction boundaries, error strategy consistency, idiomatic domain patterns
- Agent 3 produces pass, revise, or reject with specific feedback
- Revision feedback is used to regenerate and re-evaluate the entry
- API cost per Agent 3 invocation is tracked

---

### EPIC 3.4 — Model Training at Scale

---

**Story 3.4.1 — 7B model on full corpus**

As an ML engineer, I want the 7B model fine-tuned on the complete Phase A through D corpus, so that the production 7B model reflects the full training curriculum.

Acceptance criteria:
- Training data includes all validated Phase A through D entries
- Correction examples are weighted 2x relative to direct generation examples
- Training runs to completion with decreasing validation loss
- Fine-tuned model is saved with training metrics
- Model achieves Pass@1 ≥ 70% on the Gate 3 D2C benchmark set

---

**Story 3.4.2 — 32B model on full corpus**

As an ML engineer, I want the 32B model fine-tuned on the full corpus, so that a higher-capability model is available for complex Phase D and E generation tasks.

Acceptance criteria:
- Training runs on Mac Studio for Phase A through C data
- Cloud A100 burst used if Mac Studio training exceeds 96 hours
- 32B model achieves Pass@1 ≥ 75% on the Gate 3 D2C benchmark set
- 32B model achieves Pass@1 ≥ 55% on the Phase C benchmark set
- Model is evaluated on Phase D tasks and results are committed

---

### EPIC 3.5 — Self-Improvement Loop

---

**Story 3.5.1 — Error pattern analysis pipeline**

As an ML engineer, I want automated weekly analysis of structured compiler errors across the corpus, so that the most common failure patterns are identified and targeted with new training examples.

Acceptance criteria:
- Error analysis script aggregates all error_code values from corpus error traces
- Top 10 error patterns by frequency are identified each week
- For each top error pattern, 1,000 new targeted training examples are generated
- New examples are validated and added to the corpus
- Error frequency trends are tracked over time: decreasing frequency for targeted errors indicates loop is working

---

**Story 3.5.2 — Autonomous corpus growth**

As an ML engineer, I want the fine-tuned model continuously generating new corpus entries without human initiation, so that the corpus grows and improves without engineering intervention after setup. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tki interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- Model generates 500 new Phase A–C programs per day autonomously
- Generated programs are passed through the full validation pipeline
- Programs passing validation are added to the corpus
- Programs failing are added as correction examples
- Daily corpus growth is logged
- Human intervention is not required for normal operation

---

### EPIC 3.6 — Multi-Model Evaluation

---

**Story 3.6.1 — Llama family evaluation**

As a validation engineer, I want the Llama 3 family evaluated on the toke benchmark set, so that Gate 3 can demonstrate multi-model generation quality rather than single-model performance.

Acceptance criteria:
- Llama 3.1 8B and Llama 3.1 70B are evaluated on the Gate 3 D2C benchmark set with the toke spec in context (unfine-tuned)
- Pass@1 and mean repair iterations are recorded
- Results are compared against fine-tuned Qwen 7B and 32B
- Llama fine-tuning is explored if baseline performance is below 50% Pass@1

---

**Story 3.6.2 — Gate 3 evaluation and decision**

As a project lead, I want Gate 3 results across multiple model families and all benchmark categories, so that the decision to proceed to Phase 4 standardisation is grounded in broad evidence.

Acceptance criteria:
- At least two model families achieve ≥70% Pass@1 on D2C tasks
- Self-improvement loop shows measurable corpus quality improvement over 3 months
- Phase C benchmark results (system interaction) are within acceptable range
- Gate 3 decision document committed to repository and tagged gate3-decision
- Decision made within 2 weeks of Month 26

### EPIC 3.7 — Supply Chain Security and Release Signing

*Produce verifiable, reproducible releases with a software bill of materials and cryptographic signing before any production adoption.*

---

**Story 3.7.1 — SBOM generation for compiler releases**

As a security engineer, I want a Software Bill of Materials generated for every compiler release so that users and enterprises can verify the dependency chain of the compiler binary.

Acceptance criteria:
- SBOM is generated in SPDX or CycloneDX format as part of the release workflow
- SBOM covers all statically linked libraries including LLVM components
- SBOM is published as a release artefact alongside the binary
- The GitHub Actions release workflow automatically generates and attaches the SBOM on every tagged release
- SBOM generation is tested on at least one release candidate before the production process is relied upon

---

**Story 3.7.2 — Release binary signing**

As a security engineer, I want all compiler release binaries signed with a verifiable key so that users can confirm they received an authentic binary that has not been tampered with.

Acceptance criteria:
- Releases are signed using sigstore cosign (preferred) or GPG
- The signing key or sigstore identity is documented in the repository
- Verification instructions are included in each set of release notes
- The release workflow automatically signs binaries before publishing
- At least one test confirms that a tampered binary fails signature verification
- The signing process is documented at docs/security/release-signing.md

---

**Story 3.7.3 — Reproducible builds for the compiler**

As a security engineer, I want the compiler to build reproducibly from source so that any user can verify the released binary matches the published source code.

Acceptance criteria:
- Two independent builds from the same source commit produce byte-identical binaries after stripping build timestamps
- Reproducibility is verified in CI by building twice and comparing the outputs
- Build environment is fully specified: compiler version, LLVM version, OS version, and flags are all pinned in the Makefile or build config
- Instructions for performing a reproducibility verification are at docs/security/reproducible-builds.md
- Any change that breaks reproducibility is treated as a P1 bug

---

**Story 3.7.4 — Model release safety evaluation**

As an ML engineer, I want released toke models evaluated for safety and output toxicity before publication so that models do not generate harmful content when prompted outside their intended use case.

Acceptance criteria:
- Each model release is evaluated against a standard safety benchmark (LlamaGuard or equivalent) before publication
- The model is tested with adversarial prompts designed to elicit harmful code generation such as exploit patterns and malware structures
- Any model that fails safety evaluation is not published
- Safety evaluation results are documented and committed to docs/security/model-safety-evals.md
- The evaluation process is repeatable and documented so it can run on each subsequent model release

---

### EPIC 3.8 — Standard Library Production Hardening

*Bring the stdlib to production quality with the additional modules required for real-world application development and ecosystem completeness.*

**Sequencing note:** These modules and improvements are needed for Phase D and E corpus generation and for the ecosystem proof gate. They are not needed for Gate 2.

---

**Story 3.8.1 — std.log structured logging**

As a program author, I want a std.log module providing structured logging, so that toke applications produce machine-readable log output compatible with log aggregation systems.

Dependencies: 1.3.1 (std.str), 2.7.4 (std.time)

Acceptance criteria:
- log.info(msg:Str;fields:[[Str]]):bool is implemented
- log.warn(msg:Str;fields:[[Str]]):bool is implemented
- log.error(msg:Str;fields:[[Str]]):bool is implemented
- Output is newline-delimited JSON to stderr
- log level is configurable via environment variable TK_LOG_LEVEL
- Module compiles cleanly with tkc --check

---

**Story 3.8.2 — stdlib performance benchmarks**

As a compiler engineer, I want performance benchmarks for all Profile 1 stdlib modules so that regressions in stdlib performance are detected before releases.

Dependencies: Epic 1.3 complete

Acceptance criteria:
- Benchmark suite covers all six Profile 1 stdlib modules
- Benchmarks run as part of the release validation workflow
- Baseline performance figures are committed to benchmark/results/
- Any release that shows greater than 20% regression on a benchmark is blocked until the regression is explained or fixed
- Benchmark results include comparison against equivalent C implementations

---

**Story 3.8.3 — stdlib conformance test coverage**

As a compiler engineer, I want the stdlib test coverage to reach 100% of documented function signatures so that every stdlib function has a verified test before production use.

Dependencies: Epic 1.3 complete

Acceptance criteria:
- Every function in every stdlib module has at least one test covering the happy path
- Every fallible function has at least one test covering each error variant in its error type
- Tests run as part of the CI suite for the stdlib repository
- Coverage report is generated and committed to docs/

---

## Phase 4 — Standard Pathway (Months 26–32)

**Phase objective:** Formalise the language into a publishable standard. Build the conformance suite. Propose to an open consortium. Run the self-redesign pilot.

---

### EPIC 4.1 — Formal Specification

---

**Story 4.1.1 — Complete EBNF grammar publication**

As a language standardiser, I want the final EBNF grammar committed to the spec repository with machine-verified freedom from ambiguity, so that any implementer can build a conforming compiler from the grammar alone.

Acceptance criteria:
- Grammar covers all constructs including Profile 2 extensions
- ANTLR4 generation from the grammar produces no warnings or conflicts
- Grammar is versioned with a stable URL
- Grammar is accompanied by an annotated example for each production
- At least three independent readers have reviewed the grammar for errors

---

**Story 4.1.2 — Static semantics specification**

As a language standardiser, I want the type rules, name resolution rules, and arena lifetime rules formally documented in the semantics specification, so that implementers produce consistent behaviour without reference to the reference implementation.

Acceptance criteria:
- All type checking rules from Section 13.6 are stated as formal inference rules
- Arena escape rules are stated with examples of valid and invalid patterns
- Name resolution rules cover all scoping cases
- Specification is self-consistent: no rule contradicts another
- Specification is reviewed by at least one person with programming language theory background

---

**Story 4.1.3 — Error code registry publication**

As a repair loop engineer, I want the complete error code registry published as a stable document, so that automated repair systems can depend on error codes being stable across compiler versions.

Acceptance criteria:
- All E-series, W-series, and L-series codes are documented with: code, meaning, stage, example source that triggers it, fix field value where applicable
- Registry is published at a stable URL
- Registry version is tied to spec version
- Any future addition of error codes follows a documented process

---

### EPIC 4.2 — Conformance Suite

---

**Story 4.2.1 — Lexical and grammar conformance tests**

As a compiler implementer, I want a published suite of L-series and G-series conformance tests, so that any compiler can verify its lexical and parsing behaviour against the normative standard.

Acceptance criteria:
- At least 200 L-series tests covering all lexical rules
- At least 300 G-series tests covering all grammar productions and all rejection cases
- Each test specifies input, expected exit code, and expected error codes
- Tests are runnable against any conforming tkc binary
- Test results for the reference implementation are committed as expected outputs

---

**Story 4.2.2 — Semantic and type conformance tests**

As a compiler implementer, I want a published suite of N-series and T-series conformance tests, so that name resolution and type checking behaviour can be verified against the normative standard.

Acceptance criteria:
- At least 400 T-series tests covering all 10 type checking rules
- Tests include both valid programs (expected: success) and invalid programs (expected: specific error code)
- Each test specifies the expected error_code and expected fix field value where applicable
- Arena escape detection is covered by at least 50 dedicated tests
- Tests are runnable against any conforming tkc binary

---

**Story 4.2.3 — Diagnostic schema conformance tests**

As a repair loop engineer, I want a published suite of D-series tests that verify diagnostic schema conformance, so that automated repair systems can depend on diagnostic structure being correct.

Acceptance criteria:
- Every error code has at least one D-series test
- Each test verifies: error_code, stage, span_start, span_end, expected, got fields
- At least 50 tests verify fix field correctness
- Tests verify that fix field is absent (not null, not empty) when no mechanical fix exists
- Tests are runnable against any conforming tkc binary

---

### EPIC 4.3 — Tooling and Ecosystem

---

**Story 4.3.1 — Language server protocol stub**

As an IDE developer, I want a basic Language Server Protocol implementation for toke, so that syntax highlighting, error underlining, and go-to-definition work in editors that support LSP.

Acceptance criteria:
- LSP server implements: textDocument/didOpen, textDocument/didChange, textDocument/publishDiagnostics
- Syntax errors appear as diagnostics in the editor in real time
- Type errors appear as diagnostics after save
- Go-to-definition works for imported symbols
- Server is tested with VS Code and at least one other LSP-compatible editor

---

**Story 4.3.2 — Package registry prototype**

As a package author, I want a functioning package registry where toke modules can be published and consumed via the import declaration, so that the module ecosystem has a concrete implementation to reference.

Acceptance criteria:
- Registry accepts package submissions with module name, version, and source
- Import resolution can fetch modules from the registry by module path and version
- Registry enforces the tokelang package naming convention
- At least 10 stdlib extensions are published to the registry as examples
- Registry is self-hosted and not dependent on any external service

---

### EPIC 4.4 — Self-Redesign Pilot

---

**Story 4.4.1 — Error pattern corpus compilation**

As an ML researcher, I want a comprehensive dataset of the 12-month error pattern record from corpus generation, so that the model has the evidence base needed to propose language revisions.

Acceptance criteria:
- Error frequency data covers all 12 months of Phase 3 corpus generation
- Data includes: error code, frequency, context (surrounding source), fix applied, did the fix succeed
- Top 20 error patterns are identified with frequency counts and representative examples
- Data is formatted as a structured prompt suitable for model consumption
- Dataset is reviewed and verified against the raw compiler logs

---

**Story 4.4.2 — Language revision proposal generation**

As an ML researcher, I want the trained 32B model prompted to propose language construct revisions based on the error pattern data, so that the self-redesign loop has concrete proposals to evaluate.

Acceptance criteria:
- Model is prompted with the full error pattern dataset and a structured revision request
- Model produces at least 5 distinct revision proposals
- Each proposal includes: construct description, motivation, before/after example, expected error rate impact
- Proposals are evaluated for syntactic coherence and semantic correctness by a human reviewer
- At least 2 proposals are assessed as technically feasible

---

**Story 4.4.3 — Empirical proposal evaluation**

As a language designer, I want technically feasible proposals implemented in the compiler and evaluated empirically on a corpus subset, so that revision adoption decisions are based on measured outcomes rather than model assertions.

Acceptance criteria:
- Each feasible proposal is implemented as a compiler extension (behind a flag)
- A 10,000-entry corpus subset is translated to use the proposed construct
- Token count before and after is measured
- Error rate before and after is measured on a held-out task set
- Results for each proposal are committed to benchmark/results/self_redesign.json
- At least one proposal shows measurable improvement on both metrics

---

### EPIC 4.5 — Consortium Proposal

---

**Story 4.5.1 — Governance document**

As a standards advocate, I want a governance document defining how the toke specification is maintained, so that external contributors know how changes are proposed, reviewed, and adopted.

Acceptance criteria:
- Document defines: versioning scheme (MAJOR.MINOR.PATCH), change proposal process, review period requirements (30-day for minor, 7-day for patch), normative vs informative section policy
- Document defines membership and decision-making structure for the open consortium
- Document is compatible with joining an existing foundation (Linux Foundation, Apache Software Foundation, or equivalent)
- Document is reviewed by at least one person with open source governance experience

---

**Story 4.5.2 — Consortium proposal document**

As a project lead, I want a formal proposal document for submission to an open consortium or standards body, so that toke's governance transitions from a solo project to a community-owned standard.

Acceptance criteria:
- Proposal summarises the Gate 1 through 3 benchmark results
- Proposal describes the existing specification, conformance suite, and reference implementation
- Proposal identifies the target consortium and explains why it is appropriate
- Proposal identifies at least 3 external parties who have expressed interest in participation
- Proposal is reviewed by a lawyer familiar with open source IP before submission
- Proposal is submitted by the end of Month 32

### EPIC 4.6 — Security Audit and Hosted Service Readiness

*Complete an independent security audit of the compiler and establish SOC 2 readiness only if tokelang.dev operates as a hosted compilation service.*

**Note:** This epic contains a conditional story (4.6.2). Story 4.6.2 applies only if a hosted compilation API is launched. If no hosted service exists by Phase 4, 4.6.2 should be closed as not-applicable with a documented reason.

---

**Story 4.6.1 — Third-party security audit of the compiler**

As a project lead, I want an independent third-party security audit of the compiler conducted before the version 1.0 release so that the security posture is verified by an external expert before broad adoption.

Acceptance criteria:
- A qualified security firm with compiler and C expertise is engaged
- Audit scope covers: memory safety in the compiler frontend, input validation in the lexer and parser, diagnostic output injection risks, and FFI boundary safety
- All critical and high severity findings are remediated before release
- Medium findings are either remediated or carry a documented accepted-risk decision
- The audit report summary is published at docs/security/ (the full report need not be public)
- Remediation is verified by the auditing firm before final signoff

---

**Story 4.6.2 — SOC 2 readiness assessment (conditional on hosted service)**

PRECONDITION: This story is activated only if tokelang.dev operates as a hosted compilation service. If no hosted service is launched, close this story as not-applicable and document the reason.

As a project lead, I want a SOC 2 Type I readiness assessment completed if tokelang.dev operates as a hosted compilation service, so that enterprise customers can request evidence of security controls.

Acceptance criteria:
- This story is marked not-applicable and closed if no hosted service is launched by the end of Phase 4
- If a hosted service is launched: a readiness assessment against SOC 2 Trust Service Criteria is conducted by a qualified assessor
- Gaps between current controls and SOC 2 requirements are documented
- A remediation roadmap is produced
- Access logging, change management audit trail, and incident response procedures are in place and evidenced
- A SOC 2 Type II audit is planned for 12 months after Type I attestation

---

---

## Cross-Phase — Project Website and Developer Experience

---

### EPIC 5.1 — Project Website (tokelang.dev)

*Build the public-facing project website at tokelang.dev. The site serves as the entry point for developers, enterprises, and contributors. It hosts documentation, a language tutorial, API reference, and an interactive translator/comparison tool. Repository: toke-web.*

---

**Story 5.1.1 — Site scaffold and landing page**

As a visitor to tokelang.dev, I want a clean, well-designed landing page that explains what toke is and why it exists, so that I can decide within 30 seconds whether the project is relevant to me.

Acceptance criteria:
- Static site scaffold (Next.js, Astro, or similar SSG) committed to `toke-web` repository
- Landing page with hero section: tagline, 1-sentence value proposition, CTA to "Get Started"
- "Why toke?" section: problem statement (LLM token inefficiency for code), thesis (purpose-built language = fewer tokens = faster/cheaper inference), key benefit bullets
- Visual showing token count comparison (toke vs Python for equivalent program)
- Links to all 7 project repositories with brief descriptions
- Responsive design (mobile + desktop)
- Footer: MIT licence, GitHub link, community links
- Deployed to tokelang.dev (Vercel/Netlify)

---

**Story 5.1.2 — About page and project philosophy**

As a potential contributor or enterprise evaluator, I want an about page that explains the project's goals, design philosophy, and open-source commitment, so that I understand the project's vision and governance.

Acceptance criteria:
- About page with sections: Vision, Problem Statement, Approach, Design Principles, Open Source Commitment
- Problem statement: LLMs waste tokens on verbose syntax, toke compresses code 3-5x vs Python while remaining human-writable
- Design principles: Profile 1 (80 chars), LL(1) grammar, 12 keywords, everything is explicit
- Open source section: MIT licence, contribution welcome, enterprise use encouraged
- Enterprise section: toke is designed for companies building their own AI coding systems — lower inference cost, faster generation, deterministic compilation
- Community section: how to participate, where to ask questions, contribution guidelines link

---

**Story 5.1.3 — API specification browser**

As a developer learning toke, I want a searchable API specification document covering the full language and standard library, so that I can quickly find syntax, types, and function signatures without reading raw spec files.

Acceptance criteria:
- API reference section generated from spec/stdlib-signatures.md and spec/grammar.ebnf
- Searchable: text search filters results in real-time
- Navigation: sidebar with module list, anchor links to each function
- Each function entry shows: signature, parameters with types, return type, error type (if any), 1-2 usage examples
- Type system reference: all built-in types (i64, f64, u64, bool, Str, void, [T], [K:V], Task, *T, error unions)
- Grammar reference: production rules with syntax highlighting
- Error code reference: all E/W codes with description and fix guidance
- Mobile-friendly layout

---

**Story 5.1.4 — Getting Started guide**

As a new developer, I want a step-by-step getting started guide that takes me from zero to a running toke program, so that I can evaluate the language hands-on.

Acceptance criteria:
- Installation section: build tkc from source (clone, make, verify with `tkc --version`)
- "Hello World" walkthrough: write a minimal module, compile, run
- Language tour covering: modules (M=), functions (F=), let bindings, types (i64, f64, Str, bool), if/else, loops, arrays, error handling (try/match)
- Standard library quickstart: file I/O, string operations, JSON parsing
- Project structure: how to organize multi-module programs, import syntax
- Each section has copy-pasteable code blocks with expected output
- Estimated completion time: 30 minutes

---

**Story 5.1.5 — Human training course**

As a developer who wants to learn toke deeply, I want a structured multi-lesson training course that builds knowledge progressively, so that I can become proficient in the language over a weekend.

Acceptance criteria:
- Course structure: 8-10 lessons, each 20-30 minutes
- Lesson 1: Why toke exists, design philosophy, token efficiency thesis
- Lesson 2: Modules, functions, basic types, return values
- Lesson 3: Control flow (if/else, match, loops), expressions vs statements
- Lesson 4: Arrays, maps, collection operations, .len
- Lesson 5: Error handling (error types, try, match on errors, propagation)
- Lesson 6: String handling, string literals, standard library (std.str, std.json)
- Lesson 7: Modules and imports, interface files, multi-file projects
- Lesson 8: Advanced topics (FFI, async/spawn/await, pointers, extern functions)
- Lesson 9: Standard library deep dive (std.http, std.db, std.file, std.crypto)
- Lesson 10: Real-world project — build a complete application in toke
- Each lesson has: explanation text, code examples, exercises with solutions
- Progress tracking (lesson completion checkmarks)
- Hosted at tokelang.dev/learn

---

**Story 5.1.6 — Web-based translator and comparison tool**

As a developer evaluating toke, I want an interactive web tool that translates code between toke and common languages, comparing token counts and performance, so that I can see the efficiency gains first-hand.

Acceptance criteria:
- Interactive split-pane editor: toke on left, target language on right
- Supported target languages: Python, JavaScript, Go, Rust, C (at minimum)
- Translation direction: toke-to-target and target-to-toke
- Token count display for both panes: shows token count using cl100k_base (GPT-4 tokenizer) and the toke BPE tokenizer
- Compression ratio displayed prominently (e.g., "toke: 47 tokens, Python: 156 tokens — 3.3x more efficient")
- Performance metrics panel: estimated inference cost savings at current API pricing
- Pre-loaded example programs (at least 10 curated examples covering different patterns)
- Syntax highlighting for all supported languages
- Client-side tokenization (WASM SentencePiece or API endpoint)
- Share button: generates permalink to current code pair
- Mobile-responsive layout

---

**Story 5.1.7 — Community and contribution hub**

As an open-source contributor, I want a community page that makes it easy to find contribution opportunities and connect with other toke developers, so that I can start contributing quickly.

Acceptance criteria:
- Community page with: "Good first issues" feed (pulled from GitHub), contributor guide summary, code of conduct link
- Links to all repos with their current status and contribution areas
- "For Enterprise" section: how companies can adopt toke for their AI coding pipelines, integration guidance, support channels
- Discussion forum link (GitHub Discussions or similar)
- Newsletter/updates signup (optional)
- Contributor recognition: list of contributors from git history

---

**Story 5.1.8 — Site CI/CD and deployment**

As a maintainer, I want automated build, test, and deployment for the website, so that content updates go live automatically on merge to main.

Acceptance criteria:
- GitHub Actions workflow: build → test → deploy on push to main
- Preview deployments on pull requests
- Lighthouse CI: performance score >= 90, accessibility score >= 90
- Link checker: no broken internal or external links
- Build time under 60 seconds
- Deployed to tokelang.dev with HTTPS

---

### EPIC 5.2 — Phase 2 Character Reduction Documentation

*Update all public-facing documentation and learning materials to cover the Phase 2 (56-character) reduced character set and the purpose-built tokenizer that uses it.*

---

**Story 5.2.1 — Update website and learning materials for Phase 2 character reduction**

As a developer learning toke, I want documentation explaining the Phase 2 (56-character) profile, so that I understand what characters are removed, why, how the purpose-built tokenizer uses the reduced set, and how to translate between Phase 1 and Phase 2.

Acceptance criteria:
- Dedicated documentation page explaining the Phase 2 (56-character) profile
- Clear explanation of which characters are removed from the 80-character Phase 1 set and the rationale for each removal
- Description of how the purpose-built tokenizer leverages the reduced character set for efficiency
- Transformation rules from Phase 1 to Phase 2 documented with examples
- Performance implications explained (token count reduction, inference cost impact)
- Getting Started guide updated to reference Phase 2 where appropriate
- Learning course updated to include Phase 2 references where appropriate
- API/reference section updated to include Phase 2 character set and transformation rules
- Depends on: Phase 2 tokenizer work

---

### EPIC 6.1 — Publish to Hugging Face

*Package and publish the fine-tuned toke model, tokenizer, benchmarks, and a live demo to Hugging Face so the community can evaluate, reproduce, and build on the work.*

---

**Story 6.1.1 — Create Hugging Face organisation and model card**

As a project maintainer, I want a karwalski organisation on Hugging Face with a complete model card, so that the model is discoverable and its capabilities, limitations, and training data are transparently documented.

Acceptance criteria:
- karwalski organisation created on Hugging Face
- Model card includes: license, intended use, out-of-scope use, training data description, evaluation metrics
- Model card follows Hugging Face model card template best practices
- Links to project website (tokelang.dev) and source repos
- Depends on: model training complete (Epic 1.5/1.6)

---

**Story 6.1.2 — Upload model weights and tokenizer**

As a researcher, I want the fine-tuned model weights, tokenizer files, and config uploaded to Hugging Face Hub, so that I can download and run the toke model locally.

Acceptance criteria:
- Fine-tuned model weights uploaded to Hugging Face Hub under karwalski org
- Tokenizer files (vocab, merges, config) uploaded alongside the model
- Model config (architecture, hyperparameters) included
- Quantized variants (GGUF, AWQ) uploaded if applicable
- README/model card updated with download and usage instructions
- Depends on: 6.1.1

---

**Story 6.1.3 — Publish benchmark results and evaluation dataset**

As a researcher, I want the held-out benchmark task set, evaluation scripts, and published results available in the model repo, so that I can reproduce the evaluation and compare against other models.

Acceptance criteria:
- Held-out benchmark task set uploaded to the model repo
- Evaluation scripts uploaded with clear usage instructions
- Published results (token efficiency, pass@1, compression ratios) included
- Reproducibility instructions: environment, dependencies, exact commands to replicate results
- Depends on: 6.1.2, Gate 1 pass

---

**Story 6.1.4 — Inference API and demo space**

As a developer evaluating toke, I want a Hugging Face Space with a simple demo where I can paste a task description and see generated toke code with token count comparison, so that I can experience the efficiency gains interactively.

Acceptance criteria:
- Hugging Face Space created under karwalski org
- Demo interface: text input for task description, generated toke code output
- Token count comparison displayed: toke tokens vs equivalent Python/JS tokens
- Compression ratio prominently shown
- Space runs without requiring local setup (hosted inference)
- Depends on: 6.1.2

---

### EPIC 6.2 — Repository Scaffolding

*Create planned repositories that are referenced in documentation but do not yet exist.*

---

**Story 6.2.1 — Create toke-eval repository**

As a contributor, I want the toke-eval repository to exist with a proper README, license, and scaffold, so that evaluation pipeline work can begin and website links are not dead.

Acceptance criteria:
- Repository created at github.com/karwalski/toke-eval
- README with purpose, planned scope, and dependency note (depends on model + benchmark)
- Apache 2.0 license
- Basic directory structure for evaluation scripts
- Website references updated: remove "(planned)" labels from repos.md and contributing.md
- Depends on: no hard dependency, but meaningful work requires model training

---

**Story 6.2.2 — Create toke-model repository scaffold**

As a contributor, I want the toke-model repository scaffolded with README, license, and directory structure, so that model training work has a home when the corpus pipeline is ready.

Acceptance criteria:
- Repository created at github.com/karwalski/toke-model (currently exists locally but not on GitHub)
- README with purpose, planned scope, and dependency note
- Apache 2.0 license
- Directory structure for training scripts, configs, and evaluation
- Website references updated: remove "(planned)" labels
- Depends on: no hard dependency, but meaningful work requires corpus completion

---

### EPIC 7.1 — Website Code Example Correctness

*Fix all code examples on tokelang.dev that do not compile against the reference compiler. Bugs are split into compiler-side fixes and website-side documentation fixes.*

---

**Story 7.1.1 — Rewrite website examples to remove underscores from identifiers**

As a learner, I want all code examples on the website to use valid toke identifiers (no underscores), so that I can copy examples and have them compile.

Acceptance criteria:
- All snake_case identifiers across all website code examples replaced with camelCase or concatenated names (e.g. `find_first_negative` → `findFirstNeg`, `rect_area` → `rectArea`)
- Naming is consistent across lessons (same function keeps same name everywhere it appears)
- All 28+ affected files updated
- Website builds clean after changes

---

**Story 7.1.2 — Fix match arm return syntax in website examples**

As a learner, I want match expression examples to use correct syntax, so that the patterns I learn actually work.

Acceptance criteria:
- All match arms updated: `<` return goes before the entire match expression, not inside individual arms
- Pattern: `<expr|{Variant:binding body}` where arms contain bare expressions, not `<expr`
- All affected examples in lessons 03, 05, 07, and tour.md corrected
- At least 3 examples verified against `tkc --check`

---

**Story 7.1.3 — Fix error variant construction and propagation syntax in website examples**

As a learner, I want error handling examples to use correct syntax, so that I can build real programs with error types.

Acceptance criteria:
- Variant construction: determine and document correct syntax (replace `Type.Variant(payload)`)
- Error propagation: change `expr!ErrType.Variant` to `expr!ErrType` (no variant suffix)
- All affected examples in lessons 05, 06, 07, 09, 10, and tour.md corrected
- At least 3 examples verified against `tkc --check`

---

**Story 7.1.4 — Fix typed empty collection literals in website examples**

As a learner, I want collection literal examples to use correct syntax for empty arrays and maps.

Acceptance criteria:
- Replace `[Type][]` with correct empty literal syntax throughout website
- Replace `[K:V][]` with correct empty map literal syntax
- All affected examples in lessons 04, 06, 10 corrected
- Verified against `tkc --check`

---

**Story 7.1.5 — Fix loop init and spawn/await syntax in website examples**

As a learner, I want loop and async examples to use correct syntax.

Acceptance criteria:
- All `lp(i=0;...)` changed to `lp(let i=0;...)` in tour.md
- All `spawn func(args)` / `await task` changed to `spawn(func)` / `await(task)` in lesson 08
- Arena block examples in lesson 08 marked as "Phase 2 — not yet implemented"
- Verified against `tkc --check` where applicable

---

**Story 7.1.6 — Implement array indexing in compiler**

As a developer, I want `arr[i]` array element access to work in the compiler, so that the most basic collection operation is supported.

Acceptance criteria:
- Parser handles `expr[expr]` as a postfix index operation
- Type checker validates index is integer type, base is array type
- Returns element type of the array
- LLVM backend emits correct index access
- Conformance tests added
- All existing 79 tests still pass

---

**Story 7.1.7 — Implement void return type in compiler**

As a developer, I want `void` to be a recognized return type, so that functions with side effects can be properly typed.

Acceptance criteria:
- `void` recognized as a valid type in return position
- Functions with `:void` return type cannot use `<expr` (only bare `<` or implicit return)
- Type checker enforces void functions don't return values
- Conformance tests added
- All existing 79 tests still pass

---

**Story 7.1.8 — Fix struct literal with field access expression crash**

As a developer, I want struct literals containing field access expressions (e.g. `Pt{x:a.x+b.x}`) to compile without crashing.

Acceptance criteria:
- Compiler does not crash on struct literals with member access in field values
- Proper diagnostic emitted if the expression is invalid (no silent exit)
- Conformance test added for this case
- All existing 79 tests still pass

---

**Story 7.1.9 — Website code example conformance test suite**

As a maintainer, I want an automated test that extracts all code examples from the website and runs them through `tkc --check`, so that documentation drift is caught before deployment.

Acceptance criteria:
- Script extracts all toke code blocks from website markdown/mdx files
- Complete programs (with `M=`) tested directly; fragments wrapped in valid scaffold
- Examples that use stdlib imports tested with `--check` (expected failures noted)
- Test runs in CI alongside website build
- Report lists pass/fail per example with file and line reference
- Depends on: 7.1.1–7.1.8 (all syntax fixes complete first)

---

### EPIC 7.2 — File Extension Review

*Review the `.tk` file extension decision and decide whether to keep, change, or alias it. The extension appears throughout the compiler, spec, test harness, and documentation.*

---

**Story 7.2.1 — Review `.tk` file extension decision**

As a language designer, I want to review the decision to use `.tk` as the source file extension, so that any conflicts or concerns (e.g. Tcl/Tk association, Tokelau TLD overlap, tooling detection) are identified and resolved before the ecosystem hardens around the convention.

Acceptance criteria:
- Audit all uses of `.tk` across repos: compiler (`run_e2e.sh`, `run_conform.sh`), spec (`language-proposal-v03.md`, `semantics.md`), website docs, and corpus tooling
- Document the original rationale for `.tk` (from the language proposal)
- List known conflicts: Tcl/Tk `.tk` files, GitHub Linguist classification, editor syntax detection
- Evaluate alternatives (e.g. `.toke`, `.tke`) against token-efficiency goals and ecosystem friction
- Decision recorded in spec with rationale — either reaffirm `.tk` or define a migration path
- If changed: update compiler, test harness, spec, website, and corpus tooling in a follow-up story

---

### EPIC 7.3 — Stub File Audit

*Identify empty stub files across all repos that may have been marked as complete but contain no implementation, and create stories for any unblocked development work.*

---

**Story 7.3.1 — Audit empty stub files across all repos**

As a project maintainer, I want to identify all empty or placeholder files across toke repos that were scaffolded but never implemented, so that I can accurately track what work remains and avoid false confidence in project completeness.

Acceptance criteria:
- Scan all 8 repos for empty files (0 bytes) and stub files (only imports/boilerplate, no logic)
- Cross-reference against progress.md — flag any files associated with stories marked `done`
- Cross-reference against epics_and_stories.md — identify unblocked work that depends on these stubs
- Produce a report: file path, associated story (if any), current story status, whether work is unblocked
- Create follow-up stories for any implementation work that is unblocked
- Update progress.md to correct any inaccurate story statuses

---

### EPIC 8.1 — Cloud Corpus Phase A Generation

*Build and run the corpus generation pipeline on cloud compute using multi-provider LLM APIs. Generates 50,000 Phase A programs with differential testing across 4 languages, multi-model capability trials, and tiered workload allocation. See `toke-corpus/docs/cloud-corpus-proposal.md` for the full proposal.*

---

**Story 8.1.1 — Provision cloud instance and deploy toolchain**

As an infrastructure engineer, I want a cloud compute instance provisioned with all required compilers and runtimes, so that the corpus pipeline can validate programs without external dependencies.

Acceptance criteria:
- EC2 or Lightsail instance provisioned: 8 vCPU, 16 GB RAM, 80 GB SSD, Ubuntu 24.04
- `tkc` cross-compiled for Linux amd64 and deployed to instance (or built from source)
- `gcc 13+`, `python 3.11+`, `openjdk 21+` installed and verified
- Python venv created with pipeline dependencies (`httpx`, `asyncio`, `tiktoken`, etc.)
- SSH access configured and tested
- Instance can compile and run a toke program end-to-end
- No IP addresses, keys, or credentials committed to any repo

---

**Story 8.1.2 — Multi-provider API client layer**

As a pipeline developer, I want a unified API client layer that dispatches generation requests to multiple LLM providers, so that the orchestrator can route tasks without provider-specific logic.

Acceptance criteria:
- Abstract base class `ProviderClient` with: `generate(prompt, system) -> str`, `generate_batch(prompts) -> list[str]`, `name`, `tier`, `cost_per_input_mtok`, `cost_per_output_mtok`
- Concrete implementations for: Anthropic (batch API), OpenAI (batch API), Google Gemini, DeepSeek
- Each client handles: authentication, retries (exp backoff), rate limiting, cost tracking per call
- Prompt caching / context caching used where provider supports it
- All API keys read from environment variables — never hardcoded
- Unit tests with mocked API responses for each provider
- Cost tracking: each call records input/output tokens and running cost

---

**Story 8.1.3 — Task curriculum generator**

As a corpus engineer, I want the curriculum generator to produce 50,000 Phase A task specifications from templates, so that the generation pipeline has a deterministic, reproducible input.

Acceptance criteria:
- Generates tasks across all 6 Phase A categories (A-MTH, A-CND, A-STR, A-ARR, A-SRT, A-ERR)
- Each task has: `task_id`, `category`, `description`, `expected_signature`, `difficulty_tier`
- Tasks are deterministically seeded (same seed = same curriculum)
- Variant parameters ensure coverage of all type combinations per category
- Distribution: ~8,000–10,000 tasks per category (configurable)
- Output: JSON file with all 50K task specs
- Unit tests verify category distribution, uniqueness, and schema conformance

---

**Story 8.1.4 — Generation prompt templates**

As a corpus engineer, I want well-tested prompt templates for toke generation, reference language generation, and test input generation, so that models produce correctly structured output.

Acceptance criteria:
- System prompt includes: toke spec excerpt (grammar, types, error model), 5+ worked examples per category
- Generation prompt template: takes task spec, returns toke source code
- Reference language prompts: Python, C, Java — each with language-specific constraints
- Test input generation prompt: produces 5+ test cases per task with expected outputs
- Correction prompt template: includes compiler diagnostic, expected output, grammar reference
- All prompts are markdown files in `prompts/` directory (not hardcoded in Python)
- Prompts tested against at least 2 models to verify structured output compliance
- System prompt fits within 4,000 tokens (optimized for caching efficiency)

---

**Story 8.1.5 — Model capability trial framework**

As a corpus engineer, I want to run a 500-task capability trial across all candidate models and automatically score/rank them, so that I can allocate the workload to the best-performing models.

Acceptance criteria:
- Trial runner sends 500 tasks (covering all 6 categories) to each candidate model
- Each response validated: `tkc --check`, reference compilation, differential test
- Scoring rubric: first-pass compile rate (40%), differential agreement (30%), correction success (20%), cost efficiency (10%)
- Models scoring below 50% first-pass compile rate are automatically dropped
- Produces model scorecard: per-model and per-category scores, cost per accepted program
- Scorecard saved to `metrics/trial_results.json`
- Allocation recommendation output: which models get what % of workload
- Can be re-run with different prompt variants to iterate on prompt quality

---

**Story 8.1.6 — Model pool manager and task router**

As a pipeline developer, I want a pool manager that routes tasks to models based on trial results and category difficulty, so that the workload is distributed optimally across tiers.

Acceptance criteria:
- Pool config: list of surviving models with tier, allocation %, and category preferences
- Task router: assigns each task to a model based on category, tier allocation, and current progress
- Enforces 40% minimum Tier 2 allocation (30% primary + 10% escalation reserve)
- Category-aware routing: harder categories (A-ERR, A-SRT) get more Tier 2 allocation
- Escalation logic: Tier 1 failure → Tier 2 primary → Tier 2 alternate → Sonnet (last resort)
- Progress tracking: tasks dispatched, pending, accepted, failed, escalated per model
- Automatic rebalancing: if a model's failure rate exceeds 50% mid-run, shift its allocation

---

**Story 8.1.7 — Validation pipeline (compiler + differential test + quality)**

As a corpus engineer, I want an automated validation pipeline that compiles, runs, and scores every generated program, so that only correct programs enter the corpus.

Acceptance criteria:
- Compiler validation: `tkc --check` for toke; `gcc`, `python3 -c`, `javac` for references
- Execution: run all 4 binaries against shared test inputs, capture stdout + exit code
- Majority vote: ≥3 of 4 must agree on output; disagreements logged with full output
- Quality scoring: token efficiency (tk vs python), holdout isolation check, schema conformance
- Embedding deduplication: cosine similarity >0.95 with existing entry → reject
- 8 parallel validation workers (configurable)
- Each validation result written to `logs/` with full trace (compilation output, test results, vote)
- Validation can run independently of generation (process a directory of pending programs)

---

**Story 8.1.8 — Correction loop and escalation engine**

As a corpus engineer, I want failed programs to be automatically resubmitted with structured error feedback, so that correctable failures don't waste task slots.

Acceptance criteria:
- Failed programs get structured prompt: compiler diagnostic JSON + expected output + grammar reference
- Max 3 correction attempts per task per tier
- Escalation chain: Tier 1 model → Tier 2 primary → Tier 2 alternate → discard + replace
- Replacement: discarded tasks replaced with new task specs from the curriculum reserve
- Cost tracking per correction attempt
- Metrics: correction success rate by model, by category, by attempt number
- Configurable: max attempts, escalation thresholds, replacement behaviour

---

**Story 8.1.9 — Corpus writer and metrics dashboard**

As a project maintainer, I want validated corpus entries written to disk per the normative schema with real-time progress and cost metrics, so that I can monitor the run and verify the output.

Acceptance criteria:
- Accepted entries written as JSON to `corpus/phase_a/` conforming to `corpus/schema.json`
- Each entry includes: `id`, `version`, `phase`, `task_id`, `tk_source`, `tk_tokens`, `model`, `attempts`, `validation`, `differential`, `judge`
- `judge.score` computed from validation metrics (not a separate LLM call for Phase A)
- Real-time metrics file (`metrics/progress.json`) updated every 60 seconds:
  - Total tasks: dispatched, pending, accepted, failed, escalated
  - Per-model: tasks, acceptance rate, cost
  - Per-category: tasks, acceptance rate
  - Total cost (API + compute)
  - Estimated time remaining
- Human-readable progress line printed to stdout every 5 minutes
- Checkpoint file enables restart from last completed batch on failure

---

**Story 8.1.10 — Orchestrator main loop and end-to-end integration**

As a corpus engineer, I want a single entry point that runs the complete pipeline from curriculum generation through corpus packaging, so that the full run can execute unattended.

Acceptance criteria:
- `python main.py --phase A --tasks 50000 --config config.yaml` runs the full pipeline
- Config file specifies: API keys (env var references), model pool, tier allocations, thresholds
- Pipeline stages: curriculum → trial (optional, `--skip-trial`) → dispatch → validate → correct → write
- Graceful shutdown: Ctrl+C saves checkpoint, in-flight tasks marked pending
- Resume: `--resume` flag picks up from last checkpoint
- End-of-run report: total programs, per-category breakdown, cost breakdown, model scorecard, quality metrics
- Dry-run mode: `--dry-run` generates curriculum + prompts but makes no API calls
- Integration test: end-to-end with mocked APIs, 10 tasks, all stages exercised

---

---

### EPIC 2.10 — Compiler Hardening

---

**Story 2.10.1 — Populate conformance test suites for type checking, name resolution, and codegen (T/N/C-series)**

As a compiler engineer, I want the T-series (type checking), N-series (name resolution), and C-series (code generation) conformance test categories populated with test cases, so that regressions in these compiler phases are caught automatically.

Dependencies: 1.2.10 (conformance test framework exists)

Background: Story 1.2.10 established the conformance test framework with L-series (25 lexical tests), G-series (25 grammar tests), and D-series (12 diagnostic tests). The T-series, N-series, and C-series directories exist but contain no test files. The 46,754-entry corpus provides implicit coverage, but structured regression tests are needed to catch targeted regressions during Phase 2 compiler changes.

Acceptance criteria:
- At least 30 T-series tests covering: type compatibility for all primitive types (i64, u64, f64, bool, Str), array element type checking, struct field type validation, error union type checking, function return type verification, binary operator type rules
- At least 15 N-series tests covering: undefined variable detection, duplicate name detection, scope shadowing, cross-module name resolution, struct/type name resolution
- At least 15 C-series tests covering: arithmetic codegen (all binary ops), function call with parameters, let binding initialization, if/else branching, loop iteration, return values, string literals, array indexing, struct construction
- All new tests pass with current compiler (`make conform`)
- CI runs expanded test suite on every commit

---

**Story 2.10.2 — Scalar width validation in type checker**

As a compiler engineer, I want the type checker to validate scalar width constraints, so that integer overflow and narrowing conversions are caught at compile time rather than producing silent runtime bugs.

Dependencies: 1.2.5 (type checker)

Background: The type checker accepts all integer literal values without checking whether they fit in the target scalar type. For example, assigning a value exceeding i64 range to an i64 binding produces no diagnostic. This is acceptable for Phase 1 corpus generation (the corpus uses reasonable values), but must be addressed before Phase 2 end-to-end compilation produces binaries.

Acceptance criteria:
- Integer literal overflow detection: literals exceeding i64 range (signed) or u64 range (unsigned) produce a diagnostic
- Narrowing cast warnings: `as` casts from wider to narrower types produce a warning diagnostic
- At least 10 T-series tests covering width validation edge cases
- No false positives on existing 46,754 corpus entries (validate by running `tkc --check` on a sample)
- Existing conformance tests still pass

---

---

### EPIC 2.11 — Spec Proposals (Phase 2 Candidates)

---

**Story 2.11.1 — Externalized string table with placeholder syntax**

As a language designer, I want toke source to reference string literals via placeholders that resolve against an external string table, so that verbose multilingual text stays outside the token-optimized source and can support arbitrary character sets without inflating the tokenizer vocabulary.

Dependencies: Phase 2 spec finalization

Background: toke's Profile 1 character set is 80 ASCII chars, optimised for token density. Inline string literals (`"hello"`) work but embed natural-language text directly in source, which (a) wastes BPE vocabulary on natural-language tokens that don't recur across programs, (b) cannot represent non-ASCII characters (CJK, Arabic, emoji, etc.) without a Profile 2 charset expansion, and (c) mixes i18n concerns with logic.

Proposed design (for researcher review — not final):
- Placeholder syntax in source: `$1`, `$2`, ... or `$key` references
- Companion string table (JSON or similar) maps placeholders to values, optionally per-locale
- Compiler resolves placeholders at compile time (static) or runtime links a string table (dynamic)
- Alternative: implement as stdlib (`std.i18n.get(key;locale)`) rather than language syntax, keeping spec simpler

Trade-offs to evaluate:
- Language-level syntax is more token-efficient but adds complexity to the spec
- stdlib approach is simpler but still requires string literals for keys
- Could coexist with inline strings — placeholders are opt-in for i18n / verbose text
- Impacts tokenizer vocabulary: with externalized strings, BPE vocab is purely code patterns

Acceptance criteria (for proposal, not implementation):
- Written spec proposal with syntax options, trade-off analysis, and tokenizer impact estimate
- Included in Phase 2 spec review package for researcher feedback
- Decision documented: language-level vs stdlib vs hybrid approach

---

*These stories and epics cover all 32 months of the toke project plan, plus cross-phase initiatives. Each epic maps to a section of the RFC and the project plan. Story completion is the acceptance criterion for milestone delivery. Gate stories are the single most important stories in each phase: if a gate story's acceptance criteria are not met, the phase does not proceed.*
