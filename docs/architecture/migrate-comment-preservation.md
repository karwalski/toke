# `--migrate` comment preservation — design

**Stories:** 127.161 (comments are stripped before lexing; architectural),
127.160 (the per-file decline cannot classify a multi-file module).
**Status:** implemented (story 127.161, 2026-10-07) after the owner decisions
of 2026-10-01 §2.6–§2.8; §5 records how each was decided. §2 describes the
interim guard it replaced.
**Date:** 2026-09-25 (design), 2026-10-07 (implementation).

## 1. The problem

`src/migrate.c` rewrites legacy (v0.2/v0.3) toke into v0.4 text. Its first
stage, `prepass()`, deletes every `(* *)`, `//` and `#` comment **before** the
source is lexed. Every later stage works on that stripped buffer:

| Stage | Kind | Offsets computed against |
|---|---|---|
| `prepass()` | character scan | original source |
| Step 3: token transforms (keyword case, `$` sigils, `[T]`→`@T`) | lex + gap copy | stripped buffer |
| Step 4: `\|{` → `mt … {` | character scan | Step 3 output |
| Step 5: `if cond {` → `if(cond){` | character scan | Step 4 output |
| `postpass_semicolons` | character scan | Step 5 output |
| `postpass_missing_rettype`, `_equality`, `_return_types`, `_str_index`, `_str_concat`, `_mut_bindings` | re-parse, then span edits (`mig_apply_edits`) | the previous pass's output |

A comment has nowhere to live in this pipeline, so nothing downstream can put
one back. Before 127.57 this destroyed 216 of 216 comment blocks across 124
files, with exit 0.

## 2. What is in place now (the interim guard)

- **127.57:** source that already compiles standalone is declined and echoed
  verbatim (non-zero exit).
- **127.160 / 127.161 (this change):** source that contains any comment the
  prepass would strip is declined too. The source is echoed byte for byte, the
  exit code is non-zero, and stderr names the file, the comment count and the
  first comment's line. The count comes from `tkc_migrate_count_comments()`,
  which uses the prepass's own recognition rules. `TKC_MIGRATE_FORCE=1` is the
  explicit consent to lose them.
- Gate: `test/conform/C058_migrate_never_drops_comments.sh`. Its tree-wide
  case asserts that `--migrate` loses no comment marker from any `.tk` file in
  the repository.

The multi-file-module files from 127.160 (`examples/bookmarks-api/{api,main}.tk`,
`examples/mortgage/calc.tk`) are protected because they have comments. The
guard does not treat an import-resolution-only failure as clean, so 127.160's
"heuristic on a heuristic" question does not have to be answered first.

The cost: legacy source that has comments can no longer be migrated without
the override. That is intended. Losing comments is now a choice the user
makes, and the user is told about it.

## 3. Design for real preservation

### 3.1 Key observation: the pipeline nearly preserves line structure

Most rewrites stay inside one line. The Step 3 gap copy keeps newlines, and
Steps 4 and 5 and the semicolon pass never add or remove a newline. The
prepass already keeps the newline that ends a `//` or `#` comment. The
exceptions are:

1. A **multi-line `(* *)` block**. The prepass drops its interior newlines
   (`i--; continue` skips the whole block).
2. The **inserted module stub** `m=migrated;\n`. It adds one leading line and
   is removed on output.
3. **AST span edits whose span crosses a newline**, for example a `+` chain
   split over lines that `postpass_str_concat` rewrites into one
   interpolation.

So the design anchors comments by **line**, not by offset or token ordinal.
Token ordinals change whenever a pass inserts or removes a token.

### 3.2 Mechanism

1. **Record.** In `prepass()`, append each stripped comment to a side table
   `MigComment { int line; int col_kind; char *text; }`. `line` is the 1-based
   line of the comment's first character. `col_kind` is one of:
   - `OWN_LINE`: only whitespace before and after it on its line(s).
   - `LEADING`: code follows it on the same line.
   - `TRAILING`: code precedes it and nothing follows.
   - `INTERIOR`: code on both sides.
2. **Keep the line map invariant.** When the prepass strips a multi-line
   block comment, it emits one `\n` for each newline the block contained. It
   does not change anything else.
3. **Normalise the form.** The v0.4 lexer skips `(* *)` without comment. It
   also skips `#` and `//` to end of line, but each one draws W1020, and the
   `//` message says "toke has no comment syntax — documentation belongs in
   companion .tkc files". Rewrite `// t` and `# t` as `(* t *)` so the
   migrated file lexes without warnings. Whether `(* *)` itself should
   survive is a language question, not a migrator one (see §5). If the
   text of a line comment would unbalance the lexer's nesting (an unmatched
   `(*` or `*)` inside it), the file falls back to the decline in §2 and the
   file is named. Do not escape: the language has no comment escape, and a
   silent rewrite of comment text is a smaller version of the same defect.
4. **Guard.** After the last postpass, compare the newline count of the
   output (stub removed) with the newline count of the input. If they differ,
   some edit crossed a line, the map is no longer the identity, and
   reinsertion could attach a comment to the wrong line. In that case, fall
   back to the §2 decline and name the file and the pass that moved lines.
   The guard is what makes the design sound: it never guesses.
