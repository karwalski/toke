---
title: Python view (tkc --python-view)
slug: python-view
section: compiler
order: 7
---

`tkc --python-view FILE...` prints a read-only, Python-like rendering of each
toke source file to stdout, for reviewers who read Python and not toke.

```sh
tkc --python-view examples/mortgage/calc.tk
tkc --python-view src/a.tk src/b.tk > review.txt
```

**Display only.** The output is not executable and cannot be turned back into
toke. toke semantics apply to everything it shows: integer `/` truncates,
fixed-width integers keep their toke names (`u8`, `i32`), and `T!E` is shown
as `Result[T, E]`. Every rendering opens with a short header that says so.

What you can rely on:

- **Every construct is shown, in source order and with the same nesting.**
  Nothing is summarised.
- **The output is deterministic.** It depends only on the file's contents and
  its file name, so two runs give the same bytes and two versions of a file
  diff cleanly.
- **It runs on any file that parses.** No type check runs, so a file with a
  type error still renders.
- **A file that does not parse prints nothing.** It exits 1 with the same
  structured diagnostics as `--check`.

Comments are not shown. In the output, `_fnN`, `_ifN` and `_mtN` are a
closure, `if` or `mt` that was lifted out of an expression into a local
function, printed just before the statement that uses it.

The full rendering table, and why the view is display only, are in
[96.7 — tkc --python-view](/docs/decisions/96.7-python-view/).
