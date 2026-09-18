# scripts/patterns — pattern-catalogue tooling (Epic 131)

Normative rules live in `docs/spec/patterns-protocol-v0.4.md`; the catalogue is `patterns/catalogue.json`,
fixtures are `patterns/<id>/<form>.tk` (or `.blocked.tk` for compiler-broken preferred forms).

| script | story | purpose |
|---|---|---|
| `validate_catalogue.py` | 131.1 | schema + verdict-consistency check (`--strict` once entries are measured) |
| `mine_shapes.py` | 131.3 | AST shape miner over the frozen corpus → `patterns/mined_shapes.json` |
| `mask_strings.py` | 131.4 | string-literal body masking (library + CLI) |
| `train_proxy.py` | 131.4 | trains the `proxy8k` token proxy from the frozen corpus → `patterns/proxy/` |
| `count_tokens.py` | 131.4 | token table for one function / all forms of a pattern |
| `recheck_caveats.py` | 131.26 | re-check `bug_caveats` after a 127.x closure; `--stale` lists entries measured on an older tkc |

Tests: `python3 -m pytest scripts/patterns/tests -q` (stdlib + pytest; `test_count_tokens.py` needs `./tkc`, the rest mock it).

## recheck_caveats.py (131.26) — bug-close auto-recheck

Every catalogue entry may carry `bug_caveats: [{issue: "127.N", effect, preferred_when_fixed}]`. When a
127.x story is closed in `docs/progress.md` (status cell contains `DONE`, bold/dates tolerated), run:

```sh
python3 scripts/patterns/recheck_caveats.py                 # human table, all caveats
python3 scripts/patterns/recheck_caveats.py --issue 127.6   # only caveats naming that issue
python3 scripts/patterns/recheck_caveats.py --json          # machine-readable
python3 scripts/patterns/recheck_caveats.py --fail-on-action   # CI: exit 1 if anything needs unblocking
python3 scripts/patterns/recheck_caveats.py --stale [--json]   # entries whose measured_at.tkc_sha != git HEAD
```

For each caveat whose issue is closed it takes the `preferred_when_fixed` fixture (usually a `.blocked.tk`),
runs `tkc --check`, `tkc -O2 -o`, executes it with `PAT_N=1000` (`--pat-n` to change) and compares stdout
with the entry's canonical form at the same `PAT_N`. Per caveat it reports `issue`, `closed`,
`preferred_form`, `compiles`, `builds`, `runs`, `output_matches` and an `action`:

| action | meaning | follow-up |
|---|---|---|
| `unblock-and-remeasure` | fixture compiles, builds, runs and matches the canonical output | scoped story: rename `.blocked.tk` → `.tk`, re-run the 131.5 bench for that entry only, re-derive the verdict — never a full re-sweep |
| `still-broken` | issue closed but the fixture still fails (or prints a different checksum) | reopen / comment on the 127.x story with the `detail` column |
| `n/a` | issue still open, not found in progress.md, or `preferred_when_fixed` is null | nothing |

Exit status is always 0 (it is a report) except with `--fail-on-action`. `TKC` env or `--tkc` selects the
compiler (default `<repo>/tkc`); per-fixture timeout comes from `patterns/<id>/bench.json:timeout_s`
(default 30 s). Nothing is written to the catalogue or to `progress.md`.

## Token proxy (131.4) — protocol §4

The real v0.4 tokenizer is trained *from* the pattern-canonical corpus, so pattern token costs are
measured now with a stand-in and re-measured later (§8 provisional rule, story 131.25).

Columns (`candidates[].tokens` schema names are binding):

| column | role | source |
|---|---|---|
| `proxy8k` | **decision metric** | byte-level BPE, vocab 8192, `min_frequency` 2, trained on the masked `tkc --min` corpus |
| `byte256` | floor | UTF-8 byte length of the masked min text (= the untrained vocab-256 tokenizer) |
| `v03` | informational | `~/tk/toke-tokenizer/tokenizer_v03.json` (HF format, vocab 16384) |
| `qwen25coder` | informational | `Qwen/Qwen2.5-Coder-7B` via `transformers` (HF cache first, then network) |
| `cl100k` | informational | `tiktoken` `cl100k_base` |
| `min_bytes` | tie-break | UTF-8 bytes of the **unmasked** min function |

Two forms are token-tied when `proxy8k` differs by ≤ 1 token **and** ≤ 5 %; ties break on `min_bytes`.
External columns are `null` (with a reason under `unavailable`) when their tokenizer cannot load;
`proxy8k`/`byte256` are never null.

### Masking — `mask_strings.py`

Every string-literal body becomes one `_` per run of body text; escapes (`\" \\ \n \t \r \0 \xHH`)
are body and vanish with it; `\(...)` interpolation interiors are **code** and are kept, with string
literals nested inside them masked recursively. The scanner mirrors `src/lexer.c lex_string`
(raw paren counting inside `\(...)`), so masked text lexes exactly as the original.

    "hello \(x) world"           -> "_\(x)_"
    "len=\(s.split(r;"f").len)"  -> "_\(s.split(r;"_").len)"
    python3 scripts/patterns/mask_strings.py file.min.tk

