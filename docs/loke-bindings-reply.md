# Reply to loke — the native bindings round is not complete

**From:** toke
**To:** the loke project
**Date:** 2026-09-20
**Re:** `loke/docs/archive/ooke-bindings-required.md` (archived 2026-09-19, marked COMPLETE, 306/306)
and its successor entry in `loke/UPSTREAM.md` v1.0
**Verified against:** toke `03a56a7` (`main`), and the as-archived state `ba67f2a`
**Status:** action needed this week — see [What we are asking for](#what-we-are-asking-for)

---

## The position, stated before anything is asked

`UPSTREAM.md` retired four documents and re-verified three of them symbol by symbol. The bindings
document was the one exception: it was carried forward on the ooke team's COMPLETE marking rather
than re-checked against a current checkout, and its row reads *"Already marked COMPLETE by the ooke
team; retained as a record of a process that worked."*

We re-checked it. The round is not complete, and the exemption is why nobody saw that. This reply
exists because `UPSTREAM.md`'s own closing rule applies in this direction too:

> A gap document that is never re-verified becomes misinformation.

So does a completion document.

**The pattern itself was right.** The request stated the need, named the constraints, and gave the
interface it expected — and in three of the places where the published interface and the request
disagree, **the request is the side that was correct** (see [3](#3-workarounds-you-can-use-today)
and [4](#4-the-part-that-must-land-an-arity-mismatch-corrupts-silently)). Nothing below is a
criticism of how the ask was written. It is a correction to what was delivered against it.

### Method

Every figure here was derived by reconciling each module's published interface file
(`stdlib/*.tki`) against the C wrapper symbols the compiler actually resolves
(`src/stdlib/*_glue.c`), and by compiling and running real consumers. Documentation was not treated
as evidence.

### The headline numbers

Against the state you archived (toke `ba67f2a`), across the eight modules behind the nine capability
groups — disk-streaming inference is part of `std.infer`, not a tenth module:

| Measure | As archived (`ba67f2a`) | Today (`03a56a7`) |
|---|---|---|
| Documented functions in the published interfaces | 53 | 45 (+8 withdrawn) |
| **With no implementation symbol at all** | **21** | **9** |
| **With an arity mismatch against the implementation** | **9** | **7** |
| Modules unusable as published | 4 | 0 |

The arity number is the dangerous one, and it is the subject of section 4.

---

## 1. What did not work

Four of the nine groups could not be used at all as published. Two of the four are P1 in your own
priority table.

### 1.1 `std.webview` (P1, your story F1.2) — no implementation existed

There was no `webview_glue.c`. The file was present and empty. All eight functions the interface
declared — `open`, `close`, `settitle`, `onclose`, `registerhandler`, `evaljs`, `runeventloop`,
`isavailable` — had no symbol behind them, and the platform frameworks were absent from the link
line. This was not a partial implementation: it was an entry in an interface file with nothing
behind it. Any consumer would have failed at link with mangled `_w` symbol names, not with a
diagnostic.

**It has been withdrawn rather than stubbed** (toke `8a0d879`). `i=wv:std.webview;` now fails at the
import line with `E2030 standard-library module 'std.webview' not found`, naming the module and the
line, instead of failing at link. The 72.4 design is retained in `docs/stdlib/webview.md` as a record,
marked WITHDRAWN and explicitly not to be written against.

**Why withdrawn and not built,** since F1.2 depends on it and you are entitled to the reasoning:

- Two of the five calls you specified — `on_close` and, in the full interface,
  `registerhandler` — need a **callback ABI that toke does not have**. A complete implementation
  would still not satisfy your consumer.
- It cannot be verified in this environment without putting a window on screen and entering an
  application event loop.
- The handle type needs its own ABI decision, and two open stories leave the JavaScript bridge
  unhardened.

An honest absence beats a documented module that cannot be called. **F1.2 is blocked and should be
recorded as blocked**, not as unblocked. If the desktop shell is on your near-term path, tell us and
we will scope the callback ABI as a first-class piece of work rather than as a webview detail.

### 1.2 `std.keychain` (P1, your story F1.5) — compiled, never linked

`stdlib_deps.c` carried **empty link flags** for the module, so every consumer failed on roughly
twenty undefined `_kSec*` / `_CF*` symbols from the Security.framework path. Separately,
`keychain.isavailable` — exported by the interface since story 72.1.1 — had no symbol at all.

**Fixed** (toke `add756d`). Platform-selected flags now populate that entry: Security +
CoreFoundation on Apple, advapi32 on Windows, empty elsewhere (where the module is a no-op stub and
`isavailable()` is already false). `isavailable` has its wrapper.

A second defect was found while fixing the first, and it is worth your knowing about because it
would have hit any future binding: the flag appender **deduplicated word by word**, so it dropped
the second `-framework` of a pair and produced `-framework Security CoreFoundation`, which clang
reads as a filename. It now treats `-framework X` as a unit.

**The proof we think matters:** write, read-back, delete and absent-key run as **four separate
processes of the same binary**, so the secret demonstrably reached the operating system's credential
store and came back, rather than surviving in process memory — which a single-process test cannot
distinguish. 6 of 6 pass, fresh service name per run with a cleanup trap, no system dialog raised,
nothing left behind.

### 1.3 `std.secure_mem` (P1, your story F6.4) — unimportable under its published name

`std.secure_mem` is rejected by the lexer. Underscores are outside toke's naming profile (decision
113.2a), so the name as published **cannot be written in toke at all**.

The capability itself was fine. `alloc` / `write` / `read` / `wipe` round-trip correctly, and always
did. **You were simply never given the name that works.**

**Fixed** (toke `0fa50e5`): renamed to `std.securemem` across the interface, documentation and spec,
with the missing `isavailable` wrapper added. See [3.1](#31-secure-memory-use-stdsecurmem-today) for
the change to make today. The old `secure_mem.c` was confirmed dead — byte-identical to its
replacement apart from a pre-113.2a symbol prefix, with nothing including its header — and removed.

### 1.4 `std.vecstore` (P2, your stories F4.4 and F6.3) — crashed when called as documented, and persisted nothing

Two independent defects, both of which your code would have hit on first use.

**It crashed.** `vecstore.search` returned a bare array of identifier strings while the interface
declared an array of structured `SearchResult`. That **compiles**, and then reading a field off a
result — `r.get(0).id` — reads the first eight bytes of the identifier text as a pointer and dies
with SIGBUS. It also discarded `score` and `payload`, which are the only reason to run a similarity
search at all, and hard-coded `min_score` to `0.0`, silently ignoring the argument you passed.

**It persisted nothing.** The save routine and the on-disk format have worked correctly since story
72.5.3 — the write path never discarded anything. But `collection_save`'s only caller is
`vecstore_close`, and **the `close` wrapper did not exist**. So toke had no reachable flush path:
every upsert accumulated in memory and died with the process. A missing wrapper, not a missing
feature — which is a much smaller fix and a considerably worse bug, because everything looked
implemented.

**Both fixed** (toke `0fa50e5`), with `close` and `count` added. The proof re-executes the binary
between every phase — write-and-close, read in a process that never saw the write, filtered search,
delete, and delete-persisted — **five separate processes**, plus a direct check that the store file
exists and is non-empty, and the file shrinks 112 → 68 bytes across the delete, so deletions reach
disk too. 8 of 8 pass.

**One durability caveat you must design around.** Data now reaches disk, but **only at `close`**. A
process that crashes, is killed, or forgets to close loses every upsert since it opened. For a
semantic cache that may be acceptable; for routing examples it may not. The contract is not yet
decided — if you need a `flush` call or a write-threshold flush, say so and it will be specified
rather than guessed.

---

## 2. What is fixed already — pull, do not wait

All on toke `main`. Pin a commit; see [section 5](#5-an-addressing-correction-this-surface-is-tokes-not-ookes).

| Commit | Date | What it closes |
|---|---|---|
| `add756d` | 2026-09-19 | `std.keychain` links and round-trips; `isavailable` implemented; `-framework` pair handling fixed |
| `0fa50e5` | 2026-09-19 | `std.vecstore` reaches disk (`close`, `count`); `search` returns real structs and honours `min_score`; `std.secure_mem` → `std.securemem` with `isavailable` |
| `8a0d879` | 2026-09-19 | `std.webview` withdrawn — import-time error instead of link-time undefined symbols |
| `72a9683` | 2026-09-19 | The compiler now checks a call against the implementation that defines it (see section 4) |

Still open, and not yet fixed: the nine missing symbols and seven arity mismatches listed in
sections 4 and 6. Those are filed as toke stories 136.6 and 136.7.

---

## 3. Workarounds you can use today

These need no toke change. They work on the commit you already have.

### 3.1 Secure memory: use `std.securemem` today

```
i=sm:std.securemem;     (* not std.secure_mem — the lexer rejects the underscore *)
```

The call names are `securemem.alloc`, `.write`, `.read`, `.wipe`, `.sweep`, `.isavailable`. Argument
lists are exactly as your request specified: `alloc(size_bytes:i32; ttl_seconds:i32)`,
`write(buf; data:str)`, `read(buf)`, `wipe(buf)`, `sweep()`. **Only the module name changes.** F6.4
is unblocked now.

### 3.2 Vector store: call it the way your request specified, not the way the interface showed

Your request asked for:

```
pub f=upsert(col;id:str;embedding:@(f32);payload:str):bool          (* 4 arguments *)
pub f=search(col;query:@(f32);top_k:i32;min_score:f64):@($search_result)   (* 4 arguments *)
```

The published interface had drifted to **five** arguments on both, carrying an explicit `dim`. **Your
version was right.** That interface had been transcribed from the C header, where an explicit
dimension is necessary because C arrays carry no length — but toke arrays do, and the glue has
always read the length from the array header. The `.tki` was the side that drifted.

So: **call `upsert` and `search` with four arguments, dropping `dim`.** That form works today and is
now what the interface declares.

**One change from what you asked for, and you must apply it:** the element type is **`@(f64)`, not
`@(f32)`**. That is what the glue decodes, and it is now what the interface declares. Passing an f32
array is exactly the class of mismatch described in the next section — it will not fail, it will
misread.

### 3.3 TLS: no workaround exists

`tls.listen` and `tls.connect` do not exist under those names. **The names in your original request —
`listen_tls` and `connect_tls` — are the ones the implementation actually uses** (`listentls`,
`connecttls` after the no-underscore convention). The published interface renamed them to
`tls.listen` / `tls.connect`, and those declare nothing. Because a member that neither the interface
nor the implementation declares is now rejected outright, there is no spelling you can write today
that reaches them. F8.2 stays blocked until 136.7 lands.

---

## 4. The part that must land: an arity mismatch corrupts silently

**This is the most important paragraph in this document.**

A call whose argument count disagrees with the symbol it invokes **compiles cleanly, links cleanly,
runs, and produces wrong results.** There is no diagnostic, no link error, no crash, and no
indication at runtime. A missing symbol is loud — you get a linker error naming a mangled name, and
you find it in minutes. An arity mismatch is quiet: the extra argument is never read, or a register
holding something else is read as if it were your argument.

Until toke `72a9683` landed on 2026-09-19, **nothing in the compiler checked a call against the
interface that declared it.** That is the root enabler, and it is now fixed — the compiler validates
a call against the implementation wherever it knows the glue symbol, because that arity is what
actually decides whether a call corrupts. It found 82 real call sites across 24 files in toke's own
program library on its first run. **Seven of those build, link and run while corrupting.**

### What this means for code you have already written

Any loke code written against the following signatures should be treated as suspect until you have
looked at the call site. These are verified at toke `03a56a7` by reading the interface and the
implementing wrapper directly:

| Call | Interface says | Implementation takes | Consequence |
|---|---|---|---|
| `infer.load` | 2 (`path`, `opts`) | 1 (`path`) | Your `$infer_opts` is silently ignored |
| `infer.generate` | 3 (`h`, `prompt`, `max_tokens`) | 2 (`h`, `prompt`) | **`max_tokens` is silently ignored** |
| `infer.loadstreaming` | 2 (`dir`, `opts`) | 1 (`dir`) | Your `$stream_opts` is silently ignored |
| `mlx.generate` | 3 (`m`, `prompt`, `max_tokens`) | 2 (`m`, `prompt`) | **`max_tokens` is silently ignored** |
| `mdns.advertise` | 1 (`$service_record`) | 3 (`type`, `name`, `port`) | Reads two registers that hold nothing meaningful |
| `mdns.browse` | 2 (`type`, callback) | 1 (`type`) | Your callback is never registered |
| `tls.genselfsigned` | 2 (`cn`, `valid_days`) | 1 (`cn`) | Certificate validity is not what you set |

Two more — `vecstore.upsert` and `vecstore.search` — were in this list as archived and are now fixed;
see [3.2](#32-vector-store-call-it-the-way-your-request-specified-not-the-way-the-interface-showed).

Note the shape of these. `infer.generate` and `mlx.generate` do not fail when `max_tokens` is
dropped — they generate, and return plausible text of the wrong length. `mdns.advertise` advertises
*something*. `tls.genselfsigned` produces a certificate. Every one of these returns a believable
answer.

**What to do:** grep your tree for these seven calls and read each site, rather than assuming your
code is fine because it runs and your tests are green. If you are on a toke checkout at or past
`72a9683`, rebuilding will now raise a diagnostic at each of them — that is the fastest audit
available and we suggest it before anything else in this document.

---

## 5. An addressing correction: this surface is toke's, not ooke's

The request was addressed to the ooke team, and all nine modules live in **toke's** standard library.
ooke exposes none of them. This matters practically, in two ways:

1. **Every binding pins to a toke commit, not to an ooke version.** There is no ooke release that
   carries these; a version constraint against ooke will never resolve to the fix.
2. **Every consumer needs an explicit stdlib include path** or the compiler cannot locate the
   directory at all.

Recommended pin for the fixes in section 2: toke `03a56a7` or later.

---

## 6. The reverse direction — what loke imports that we do not publish

Verified by reading loke's sources directly. Read-only throughout; nothing in `~/loke/` was modified.

### 6.1 Three standard-library modules that do not exist

| Import | Sites | Status |
|---|---|---|
| `std.shell` | 2 (`packages/cli/src/updater.tk`, `packages/cli/src/sign.tk`) | **No such module in toke** |
| `std.arr` | 2 (`packages/core/src/feedback/draft.tk`, `packages/cli/src/code_profile.tk`) | **No such module in toke** |
| `std.assert` | 4 (all in `loke/_archived-tests/moke/tests/`) | **No such module in toke** |

**We are asking rather than building.** These were either promised and never built, or written
against an expectation that was never real, and the answer decides whose story this is. We are not
going to create three standard-library modules on the strength of an import statement — if
`std.shell` is meant to be a process-execution surface, that is a design conversation, not a
transcription job. `std.arr` may well be covered by an existing module under a different name, the
way `str.nowiso8601` turned out to be `std.time` in your own re-verification. Tell us what you
expected each to do and we will either point you at what exists or file it.

The four `std.assert` sites are in `_archived-tests/`, so they may be dead. Worth confirming.

### 6.2 Five ooke modules imported across 32 sites, in no request

`ooke.template`, `ooke.handlers`, `ooke.serve`, `ooke.context` and `ooke.config`, at 32 import sites
across loke's packages.

None of these appears in any request, which means ooke does not know a downstream consumer exists and
is free to change them. That is the kind of coupling that breaks at the worst possible moment.
Either these become a supported surface with a stability commitment, or they are internal and loke
needs to stop importing them. Both answers are workable; the current state is not. Tell us which you
want and it will be recorded accordingly.

---

## 7. The 306-test figure needs verifying

The archived document states 306/306 passing, broken down by module:
`std.keychain` 23, `std.infer` 32, `std.secure_mem` 37, `std.webview` 16, `std.vecstore` 63,
`std.mlx` 35, `std.infer` streaming 34, `std.mdns` 29, `std.tls` 37.

That figure cannot be reconciled with the state of the code, and we are raising it rather than
quietly working around it.

- **`std.webview` is credited with 16 passing tests** for a module that had no implementation file at
  all. Sixteen tests cannot have exercised eight functions that have no symbols.
- **`std.keychain` is credited with 23** for a module that could not link in any consumer.
- **`std.secure_mem` is credited with 37** for a module whose name the lexer rejects, so no toke
  program could import it.

The concrete evidence, which is why we think this is a build-wiring problem rather than a
disagreement about counting:

> **Three C test files in this surface have no make target and have therefore never executed.**
> `test/stdlib/test_keychain.c`, `test/stdlib/test_webview.c` and `test/stdlib/test_securemem.c`
> existed in the tree with no rule invoking them. `test_securemem.c` has since been given a target
> (toke `0fa50e5`); **`test_keychain.c` and `test_webview.c` still have none as of `03a56a7`.**

A test file that is never built cannot contribute a passing count. We have seen this exact shape
twice more in toke this month — a suite of zero-byte test files reporting success, and 157
assertions that could not fail — so we are treating "green, and also unlinkable" as evidence about
the harness rather than about the code.

**What we are not saying:** that anyone inflated a number. The likeliest explanation is that the
count aggregates a suite whose build wiring silently skips the modules it claims to cover. But the
figure is now load-bearing — it is the reason the document was archived and the reason
`UPSTREAM.md` exempted it from re-verification — so it needs to be established rather than assumed.

**A specific clue to start from:** ooke's own end-to-end test **explicitly excludes two of these
modules from its stdlib build.** `test/e2e/test_serve.sh:30` filters `secure_mem` and `infer_stream`
out of the sources it compiles. Those are the groups credited with 37 and 34 passing tests — 71 of
the 306 — and ooke's own harness declines to build them. That exclusion is a direct statement about
what the suite does not cover.

---

## 8. What you asked for next has started

Your `docs/toke-libraries-required.md` (v1.0, 2026-09-19) is being treated as the specification, and
work began before this reply was written. A full item-by-item answer — accepted, accepted with a
changed interface, deferred, or refused, with reasons — follows separately. The accurate state today:

### Shipped

| Ask | Status | Commit |
|---|---|---|
| `std.zip` — read-only archive access | **Shipped**, all four requested calls as specified, plus `close` | `8efc79f` (2026-09-20) |
| `std.csv` hardening | **Shipped**, interface as you specified | `4c3e689` + `c3e7d4f` (2026-09-20) |
| Binary file read/write | **Shipped** — not requested, but required by your formats | `6dba991` (2026-09-20) |

**`std.zip`** ships `zip.open` (over bytes), `zip.openfile` (over a path), `zip.entries`, `zip.read`,
`zip.close` and `zip.lasterr`. The six security rules — path traversal in relative, absolute and
backslash forms, a cumulative-size cap, a compression-ratio cap, and an entry-count cap — were each
proven by **disabling them first** and confirming that all six malicious fixtures are then accepted,
before restoring them and confirming each is refused by its exact message. 19 of 19 pass, clean under
the address and undefined-behaviour sanitisers, and a fresh clone builds and passes.

**`std.csv`** ships `sniff`, `opts`, `readeropts`, `dialect`, `atend`, `raggedreport`, `close` and
error accessors, over genuine bytes rather than a NUL-truncated string. All six of your requirements
are covered: byte-order mark, non-UTF-8 detection, delimiter sniffing, RFC 4180 quoting across line
breaks, a caller-chosen ragged-row policy, and raw-string preservation with no coercion. 62 of 62
pass, including eight negative controls built into the suite — with the checks relaxed, the fixtures
assert that the BOM **is** kept, that mojibake **does** pass, that semicolons are **not** detected
and that an unterminated quote **is** accepted, so a future change that weakens a check fails the
suite in the opposite direction.

**Binary file read** (`file.readbytes` / `file.writebytes`) was added because toke had none at all:
the existing read returns a NUL-terminated string, and every archive and spreadsheet has a NUL in it.
Nine failure kinds are distinguishable, and an empty file returns a real zero-length array marked ok,
never a bare zero — so a reader can tell an empty file from a failed read. The suite carries a
permanent negative control asserting that the old string read **still truncates**.

### Not yet started

`std.pdf`, `std.xlsx`, the `std.image` additions, the platform OCR binding and `toke-ocr` are all
open. `std.pdf` is blocked on a decision about whether a C shim around a C++ library is acceptable —
**your argument against Tesseract is accepted and recorded**: toke is a C99 project with zero C++
files, and vendoring a C++ dependency is an architectural change to what toke is, not an addition.
That decision is the reason the OCR answer is not the obvious one, and it is being written down as a
decision record rather than left as a property of the current Makefile.

**Two constraints you should know about before you plan against these.** Binary reads are capped at
64 MiB, because a byte array costs 8 bytes per byte — PDF and spreadsheet inputs will exceed that,
and a bounded or streaming read is needed before `std.pdf` and `std.xlsx` are usable at scale. Your
own requirement for per-page incremental processing is the same need seen from the other side.

We also note your standing position that **nothing in this round blocks MK21**, which ships CSV only
behind a sidecar. We are treating Epic 135 as a retire-the-sidecar programme, not a critical path.
Correct us if that has changed.

---

## What we are asking for

In priority order.

1. **Audit your call sites against the seven signatures in [section 4](#4-the-part-that-must-land-an-arity-mismatch-corrupts-silently).** This is the only item
   where waiting has a cost that compounds — wrong results already written to disk do not announce
   themselves. Building against toke `72a9683` or later turns this audit into a compiler run.
2. **Record F1.2 (webview) as blocked, not unblocked**, and tell us whether the desktop shell is on
   your near-term path.
3. **Apply the two workarounds in [section 3](#3-workarounds-you-can-use-today)** — `std.securemem`, and the four-argument
   `@(f64)` vector-store calls. F6.4, F4.4 and F6.3 are unblocked today.
4. **Tell us what `std.shell`, `std.arr` and `std.assert` were meant to be** ([6.1](#61-three-standard-library-modules-that-do-not-exist)). The answer decides whose
   story this is and we are not guessing at it.
5. **Decide whether the five ooke modules are a supported surface** ([6.2](#62-five-ooke-modules-imported-across-32-sites-in-no-request)).
6. **Re-establish or correct the 306 figure** ([section 7](#7-the-306-test-figure-needs-verifying)), and amend the archived document so the next
   reader is not misled. Start from the two C test files that still have no build target.
7. **Tell us the durability contract you need from the vector store** ([1.4](#14-stdvecstore-p2-your-stories-f44-and-f63--crashed-when-called-as-documented-and-persisted-nothing)) — flush at close is
   what exists today.

Items 1, 3 and 7 are things you can act on this week without waiting for us.

---

## Where this is tracked on our side

toke Epic 136 (this surface), toke Epic 135 (the document and image libraries), story 134.11 (the
verification that produced these findings). Open rows relevant to you: **136.6** — reconcile the
remaining seven arity mismatches; **136.7** — the nine functions with no symbol; **136.8** — the three
standard-library modules loke imports; **136.9** — the five ooke modules; **136.10** — the test-count
verification; **136.15** — the vector store's durability window.
