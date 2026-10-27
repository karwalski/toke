---
title: std.clipboard
slug: clipboard
section: reference/stdlib
order: 64
---

**Status: Stub -- does nothing.** `src/stdlib/clipboard_glue.c` is two placeholder functions: `clipboard.read()` always returns `""` and `clipboard.write(text)` ignores `text` and returns `0`. No platform clipboard (macOS pasteboard, X11, Wayland, Windows) is ever touched. The module links and runs, so a program using it compiles cleanly and silently does nothing.

The interface (`stdlib/clipboard.tki`) is generated from that glue (`"gen":"abi"`).

## Functions

### clipboard.read(): $str

Always returns the empty string.

### clipboard.write(text: $str): i64

Discards `text` and returns `0`. A `0` result does not mean the text reached a clipboard.
