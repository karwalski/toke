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

As a compiler engineer, I want a complete formal EBNF grammar for toke Phase 1, so that the parser is built from a single authoritative document and conformance tests can be mechanically generated from it.

Acceptance criteria:
- Grammar covers all constructs: module, import, type, constant, function, all statement forms, all expression forms, all literal forms, all type expression forms
- Grammar is LL(1): no production requires more than one token of lookahead
- Grammar is unambiguous: validated by an automated parser generator with zero conflict warnings
- Grammar is committed to the spec/grammar.ebnf file in the repository
- An ANTLR4 grammar generated from the EBNF produces a working parser for all example programs

---

**Story 1.1.5 — Phase 2 transformation rules**

As a tooling developer, I want the Phase 1 to Phase 2 transformation rules documented precisely, so that a mechanical translator can convert any valid Phase 1 program to a valid Phase 2 program without human judgment.

Acceptance criteria:
- Uppercase type name to $ sigil rule is formally stated with no exceptions
- Array literal [] to @() rule is formally stated with no exceptions
- Array index a[n] to a.get(n) rule is formally stated
- Transformation is deterministic: one Phase 1 program maps to exactly one Phase 2 program
- A round-trip test exists: Phase 1 → Phase 2 → Phase 1 produces the original program
- At least 20 example pairs (Phase 1, Phase 2) are included in the spec

---

**Story 1.1.6 — Spec review and alignment**

As a compiler engineer, I want all Phase 1 spec documents reviewed for internal consistency and completeness before compiler work begins, so that the risk of mid-implementation spec changes is minimised.

Acceptance criteria:
- All five spec documents reviewed: character-set.md, keywords.md, symbol-disambiguation.md, grammar.ebnf, phase2-transform.md
- Every production in grammar.ebnf references only terminals defined in character-set.md and keywords.md
- Every disambiguation rule in symbol-disambiguation.md is present and correctly encoded in grammar.ebnf
- Every Phase 2 transformation in phase2-transform.md references valid grammar non-terminals from grammar.ebnf
- No contradictions between any two spec documents
- Any gap, ambiguity, or risk item that would require a compiler change post-implementation is documented
- A review report is committed to toke-spec/docs/spec-review-m0.md
- Any identified issues are either resolved in the spec or logged as a numbered risk item in the report

---

### EPIC 1.2 — Reference Compiler Frontend

*Build the minimal compiler frontend in C: lexer, parser, name resolver, and type checker. Zero external dependencies. Structured error output from the first line of error-emitting code.*

---

**Story 1.2.1 — Lexer implementation**

As a compiler engineer, I want a C lexer that accepts a Phase 1 source file and produces a flat token stream with no whitespace tokens, so that the parser receives a clean token sequence independent of formatting.

Acceptance criteria:
- Lexer correctly classifies all 80 Phase 1 characters into their token classes
- Whitespace between tokens is silently consumed and produces no tokens
- Two programs differing only in whitespace between tokens produce identical token streams
- String literal content is captured verbatim including escape sequences
- Invalid escape sequences produce a structured E1001 diagnostic and halt
- Unterminated string literals produce a structured E1002 diagnostic and halt
- Characters outside the Phase 1 set in structural positions produce E1003
- Lexer processes a 200-token source file in under 1ms on reference hardware
- Lexer source is under 300 lines of C

---

**Story 1.2.2 — Parser implementation**

As a compiler engineer, I want a C parser that accepts the token stream from the lexer and produces an AST, so that all downstream phases work from a single structured representation.

Acceptance criteria:
- Parser implements the complete Phase 1 EBNF grammar with no extensions
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
- Resolver loads .tokei interface files for all declared imports
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