5. **Reinsert.** Walk the output line by line and put each comment back on
   its recorded line:
   - `OWN_LINE`: the line is blank after stripping, so write the comment
     there with the original indentation.
   - `LEADING`: insert it after the line's indentation, before the code.
   - `TRAILING`: append it to the line after one space.
   - `INTERIOR`: move it to the end of the line and tell the user on stderr.
     This moves the comment but does not lose it. Moving it instead of
     declining is a policy choice for the owner (see §5).
6. **Verify.** Re-lex the final text. The comment count must equal the table
   size, and the token stream with comments skipped must be identical to the
   token stream before reinsertion. If either check fails, decline and name
   the file. This check is cheap and it rules out a reinsertion that breaks
   the program.

### 3.3 The retry path

When both lex profiles fail, `tkc_migrate()` calls itself on the prepass
output. The inner call receives text whose comments are already stripped, so
it has no table. The outer call must pass its table down, or the retry must
count as a guard failure (decline when there are comments). The second option
is simpler and loses nothing.

`tkc_migrate()` writes straight to `out`. To decline after it has started, the
caller has to buffer the output first. `main.c` would pass an
`open_memstream()` stream (POSIX 2008, available on macOS 10.13+ and glibc)
and copy it to stdout only on success.

### 3.4 Size and tests

- `migrate.c`: side table and recording in `prepass()` (about 60 lines),
  newline preservation for blocks (about 5), form normalisation (about 30),
  guard plus reinsertion plus verification (about 120). `main.c`: memstream
  buffering (about 20).
- `test/migrate/`: add a `.legacy.tk`/`.expected` pair for each `col_kind`
  and each comment form.
- C058: invert case 2 so that legacy source with comments migrates, compiles
  and keeps every comment. Add a case that forces the line-map guard to trip
  (a `+` chain split across lines) and asserts a named decline. The tree-wide
  sweep stays unchanged.

## 4. Why this was not implemented in this batch

The change touches every stage in §1 because of the guard, and the retry
path because of the buffering. It also needs a placement policy for
`INTERIOR` comments. That makes it a story of its own, not a patch.
The interim guard ends the silent loss now, which was the defect's harm.
Real preservation brings back a feature: migrating commented legacy code
without the override.

## 5. Open decisions (owner)

1. `INTERIOR` comments: move them to the end of the line with a note, or
   decline?
2. Declined runs reuse exit code `ECOMPILE` (1), because `tkc_limits.h`
   defines only 1, 2 and 3. A dedicated "declined, nothing written"
   code would let a migration driver tell *nothing to do* apart from
   *this failed*.
3. W1020's `//` message says toke "has no comment syntax", but the lexer
   accepts `(* *)` silently and C041 treats `(* *)` in valid v0.4 as
   something to preserve. Is `(* *)` part of the language, or only tolerated?
   If only tolerated, "preservation" should mean moving comments to the
   companion `.tkc` file, not re-inserting them into the source.
4. Both declines (127.57's and this one) print plain text to stderr, not a
   structured diagnostic record. AGENTS.md §3.1 asks for a JSON record on
   every error path, and a decline needs a code to have one.

## 6. As implemented (127.161, 127.197, 127.198)

- **§5.1 → move.** An `INTERIOR` comment goes to the end of its line, after
  the code and before any trailing comment, with a note on stderr:
  `tkc: --migrate: <file>:<line>: note: comment moved to the end of its line`.
- **§5.2/§5.4 → E9026, exit 4.** Every decline echoes the source, emits one
  E9026 record (`docs/reference/errors.md`) and exits `TKC_EXIT_DECLINED`.
- **§5.3 → official.** `(* *)` is toke's comment form (spec v0.4 §H), so
  comments are re-inserted into the source, not moved to a companion file.
- **Placement is per line, from the source line.** Rather than a `col_kind`
  per comment, each source line is split into code and comment bytes: the
  source text before the first code (indentation and leading comments)
  replaces the output line's indentation, the source text after the last
  code replaces its trailing space, and lines holding only comments are
  written back verbatim (their output line must be blank). A multi-line
  comment is therefore the trailing part of its first line, the leading part
  of its last, and own-line in between, so it always returns exactly.
- **Guards** (each declines naming the cause): unclosed `(*`; a `//`/`#`
  text holding `(*` or `*)`; the retry path (§3.3, "decline" option); a line
  count that changed; code on a comment-only line; and the §3.2 verify step —
  same tokens with and without the comments, and the same comment count.
- **`TKC_MIGRATE_FORCE=1`** still runs the stripping migrator, so C041's
  negative control keeps its witness.
- `prepass()` also stopped gluing tokens: a one-line block comment between
  two word characters becomes a space (`a(*x*)b` used to become `ab`).
- Tests: C058 (every comment form and position, an interior comment in each
  rewrite of §1, both guards, the tree-wide sweep), C711 (the record).
