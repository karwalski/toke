# Epic 130 close-out — workspace audit & cleanup report (2026-08-19)

Executed 2026-08-19 by a worker-agent fleet (one repo per worker, main-thread
acceptance), per `docs/repo-hygiene.md` (130.1). Every repo was tagged
`pre-cleanup-20260818` before its first change. Initially nothing was pushed;
on 2026-08-19 the owner ratified the close-out decisions in-session and the
gitleaks-clean repos were pushed (see "Owner decisions" below). Still local:
toke-website and toke-test-programs (held for the fixture-secret pass).

## Per-repo before → after

| Repo | Size before | Size after | Dirty before | Dirty after | Key commits (local, unpushed) |
|---|---|---|---|---|---|
| toke | 172M (.git 148M loose) | **34M** (.git 10M) | 3 | 1¹ | 1c5ffce (130.1), 567651f (130.3) + Epic 130 entry rode into 17e0f82 |
| toke-corpus | 9.3G | 8.6G² | 135 | 1³ | 3c95c1c/482c0b0 (130.2⁴), b67b932 (130.4), 0a3e911 (130.17) |
| toke-model | 10G | **6.5G**⁵ | 4 | 0 | b44e174 (130.2), d71294e (130.5), c4adbb1 (130.17) — shas rewritten pre-push to strip unpushed training-data-p2 blobs |
| toke-tokenizer | 387M | **73M** | 2 | 1⁶ | c04b57a (130.2), f10a654 (130.6), e25c5ba (130.17) |
| toke-eval | 9.3M | 12M | 1,014 | 0 | 518e4ea (130.2⁷), 039fd2c (130.7), ff023a2 (130.17) |
| toke-benchmark | 73M | **removed**⁸ | 409 | — | GitHub repo archived (read-only, reversible) |
| toke-mcp | 162M | 162M | 7 | 0 | 93a52d6 (130.2), 03ddd70 (130.17) |
| toke-test-programs | 195M | 164M | 2 | 0 | 520ec35 (130.2), 982a617 (130.10), 95dc4a57 (130.17) |
| toke-website | 27M | 25M | 5 | 0 | d939b78 (130.2), 38c691d (130.9), 8d71fff (130.17) |
| toke-cloud (PRIVATE) | 211M | **1.1M**⁹ | 14 | 5¹⁰ | wip/refactor-rescue-20260419 @ 990f9c6 |
| toke-console (PRIVATE) | 404K | 408K | 0 | 0 | — (clean) |
| toke-demo | 340K | **archived whole** | 2 | — | ~/tk/archive/toke-demo-20260819 |
| toke-stdlib | 584K | **retired + archived** | 3 | — | e2f7d1e + 8858ec8 (in archive copy, unpushed¹¹) |
| toke-spec | 2.4M | 2.4M | 0 | 0 | b654855 (v0.3-staleness flag) |
| toke-ooke / homebrew-toke | — | — | 0 | 0 | verified clean, no action |
| ~/tk root | — | — | — | — | scratch deleted; research archived (130.13); AGENTS.md v2.0 (130.16) |

¹ the modified tracked `toke` binary — deliberately untouched, its removal is the 130.15 history-rewrite item.
² 2.2G of legacy under `corpus/` (archive_phase1, phase2_deduplicated) + `regen_v04/work` 312M **deferred** while the Epic 129 session actively commits there; move them when 129 waves are idle.
³ `regen/t1` stray binary — under `regen/`, deferred for the same reason.
⁴ a concurrent 129 commit swept the staged rescue files into 482c0b0 (16s race); content is correct and tracked, message attribution split across the two commits. Not worth a history rewrite.
⁵ remaining bulk is `output/7b-merged` 5.5G — owner-gated (see decisions).
⁶ `tokenizer_v03.json` deliberately untracked, pending the 116.9 v0.4 retrain.
⁷ 130.2 sample compile-check: 4/12 — every failure verified pre-existing in the v0.3 sources (`=` vs `==` in if-conditions, immutable-binding writes), not migration damage. Follow-up story recommended: repair the `=`/`==` bug class across the solution set.
⁸ byte-superset verified (diff -qr exit 0) against ~/tk/archive/toke-benchmark before removal; GitHub repo karwalski/toke-benchmark archived.
⁹ deleted regenerable `infra/node_modules` (210M) + `cdk.out`; a proper `package-lock.json` was recovered from npm's hidden lockfile first — commit it (recommended).
¹⁰ the recovered lockfile + 4 documented placeholder lambda stubs — owner call.
¹¹ done 2026-08-19: pushed (incl. salvaged ARCHIVED.md, a3c13c1), then `gh repo archive karwalski/toke-stdlib`; duplicate `archive/toke-stdlib` copy deleted under 130.14.