As a repair loop engineer, I want every compiler error to be emitted as a JSON-schema-conforming diagnostic record, so that automated repair systems can consume errors without parsing English prose. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- All diagnostics conform to the schema defined in Section 9.1 of the spec
- schema_version is "1.0" for all Phase 1 diagnostics
- fix field is populated for all mechanically derivable corrections
- fix field is omitted (not null, not empty string) when no mechanical fix exists
- Diagnostics are emitted to stderr, one JSON object per line
- Human-readable text output is available via --diag-text flag
- JSON output is the default when stdout is not a terminal
- Every error code in the E1xxx through E5xxx ranges has at least one test case

---

**Story 1.2.7 — Interface file emitter**

As a build system developer, I want the compiler to emit a .tokei interface file for each compiled module, so that dependent modules can be type-checked against the interface without recompiling the source.

Acceptance criteria:
- .tokei file is emitted alongside the binary for every successful compilation
- .tokei file contains module path, exported symbol names, kinds, and signatures
- .tokei file is machine-readable (JSON or equivalent)
- A module can be type-checked using only its dependencies' .tokei files
- .tokei files support incremental compilation: changing an implementation file that does not change exported signatures does not require recompilation of dependents

---

**Story 1.2.8 — LLVM IR backend**

As a compiler engineer, I want a backend that lowers toke IR to LLVM IR and invokes LLVM to produce a native binary, so that toke programs run as self-contained native executables with no runtime dependency.

Acceptance criteria:
- LLVM IR is generated for all constructs defined in the Phase 1 grammar
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
- --emit-interface flag triggers .tokei emission
- --check flag runs type checking only without code generation
- --phase1 and --phase2 flags select character set profile
- --diag-json and --diag-text flags select diagnostic format
- Exit codes are: 0 (success), 1 (compilation error), 2 (internal error), 3 (usage error)
- --version prints compiler version and exits 0

---

**Story 1.2.10 — Conformance test suite (Phase 1)**

As a compiler engineer, I want a conformance test suite covering all Phase 1 language features, so that any change to the compiler can be validated against expected behaviour for every construct.

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
- .tokei interface file is emitted and accurate

---

**Story 1.3.2 — std.http interface**

As a corpus engineer, I want the std.http module available with route registration working end-to-end, so that Phase C corpus programs can generate working HTTP handlers.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions listed in Section 17.2 of the spec are defined
- http.GET, http.POST, http.PUT, http.DELETE, http.PATCH route registration compiles
- Route pattern parameters (:id, :name) are parsed and accessible via req.param()
- A minimal HTTP server accepting a request and returning a response runs end-to-end
- .tokei interface file is emitted and accurate

---

**Story 1.3.3 — std.db interface**

As a corpus engineer, I want the std.db module available with query functions typed correctly, so that database access patterns can appear in corpus programs from Phase B.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.3 of the spec are defined
- db.one, db.many, db.exec accept typed parameters
- Row accessor functions (row.str, row.u64, row.i64, row.f64, row.bool) type-check correctly
- DbErr sum type is defined and exhaustively matchable
- .tokei interface file is emitted and accurate

---

**Story 1.3.4 — std.json interface**

As a corpus engineer, I want the std.json module available, so that JSON encoding and decoding appears in corpus programs and HTTP handlers.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.4 of the spec are defined
- json.dec returns Json!JsonErr and is correctly typed
- json.enc accepts Str and returns Str (generic version deferred)
- Field accessor functions type-check correctly
- .tokei interface file is emitted and accurate

---

**Story 1.3.5 — std.file interface**

As a corpus engineer, I want the std.file module available, so that file I/O patterns can appear in Phase C corpus programs.

Dependencies: 1.3.1 (std.str)

