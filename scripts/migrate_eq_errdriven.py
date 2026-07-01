#!/usr/bin/env python3
"""migrate_eq_errdriven.py — v0.4 equality `=` -> `==` for files that no longer parse.

`migrate_eq.py` is AST-driven (`tkc --dump-ast`), which fails once a file contains a
bare `=` equality because that is now a hard parse error (E2002) — so the AST never
builds. This migrator instead drives off the compiler's own E2002 diagnostics:

  E2002 "`=` is assignment; use `==` for equality"

is emitted at the EXACT byte offset of every `=` that sits in expression/comparison
position — i.e. exactly the equality operators, and never assignment/binding/decl/
loop-step `=` (those are statement position and compile clean). So the diagnostic set
is a precise, false-positive-free list of the operators to double.

Algorithm (safe against the offset-shift corruption of the earlier text fixer):
  repeat:
    run `tkc --check`; collect E2002 span_start offsets from THIS fresh pass
    stop if none
    insert one `=` after each offset, right-to-left (earlier offsets stay valid)
    write; re-run fresh (never reuse a previous pass's offsets)
  abort if a pass makes no progress (guards against loops).

Offsets are BYTE offsets — operate on bytes throughout.

Usage: python3 scripts/migrate_eq_errdriven.py [--write] <file.tk> [...]
"""
import sys, json, subprocess, os

TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))
EQ = ord('=')
MAX_PASSES = 40


def _e2002_offsets(path):
    """Byte offsets of every E2002 (`=` used as equality) in the file's current state."""
    r = subprocess.run([TKC, path, "--check"], capture_output=True, text=True)
    offs = []
    for line in (r.stdout + "\n" + r.stderr).splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("error_code") == "E2002":
            o = d.get("span_start", d.get("pos", {}).get("offset"))
            if isinstance(o, int):
                offs.append(o)
    return sorted(set(offs), reverse=True)


def migrate(path):
    src = bytearray(open(path, "rb").read())
    total = 0
    for _ in range(MAX_PASSES):
        # write current state so the compiler sees it
        open(path, "wb").write(src)
        offs = _e2002_offsets(path)
        if not offs:
            return src, total
        progressed = 0
        for o in offs:                      # right-to-left
            if 0 <= o < len(src) and src[o] == EQ and (o + 1 >= len(src) or src[o + 1] != EQ):
                src.insert(o + 1, EQ)
                progressed += 1
        total += progressed
        if progressed == 0:                 # nothing we can safely fix -> stop (avoid loop)
            return src, total
    return src, total


def main():
    write = "--write" in sys.argv
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    total = 0
    for p in files:
        original = open(p, "rb").read()
        out, n = migrate(p)
        # migrate() writes as it probes; restore original unless --write
        if not write:
            open(p, "wb").write(original)
        total += n
        if n:
            print(f"{p}: {n} equality '=' -> '=='")
    print(f"total: {total} equality operators{' (written)' if write else ' (dry run)'}")


if __name__ == "__main__":
    main()
