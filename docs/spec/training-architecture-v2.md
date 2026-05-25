# From-Scratch Toke Code Generation — Training Architecture v2

## Summary

Three-model architecture for toke code generation:
1. **Code Model** (toke BPE, 500M-1.5B) — generates pure .tk source with `"_"` string placeholders
2. **String Model** (general LLM or small fine-tune) — fills string content contextually
3. **Reasoning Model** (general LLM) — generates .tkc.md companion documentation

## Corpus Requirements

### Phase A — Syntax Model (compile-correct)
- 50K execution-verified programs
- 20K error→fix pairs (from repair loops)
- 30K function completions
- 20K partial→complete pairs
- **Total: 120K records, ~6M toke BPE tokens**
- Model: Qwen 2.5 Coder 1.5B fine-tune
- Training: ~8 hours on A10G

### Phase B — Full Model (functionally-correct, from scratch)
- 200K execution-verified programs with I/O tests
- 200K I/O test pairs
- 50K multi-function programs
- 50K stdlib-heavy programs
- 50K error→fix pairs
- **Total: 550K records, ~32.5M toke BPE tokens**
- Model: 500M-1.5B from scratch with toke BPE 16K tokenizer
- Training: ~48 hours on A100

## Data Streams

### Stream 1 — Code (Model 1)
- Pure .tk source, strings replaced with `"_"`
- Verified: tkc --check + I/O test harness
- No legacy syntax, no keyword-as-variable, no uppercase

### Stream 2 — Strings (Model 2)
- Pairs: (code_with_placeholders, code_with_real_strings)
- Context-appropriate content filling

### Stream 3 — Reasoning (Model 3)
- Pairs: (code, companion_documentation)
- English explanations, algorithm notes, complexity

## Quality Gates

Every program in the corpus MUST:
1. Pass `tkc --check` (zero errors)
2. Compile to binary via `tkc --emit-llvm` + clang
3. Run with test input and produce expected output
4. Use no legacy v0.1/v0.2 syntax
5. Use no keywords as variable names (i, f, t, m)
6. Have strings normalised (stripped for Stream 1, preserved for Stream 2)
7. Have a companion file (Stream 3)

## Current State (2026-05-25)

- 47K compile-verified programs (existing corpus)
- ~615 execution-verified programs (Epic 100 workers, growing)
- 0 error→fix pairs captured (need to add to worker scripts)
- 73K curriculum records (from 71.5.7, need I/O verification)
- **55.6% functional Pass@1** (272/489) — corrected from 8% after io.readln() stdlib fix (2026-05-25). The model generates functionally correct code at a much higher rate than originally measured.
- 0% one-shot rate from toke API — all complex programs written by Claude repair loop