### Training — `train_proxy.py`

    python3 scripts/patterns/train_proxy.py                     # full corpus -> patterns/proxy/
    python3 scripts/patterns/train_proxy.py --limit 300         # smoke run (artefact gets -limit300 suffix)
    python3 scripts/patterns/train_proxy.py --holdout 500 --check-determinism

* Input: every accepted record (`judge.accepted`) under `~/tk/toke-corpus/corpus/regen_v04/<CAT>/*.json`
  (`tk_source`), files sorted by path; `tkc --min` in a thread pool (failures skipped and listed in
  the meta file); masked; sorted by `task_id`.
* `corpus_sha` = sha256 over the sorted lines `task_id\tsha256(masked_min)\n`; artefact name
  `patterns/proxy/proxy8k-<corpus_sha[:12]>.json` + `.meta.json` (record counts, min failures,
  vocab, timings, tkc version/git sha/binary sha, script sha, date, holdout numbers).
* Model: HF `tokenizers` BPE with `ByteLevel(add_prefix_space=False, use_regex=False)` — plan D1
  (`docs/architecture/tokenizer-v04-plan.md`): **no GPT-2 regex pre-split**, so cross-category merges
  like `f=`, `@(`, `):i64{` can form, as in `tokenizer_v03` and the future v0.4 tokenizer. No special tokens.
* Deterministic: same corpus ⇒ byte-identical artefact (`--check-determinism` trains twice and
  asserts identical vocab + merges; the meta file records the result).
* `--holdout N` trains a *throw-away* tokenizer without a deterministic stride sample of N records
  and reports the share of its vocab used on them (sanity number in the meta file). The shipped
  artefact is always trained on **all** records.
* `measured_at.proxy_sha` = sha256 of the artefact file (printed by both scripts);
  `measured_at.corpus_sha` = the corpus sha above.

### Counting — `count_tokens.py`

    python3 scripts/patterns/count_tokens.py patterns/str-build-loop/a.tk            # function pat
    python3 scripts/patterns/count_tokens.py bench/programs/prime_sieve.tk --function main
    python3 scripts/patterns/count_tokens.py --all-forms patterns/str-build-loop/     # per-form table
    python3 scripts/patterns/count_tokens.py --all-forms patterns/str-build-loop/ --json
    python3 scripts/patterns/count_tokens.py --text 'm=main;...' --whole

Output JSON: `{min_bytes, tokens{proxy8k, byte256, v03, qwen25coder, cl100k}, proxy_sha, proxy_file,
function, file, unavailable}` — paste `tokens` and `min_bytes` straight into a catalogue candidate.
The proxy defaults to the newest `patterns/proxy/proxy8k-*.json` (by meta date; `-limit` smoke
artefacts are ignored); `--proxy PATH` overrides.

**Region.** `--min` runs on the whole program first (a bare function does not compile), then
`--dump-ast` on the min text — its spans are byte offsets into exactly the text that is counted —
locates the `FUNC_DECL` named by `--function` (default `pat`). In tkc 2.8.0 a `FUNC_DECL` span covers
only the `f` keyword and no node carries the closing brace, so the end is found by string-aware brace
matching: the extent is `f=pat(...)...{...}` **excluding** the trailing `;` separator. The slice is
then masked and counted, so harness bytes never leak in.

### Known bias (protocol §4)

The proxy is trained on the *current, verbose* corpus: verbose shapes have cheaper merges and
canonical forms are measured **pessimistically**. This is the safe direction — a form that wins
under a hostile proxy wins more under the real tokenizer — and is why every verdict starts
`provisional`. It is countered by the `byte256` floor and by the 131.25 re-measure.

### Regenerating the proxy after a rewrite wave (131.25)

1. After 131.19 (post-wave re-audit) with the corpus re-frozen:
   `python3 scripts/patterns/train_proxy.py --holdout 500 --check-determinism`
   → a new `patterns/proxy/proxy8k-<new corpus_sha12>.json` (+ meta). Keep the old artefact — every
   existing `measured_at.proxy_sha` still points at it.
2. Re-run `count_tokens.py --all-forms patterns/<id>/` for every catalogue entry (the newest proxy is
   picked automatically), update `tokens`, `min_bytes`, `measured_at.{proxy_sha, corpus_sha, date}`.
3. `python3 scripts/patterns/validate_catalogue.py --strict` — every verdict flip is listed and
   re-opened, never silently changed (protocol §8).
4. Repeat once more under the real v0.4 tokenizer when 116.9 Phase 3 locks (add it as the decision
   column then; `proxy8k` becomes informational).

### Environment (as used for the first proxy, 2026-09-18)

Nothing had to be installed: Python 3.14.7 (`/opt/homebrew/bin/python3`), `tokenizers` 0.22.2,
`transformers` 5.8.1, `tiktoken` 0.12.0, `pytest` 9.0.2. If `tokenizers` is missing:
`python3 -m pip install --user tokenizers` (and `transformers`/`tiktoken` for the informational
columns — they degrade to `null` when absent). The Qwen tokenizer is fetched from the HF cache
(`~/.cache/huggingface/hub/models--Qwen--Qwen2.5-Coder-7B`) or downloaded on first use.