## Archive index (~/tk/archive/, all with MANIFEST.md)

New Epic-130 drops: `toke-legacy-20260819` (toke phase1/gate2 tests+docs),
`toke-corpus-phase-eras-20260819` (769M), `toke-model-gate2-era-20260819` (4.0G),
`toke-tokenizer-legacy-20260819` (297M), `toke-eval-gate2-scripts-20260819`,
`toke-test-programs-results-20260819` (~30M), `toke-website-legacy-20260819`,
`toke-demo-20260819`, `toke-stdlib-20260819`, `research-stale-pre-v0.4-20260819`;
plus MANIFEST.md added to the pre-existing `toke-benchmark` copy.

## Owner decisions — ratified 2026-08-19 (in-session), execution status

1. **130.14 archive prune — APPROVED & EXECUTED.** Salvage checklist completed
   first (`training-data-p2-late/` → gate2-era archive; tokenizer keep-set →
   `toke-tokenizer-legacy-20260819/salvage-130.14/`; stdlib `ARCHIVED.md`
   committed into the dated copy; corpus `manifest.json` kept). All big-file
   checksums re-verified immediately before deletion. Archive **14G → 5.4G**.
   Optional extras (keep-one-encoding dedupe, toke-ooke snapshot) declined.
2. **130.15 history rewrites — DEFERRED by decision.** Ordering when taken up:
   tokenizer_v03 blobs (mcp/website) + test-programs anytime; toke after
   `feat/type-flow-bytes-redesign` merges; corpus/model last (129 active).
   Update: toke-model's *unpushed* `training-data-p2/*.jsonl` blobs (113.7M)
   were stripped by a local-only rewrite before first push (no force-push
   involved) — the remaining rewrite scope is only what was already on remotes.
3. **`7b-merged` 5.5G — HOLD** until 128.3 lands; then archive (not delete)
   for one cycle before pruning.
4. **Private remotes — DONE.** `karwalski/toke-cloud` and
   `karwalski/toke-console` created PRIVATE and pushed (cloud: main +
   `wip/refactor-rescue-20260419`; recovered `package-lock.json` committed as
   part of main). The 4 placeholder lambda stub dirs remain uncommitted —
   still owner's call.
5. **Fixture-secret review — with owner.** Review sheet prepared at
   `~/tk/tmpwork/fixture-secret-review-130.md`. toke-website and
   toke-test-programs pushes are HELD until this pass is done.
6. **Pushes — APPROVED; executed for gitleaks-clean repos** (eval, mcp, spec,
   corpus, tokenizer, model, toke branch; stdlib pushed then GitHub-archived).
   Held: website + test-programs (see 5).
7. **Deferred corpus moves — remain deferred** until Epic 129 waves are idle
   (owner to trigger).

## Guardrails now in place

- `.gitignore` regression coverage added per repo for the junk classes that
  actually accrued: `*.ll`, stray binaries, `__pycache__`, `.DS_Store`,
  build output (`dist/`, egg-info, `*.js.map`, `test-results/`, `cdk.out/`,
  `node_modules/`), weights/corpus data (`training-data*/`, `ollama/*.gguf`).
- `docs/repo-hygiene.md` (130.1) is standing policy; `~/tk/AGENTS.md` v2.0
  (130.16) carries the repo map + rules forward.
- Recommended (not yet wired): a blob-size check in toke's PR CI — ties 123.8/123.9.

## Contributor-path verification (130.17)

Fresh local clones of every public repo, README followed cold, gitleaks run:
- READMEs rewritten truthfully: toke-corpus (was documenting the dead phase-2
  pipeline), toke-model (Gate-1 claims reframed as v0.3-era historical),
  toke-tokenizer (post-phase2 framing; every documented command had wrong flags
  — fixed).
- Fixed: toke-test-programs README layout drift (quickstart pointed at a
  nonexistent dir), toke-website stale hardcoded docs path in
  `validate_examples.py`, toke-eval/mcp missing related-repo links, remote-name
  mismatches (`toke-models`, `toke-web`, `ooke`) documented.
- gitleaks: clean in corpus/model/tokenizer/eval/mcp; fixture-data hits in
  website/test-programs (decision 5 above).