Acceptance criteria:
- All types and functions in Section 17.5 of the spec are defined
- FileErr sum type is defined and exhaustively matchable
- file.read and file.write compile and execute end-to-end
- .tokei interface file is emitted and accurate

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
- .tokei interface file is emitted and accurate

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
- Pipeline sends task + spec to Qwen 32B first (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)
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
- Task descriptions are concise enough that the spec + task fits in under 2,000 tokens (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

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

As a validation engineer, I want Pass@1 measurements for tk on the benchmark task set using a general-purpose LLM with the toke spec in context, so that Gate 1 has an empirical correctness result before any fine-tuning. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

Acceptance criteria:
- Each benchmark task is attempted 10 times using Claude Haiku 4.5 with the full Phase 1 spec in the system prompt
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
- Document states the decision: proceed to Phase 2 or pivot to IR approach
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

### EPIC 2.2 — Phase 2 Tokenizer

---

**Story 2.2.1 — Phase 1 corpus preparation for tokenizer training**

As an ML engineer, I want the Phase 1 corpus prepared as a clean text dataset for BPE tokenizer training, so that the purpose-built tokenizer is trained on representative, high-quality toke source.

Acceptance criteria:
- All validated Phase 1 corpus entries are extracted as raw .toke source text
- String literal contents are stripped and replaced with placeholder tokens
- Dataset is deduplicated at the character level
- Dataset size is at least 500,000 source files
- Dataset is split: 90% train, 10% validation
- Preparation script runs in under 4 hours on Mac Studio

---

**Story 2.2.2 — BPE tokenizer training**

As an ML engineer, I want a purpose-built BPE tokenizer trained on the toke corpus with a 32,768-token vocabulary, so that Phase 2 achieves substantially lower token counts than cl100k_base.

Acceptance criteria:
- Tokenizer is trained using a standard BPE implementation (SentencePiece or tiktoken-compatible)
- Vocabulary size is exactly 32,768
- Training completes in under 12 hours on Mac Studio
- The 100 most common toke constructs are each represented as a single token
- Validation perplexity on held-out corpus is lower than cl100k_base
- Tokenizer is saved in a standard format loadable by the fine-tuning pipeline

---

**Story 2.2.3 — Tokenizer evaluation**

As a validation engineer, I want the purpose-built tokenizer evaluated against cl100k_base on the Gate 1 benchmark task set, so that the token density improvement is quantified before committing to Phase 2.

Acceptance criteria:
- Token counts are measured for Phase 2 source using the purpose-built tokenizer
- Token counts are measured for Phase 1 source using cl100k_base
- Token density improvement ratio is computed for each benchmark task
- Mean improvement across the benchmark set is reported
- Target: at least 2x token density improvement over cl100k_base for Phase 2 source
- Results are committed to benchmark/results/tokenizer_eval.json

---

### EPIC 2.3 — First Fine-Tuned Model

---

**Story 2.3.1 — Training data preparation**

As an ML engineer, I want the Phase A corpus formatted as instruction-tuning examples for Qwen 2.5 Coder 7B, so that the fine-tuning run has clean, correctly formatted data. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

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

As an ML engineer, I want the fine-tuned model continuously generating new corpus entries without human initiation, so that the corpus grows and improves without engineering intervention after setup. (The task description passed to the model should itself be expressed as densely as the toke spec allows — toke's structured type context and import .tokei interface files serve as the compressed task representation, replacing verbose natural language scaffolding.)

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

As a compiler engineer, I want performance benchmarks for all Phase 1 stdlib modules so that regressions in stdlib performance are detected before releases.

Dependencies: Epic 1.3 complete

Acceptance criteria:
- Benchmark suite covers all six Phase 1 stdlib modules
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
- Grammar covers all constructs including Phase 2 extensions
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
- All E-series, W-series, and L-series codes are documented with: code, meaning, phase, example source that triggers it, fix field value where applicable
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
- Each test verifies: error_code, phase, span_start, span_end, expected, got fields
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

*These stories and epics cover all 32 months of the toke project plan. Each epic maps to a section of the RFC and the project plan. Story completion is the acceptance criterion for milestone delivery. Gate stories are the single most important stories in each phase: if a gate story's acceptance criteria are not met, the phase does not proceed.*
