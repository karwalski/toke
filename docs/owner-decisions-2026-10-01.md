---
title: Owner decisions and actions — 2026-10-01
---

# Owner decisions and actions — 2026-10-01

Every story in `docs/progress.md` that is blocked on the owner, with the options and a
recommendation. Produced by the 2026-10-01 backlog review, which also marked stale rows done or
superseded (each with its evidence in the row). Rows referenced here carry
`Blocked: owner — see docs/owner-decisions-2026-10-01.md §n` in the tracker.

**How to use this:** answer an item with its number and a letter (e.g. "2.3 a"); the main thread
records the decision in the row, lifts the block, and schedules the work.

**Highest leverage first:** §1.1 (push the release) unblocks the loke handoff; §4.1 (Epic 131 waves)
unblocks the whole Gate 2 chain (131.18 → 131.27 → Epic 128 lanes); §2.1–2.3 unblock the remaining
language rows.

---

## 1. Release and publishing — owner actions

| # | Story | Action | Unblocks |
|---|---|---|---|
| 1.1 | 138.17, 138.7 | Push toke `release/3.1.0` and tag `v3.1.0`; push toke-ooke `release/ooke-rc2` and tag `v3.0.0-rc.2`. Commands: `git -C ~/tk/toke push origin release/3.1.0 v3.1.0` and `git -C ~/tk/toke-ooke push origin release/ooke-rc2 v3.0.0-rc.2`. Then send loke `docs/loke-handoff-2026-09-27-reply.md` (on the release branch) and tell them to pin v3.1.0. | loke handoff close-out; 138.17, 138.7 |
| 1.2 | 132.4 | Publish the prepared registry texts: `gh repo edit` descriptions/topics, PyPI, npm, VS Code Marketplace, both Hugging Face cards (withdrawn claims are still live there). | 132.4 and the canonical-facts pending warnings |
| 1.3 | 132.5 | Approve the website deploy; grant Search Console and analytics access for the baseline. | 132.5 |
| 1.4 | 132.27 | Create `karwalski/homebrew-toke` on GitHub (the tap has no remote; `brew tap` fails). Checksums are filled after release artefacts are published. | 132.27 |
| 1.5 | 132.28 | Describe or archive `karwalski/tkc` (early compiler copy). **Recommend archive.** | 132.28 |
| 1.6 | 130.11 | Create the **private** GitHub repos for toke-cloud and toke-console and push (remotes are configured, nothing pushed). | 130.11 |
| 1.7 | 135.8 | Create the `toke-ocr` repository and remote. | 135.8 |

## 2. Language and compiler decisions

