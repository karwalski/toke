# repo-hygiene.md — workspace keep/archive/delete rules (Epic 130.1)

**Status:** ratified by owner kick-off, 2026-08-19 (Epic 130 execution start).
**Scope:** every repo under `~/tk/` plus the workspace root. Binding on every
worker (human or agent) doing Epic 130 work, and standing policy afterwards.

---

## 1. The keep test

A file stays in a repo **only if a contributor needs it to build, use, or
extend** toke, its models, docs, training data, corpus, examples, or MCP.
Everything else is archived or deleted per the rules below. "Might be
interesting someday" is what the archive is for — not the repos.

## 2. Archive convention

- Location: `~/tk/archive/<repo>-<what>-<YYYYMMDD>/`
  (precedent: `archive/docs-stale-pre-v0.4-20260703/`).
- Archive is **local-only**. Archived content is *moved* out of the repo
  (deleted from the working tree in a scoped commit), never copied-and-left.
- Every archive drop carries a `MANIFEST.md` at its root:
  - source repo + path(s)
  - source repo HEAD sha at time of move
  - date moved, story ID (130.x)
  - one-line reason
  - restore hint (`mv` back + `git checkout` of what, if applicable)

## 3. Hard-delete whitelist

Delete (no archive) **only**:

1. Compile/build artifacts: `*.ll` scratch IR, stray Mach-O binaries,
   `__pycache__/`, `.pytest_cache/`, `.DS_Store`, `dist/`, build output.
2. Content **byte-verified identical** (checksum, not eyeballing) to a copy
   that lives in a repo or in the archive.

Anything else that leaves a repo goes through §2.

## 4. Do-not-touch list (Epic 128/129 dependencies)

- `toke-corpus/regen/` (harness) and banked `toke-corpus/corpus/regen_v04/`
  records — Epic 129 is active. (`regen_v04/work/` intermediates may be pruned
  only if `regen/REBUILD_STATUS.md` confirms banking complete.)
- Library manifests and the audit ledgers (129.3 / Epic 126).
- The Epic 128.1 benchmark set (`toke-eval/benchmark/`, incl. `hidden_tests/`).
- Anything on an in-flight feature branch (toke is mid
  `feat/type-flow-bytes-redesign` — tidy from the branch state, no switching).
- `hidden_tests/` everywhere: **move/preserve only, never read** (AGENTS.md §3.5).

## 5. Public/private policy

| Repo | Policy |
|---|---|
| toke, toke-corpus, toke-model, toke-tokenizer, toke-eval, toke-mcp, toke-spec, toke-stdlib, toke-test-programs, toke-website, homebrew-toke, toke-ooke | Public-capable. Secret scan required before any push to a public remote. |
| toke-cloud, toke-console | **Private, never public.** Billing/auth/infra code. Private remote only. |
| toke-benchmark | Retired (ARCHIVED.md 2026-05-18) → GitHub-archived, local copy removed after byte-verification. |

No instance IPs, key paths, credentials, or `.env` contents in any public repo
(existing Epic 128 rule, generalised).

## 6. Worker protocol (Epic 130)

1. One repo per worker; the main thread owns acceptance.
2. Before a repo's first change: `git tag pre-cleanup-20260818` (rollback point).
3. Scoped commits per AGENTS.md §4.4 — rescue commits separate from tidy
   commits, one logical change each, story ID in the message.
4. Rescue before tidy: nothing valuable dies uncommitted (130.2 completes
   before any 130.3–130.13 archiving in that repo).
5. Owner-gated (prepared, not executed, without explicit sign-off):
   deletions *inside* `~/tk/archive/` (130.14), history rewrites/force-pushes
   (130.15), disposal of `toke-model/output/7b-merged` (130.5).
6. Nothing is pushed to remotes by workers; the owner reviews and pushes.

## 7. Standing rules (survive the epic; folded into AGENTS.md by 130.16)

- No binaries, model weights, corpus data, or generated results tracked in git.
  The §4.3 100MB pre-commit check tightens to: no binary of any size, no data
  file >5MB, without an explicit story justifying it.
- Every repo's `.gitignore` covers the junk classes this epic actually found:
  `*.ll`, Mach-O outputs, `__pycache__/`, `.DS_Store`, results/, weights,
  corpus JSONL.
- Scratch lives in `~/tk/tmpwork/` (recreate at will) or `/tmp`, never in a
  repo and never at the workspace root.
- New top-level workspace directories require a tracking story (extends
  AGENTS.md §8 to the workspace).