| # | Story | Decision | Options | Recommendation |
|---|---|---|---|---|
| 2.1 | 127.170 (loke T-55) | Strings carry no length, so `str.charat` is quadratic | (a) S1 sealed string heap + side bitmap; (b) S2 | **(a)** — the design in runtime-abi §3.1 and the prototype: large instruction savings and old producers stay correct; costs reserved address space. |
| 2.2 | 127.196 | `let` leaks out of its block | (a) block scoping; (b) document function scoping | **(a)**, after measuring how many corpus programs rely on the leak — least surprise for model-written code. |
| 2.3 | 127.225 | Re-raising into a different error type | (a) check-time error; (b) runtime trap; (c) map variants by name | **(a) with (b) as a backstop.** |
| 2.4 | 127.227 (3) | Typed-empty `@($T)` forms in the spec | ratify into semantics §2.1 + grammar / remove | **Ratify** — they already compile and are used. |
| 2.5 | 127.251 | Spelling for the upper-case `.tki` type names that collide with modules | a naming rule | **`<module><noun>`** (jsonval, dbrow, toonval, yamlval, vechandle, timeduration), following i18nbundle/tlsconfig. |
| 2.6 | 127.197 | `--migrate` declines reuse ECOMPILE | dedicated exit code + structured diagnostic / keep | **Dedicated code + structured record** (AGENTS §3.1). |
| 2.7 | 127.198 | Comment form `(* *)` contradicts W1020's text | make `(* *)` official / ban it in the lexer | **Make it official** — the lexer and C041 already accept it; fix W1020 and the spec. |
| 2.8 | 127.161 | `--migrate` placement of interior comments (after 2.6/2.7) | per design doc §5 | **Move to end of line with a note.** |
| 2.9 | 138.9 (loke T-17) | `json.parse` on an object handle | (a) alias to `decode`; (b) type it to reject object handles | **(b)** — keeps C255 and the documented API, turns a silent wrong result into a compile error. |
| 2.10 | 121.28 (COM-04) | tkc finds clang via `$PATH` | (a) bake the absolute path found at build time; (b) `TKC_CC` override; (c) both | **(c)** — safe default, documented escape hatch. Small once decided. |
| 2.11 | 135.18 | Compact `[byte]` on `fix/ea-bytes` costs per-element dispatch | (a) accept the cost and re-record perf; (b) typed `[byte]` (language change); (c) skip the width test when the element type rules out a compact block | **(c)** — keeps the perf gate meaningful without a language change. |
| 2.14 | 127.264 (2) | Exit code of a limit E9010. Today a cap exceeded *before* codegen (funcs/structs/imports/locals counted) exits 1; one found *during* codegen or a cap too large to allocate exits 2 ("internal"). The v0.4 spec says nothing; v0.3 §17.4 calls rc 2 an internal compiler error. | (a) every limit E9010 is a user error, rc 1; (b) keep the split | **(a)** — a cap the user set and exceeded is not an internal error; one rule is easier for the repair loop. Small change (C641 expectations + three call sites). |
| 2.15 | 131.32 | Raw control bytes inside string literals now get **W1021** (warning, exact `\xHH` fix) because the spec permits any UTF-8 there. | (a) keep a warning; (b) make it an error by amending the spec ("Arbitrary UTF-8 is permitted inside string literal content") | **(a)** — the fix is mechanical and the repair loop applies it; an error would reject existing valid programs for no safety gain. |
| 2.12 | 124.6c | `@nobounds` annotation | new annotation grammar / CLI flag or pragma / withdraw | **Withdraw** until a perf trace shows real residual bounds cost; -O2 already removes provably safe checks. |
| 2.13 | 124.6e | Post-hoc measurement of the ADR-0012 flips | define a rebuild-and-compare / retire | **Retire into 128.15** — the flips showed no codegen change and the row is behind the compute hold. |

## 3. Security and runtime decisions

| # | Story | Decision | Options | Recommendation |
|---|---|---|---|---|
| 3.1 | 121.64 | TLS client with no pin authenticates nothing | (a) verified mode opt-in; (b) verify by default, pinning/insecure explicit | **(b)** — llm.c already verifies by default (121.23). Breaks unpinned callers: changelog entry required. |
| 3.2 | 121.41 (3) | Text-format access-log field spoofing | (a) logfmt quoting of the message; (b) escape spaces and `=` only; (c) document | **(a)** — the only option that fully prevents field spoofing. |
| 3.3 | 127.246 (3) | ocr/pdf/xlsx `lasterr` accessors duplicate the error payload | retire / keep | **Retire**, matching the 127.102 ruling for csv/file/zip. |
| 3.5 | 121.80 | Idle HTTP/2 connections now close after 2 s (the HTTP/1.1 keep-alive idle) instead of 30 s, because an idle h2 connection held a pre-fork worker and stalled the next client ~30 s. Build-time overridable (`H2_IDLE_TIMEOUT_S`). | keep 2 s / restore 30 s / make it a runtime setting | **Keep 2 s, and make it a runtime setting later** if a deployment needs long-lived h2 — a worker held hostage by an idle connection is the worse failure. |
| 3.4 | 127.199 | `stdlib/toon.tk` (std.toon is implemented in C; this file is unused) | port / withdraw | **Withdraw (archive)** together with 135.14. |

## 4. Corpus, training and tokenizer

| # | Story | Decision / action | Recommendation |
|---|---|---|---|
| 4.1 | 131.15, 131.16, 131.17 | Launch the Epic 131 rewrite waves. The stated hold ("until Epic 127 has no open P0") is met. Each wave needs a fresh compiler pin and re-prep first; these are large corpus jobs. The earlier concern that the bank's diff gate rejects part of the 131.15 wave by construction (131.69) should be re-checked at re-prep. | **Go, one wave at a time (131.15 first)**, re-pinned to the current compiler. |
| 4.2 | 131.1 | Ratify the protocol thresholds (5% / 1.5× / 50 ms). | **Ratify** — 131.25's two identical runs flipped no canonical form inside these bands. |
| 4.3 | 132.110 (1) | `must_merge_v04.json` no longer derivable from the current patterns catalogue. (a) re-derive and commit; (b) pin derive + test to the catalogue at `a084187b`; (c) re-derive and carry `}};` forward. Fresh derivation: `~/tk/tmpwork/z18/must_merge_fresh.json` (forced set unchanged; four verify entries drop, two added). | **(a)** — Gate 2 should use the verdicts 131.25 measured. |
| 4.4 | 132.64 (+132.65) | Canonical tokenizer for published token counts. (a) the shipped `tokenizer_v03.json`; (b) the 8K SentencePiece; (c) drop toke counts until 116.9. | **(a), labelled "v03, legacy"** — it is what the page loads and 132.106 already regenerated panels with it. Then regenerate the remaining /tokens panels (132.65). |
| 4.5 | 135.34 (2) | One gitignored phase2 corpus record carries the retired W1010 fix text (an accurate record of tkc 0.1.0's output). keep / strip the `fix` key / exclude from training prep | **Exclude** — legacy data; excluding costs nothing and falsifies nothing. |
| 4.6 | 136.72 (3) | Branch `fix/gc-residue` (`bfb0d46`) banks records without a differential test | **Merge, stamped compile-only** as 131.78 did. |
| 4.7 | 128.23 | Retire Epic 116 and merge the duplicate Epic 128 rows (128.3→128.10, 128.5→128.11/128.14, 128.6→128.12/128.13) | **Approve**, before any other Epic 128 work. |
| 4.8 | 128.4 | GPU EC2 spend for Gate 2 training — approve and lift the compute hold, or defer explicitly. | Owner call (cost). Nothing in Epic 128's lanes moves without it or local hardware. |
| 4.10 | 128.1b / 128.1a | BENCH-128's G0 "all-PASS against pre131" needs the exact tkc the pre131 manifest pinned (`e31e45…`); that pinned copy lived in a temp directory and is probably gone. (a) re-pin pre131 to a rebuilt tkc at that commit; (b) accept only the v04 inputs for G0. | **(a) if the commit still builds reproducibly, else (b)** — a check against a compiler nobody can reproduce is not evidence. |
| 4.11 | 132.24 | Batch Z23 (protocol K7) found the published Gate-2 "200 eval" set was *plausibly* the corpus-derived `tasks.jsonl` (now quarantined, 128.16). Unproven, so metrics-baseline.md and the HF card were left unchanged. | **Investigate under 132.24 before any Gate-2 figure is restated** — if confirmed, mark those figures as partly measured on corpus-derived tasks. |
| 4.12 | 131.22a, 131.85 | The benchmark holdout check compares corpus ids against `holdout_task_ids.txt` (plain integers and `HumanEval/N`), which can never match — it is now reported VACUOUS, not a pass. (a) compare against BENCH-128's id list once 128.1a freezes it, and use the 128.1b D4 content-hash check meanwhile; (b) keep VACUOUS as a warning. Also: re-stamp the committed `data/tokenizer_manifest_pre131.json`, which still says `disjoint_benchmark: true`? | **(a), and run the v04 curation with `--strict-disjoint`; re-stamp the pre131 manifest** so no committed file claims a disjointness nobody checked. |
| 4.13 | 131.82 | Eight test ids are shared between test/types/*.yaml and test/conform/*.sh (L001, T001–T007), now grandfathered. Renumber? | **Keep grandfathered** — renumbering changes each test's identity for no behavioural gain; the registry prevents new collisions. |
| 4.14 | 131.63 | Comparison examples carry per-side token counts (`[PYTHON Nt]` / `[TK Nt]`) with no guarantee both sides used the same tokenizer. Should the exporter block them like `[RATIO]`? | **Block unless both counts come from one named tokenizer** (TEMSpec §6.3) — otherwise it is the same unsourced ratio in two numbers. |
| 4.15 | 128.31 | toke-eval's `benchmark/generate_tasks.py` is seeded, so it deterministically regenerates the hidden task set (0501–1000) into any directory — in a public repo agents can run. (a) move it to a private/owner-only location; (b) keep it but have it refuse to run without an owner opt-in; (c) delete it (the hidden set already exists). | **(a)** — a generator for the held-out set is held-out material; a public copy makes the hold-out reproducible by anyone. |
| 4.16 | 131.87 | toke-corpus's Phase A differential test never compares the three reference outputs (three different answers pass), ignores `test_inputs`, and always sets `toke_agrees=True`. (a) compare reference outputs (majority agreement) and run the declared test inputs; (b) also build and run the toke program now that tkc emits executables; (c) leave Phase A. Either change alters which records are accepted. | **(a) then (b)** — a differential test that cannot disagree is not one; re-run it over the banked records before 131.27 freezes. |
| 4.9 | 133.5 | Grade the classified solutions with `toke-eval/scripts/solutions_v04.py` — it reads hidden tests, so only the owner may run it. | Owner action. |

## 5. Repository hygiene and licensing

| # | Story | Decision / action | Recommendation |
|---|---|---|---|
| 5.1 | 135.15a, 132.78 | Stdlib licence: (a) MIT `stdlib/LICENSE` + SPDX headers; (b) Apache-2.0 everywhere | **(a)** — the stdlib is statically linked into every user binary and MIT has no NOTICE obligation. 132.78's repo-name fixes can proceed now. |
| 5.2 | 132.83 | Delete local refs in toke-model that hold a 113 MB file (`refs/original`, `feat/116-idiom-judge*`, `pre-cleanup-20260818`) | **Delete**, after confirming the tag is not the pushed copy — one accidental push publishes it. |
| 5.3 | 135.14 | Permission for a bulk `git rm` of dead stdlib `.tk`/`.md` files (quarantine list) | **Grant** — archive per the hygiene rubric. |
| 5.4 | 136.67 | Sign off deleting duplicate C implementations | **Grant.** |
| 5.5 | 136.71 | Dead glue symbols: wire up or delete | **Delete the shadowed/unreachable ones; wire only those with a `.tki` quarantine entry.** |
| 5.7 | 128.27 (1) | toke-eval's GitHub workflow sits at `benchmark/.github/workflows/ci.yml`, so GitHub never runs it. Moving it to a root `.github/` creates a new top-level directory (AGENTS §4.6 needs a story + sign-off). | **Approve the move** — a CI file GitHub cannot see protects nothing; `make ci` is the local stand-in meanwhile. |
| 5.8 | 131.84 | Stale code in toke-corpus and toke: 0-byte stub modules (`diff_test/{compile,harness,vote}.py`, `pipeline/{validate,validate_schema,escalate,generate}.py`), the unmerged local branch `feature/corpus-pipeline-scaffold`, and two toke branches whose work is already on main (`fix/compiler-error-union-payload` → d23cc8b1, `fix/127-81-82` → 63f7fceb). | **Remove the stubs (archive convention) and delete the two toke branches**; archive the scaffold branch as a bundle in ~/tk/archive before deleting it. |
| 5.6 | 130.15 | Rewrite published git history | **Close as accepted debt** — the blobs are already public and cloned; `git gc` already shrank the repo; binaries are no longer tracked. |

## 6. Infrastructure and cost

| # | Story | Action / decision | Recommendation |
|---|---|---|---|
| 6.1 | 121.47 | DEP-02 Cloudflare Origin CA or ACME cert with Full (strict); DEP-03 rotate the TLS key that was rsynced; DEP-05 build on a build host; DEP-07 store a hash of the API key in toke-cloud. | Owner actions; DEP-03 first. |
| 6.2 | 132.92 | Create an IAM instance profile scoped to the model-store prefix. | Owner action. |
| 6.3 | 134.38 | Record the live server facts (`systemctl cat`/`show`: unit, binary, `User=`, logs, whether content deploys need a restart). | Owner action. |
| 6.4 | 108.13f | TokeCloudStack's gateway still answers for a deleted endpoint. deploy / keep blocked / tear down | **Tear down** (or keep blocked) — a live gateway for a deleted model reads as a key error. |
| 6.5 | 71.2.1 | Hosted MCP deploy (the DNS blocker is cleared) | **Defer until after 128.9** — the hosted API was withdrawn 2026-09-20; self-hosted npm covers users. |
| 6.6 | 100.0 | Email for the project domain | **Cloudflare Email Routing** — DNS is already on Cloudflare; no server to run. You add the MX record. |
| 6.7 | 130.22 | Check the five Lightsail workers from 108.1 are gone (fixed monthly billing). | Owner action. |
| 6.8 | 130.23 | Seventeen `http_recorder.py` processes and one `ooke-toke serve` from 27–28 Sep are still running (parent 1). | Owner: confirm and kill, or authorise the main thread to kill those exact PIDs. |

## 7. Product scope

| # | Story | Decision | Recommendation |
|---|---|---|---|
| 7.1 | 5.1.6 | Web translator | **Cancel** — /tokenizer and /tokens already compare, and translation needs a model (Gate 1 not passed, 128.19). |
| 7.2 | 57.10.4 | DynamoDB backend | **Won't do** — SQL backends cover the use cases. |
| 7.3 | 103.3 | llama.cpp streaming load | **Park** until there is a toke model to serve (128.9). |
| 7.4 | 132.75 | toke-mcp rate limit is per process | **Document per-process** — there is one deployed instance. |
| 7.5 | 132.3 | Article page | Owner action: supply the draft (never saved to disk). |

## 8. For the loke owner (outside our repositories)

- loke `installer.tkt:317-318` uses `{! raw("hardware_json") !}` inside a JavaScript string. ooke has
  no `raw` directive, so it is silently dropped today; if it ever became one, it would inject unescaped
  text into a `<script>` string. Build that value with `{= … =}` (now context-escaped, 121.58) or JSON
  encoding.

## 9. What continues without the owner

Open and workable now (the main thread schedules these as batches):

- **Compiler / stdlib:** 121.29 (http REL-02/07/08), 121.42 (three ungated capability sinks), 121.62
  (`http.setnotfound`), 121.71, 121.72, 127.257 (template RSS), 127.263, 127.244 (enforce stdlib `T!E`,
  owner-decided in 127.30), 127.236 (sret ABI, large), 122.5 (PQC API — library choice already made by
  ADR-0013(e)), 123.13, 138.19 (scoped grants — design first).
- **Corpus pipeline (Epic 131):** 131.22a, 131.31, 131.32, 131.33, 131.43, 131.45, 131.50, 131.52, 131.57,
  131.60, 131.62, 131.63, 131.64, 131.65, 131.68, 131.71, 131.75, 131.80, 131.81, 131.82, 131.83;
  131.48 (check `xml.get`'s missing-path behaviour, then close) and 131.59 (re-run its repro, then
  supersede by 127.114).
- **Gate 2 prep (Epic 128):** 128.1b, 128.16, 128.18, 128.20, 128.21, 128.22, 128.24, 128.25; 128.2 after §4.7.
- **Website / tooling:** 132.37a, 132.52, 132.71, 132.112, 76.1.3b–d (`tkc pkg`), 76.1.6c, 96.7,
  114.28, 119.8, 102.12 (remaining links).
- **In flight now:** Batches Z3, Z14, Z15, Z16 (rows marked `in_progress` with their branch).

Blocked on something other than the owner (compute, a quiet machine, or an earlier story) — no
action needed now: the Epic 128 lane chain (on 128.1a → 131.27, 128.4, 116.9), 131.18–131.29,
131.51/53/54/56, 131.5, 134.6, 134.12, 116.9, 116.13-H4, 118.4, 133.2, 133.3, 81.x, 71.2.2/3/6, 99.2.
