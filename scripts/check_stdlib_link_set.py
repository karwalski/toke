#!/usr/bin/env python3
"""
check_stdlib_link_set.py — the directory and the manifest must agree on what
the standard library is (story 127.99).

Two builds of the same program disagreed.  `tkc --out` links a curated list:
the per-module table in src/stdlib_deps.c (which `tkc --emit-deps` prints),
with find_stdlib_sources() in src/llvm.c as the link-all fallback.  The
website deploy instead globs `stdlib/*.c` on the server.  A rename (136.17)
left llmtool.c beside llm_tool.c and a superseded stub of
tk_infer_load_streaming in infer.c beside the real one in infer_stream.c.
The curated build never saw either, because it never named them; the globbing
build failed at link with multiple definitions and blocked the deploy.

THE MANIFEST IS AUTHORITATIVE.  src/stdlib_deps.c is the compiler's own
statement of what each module is built from, and it is what ships in every
binary tkc produces.  A glob is a guess.  This gate makes the directory a
faithful derivation of the manifest, so the guess can no longer be wrong:

  1. No two .c files in src/stdlib/ may define the same external symbol.
     This is exactly the property a globbing link depends on.
  2. Every .c file in src/stdlib/ must be named by the manifest — by
     src/stdlib_deps.c, by find_stdlib_sources() in src/llvm.c, or by
     STDLIB_SRCS in the Makefile.  A file the manifest never names is either
     stale or unreachable; both are the defect this gate exists to catch.

OPT_IN_BACKENDS are the sole exception: their entire contents sit inside an
#ifdef that no default build defines, so they compile to an empty object and
contribute no symbol to a glob link.  Adding to this list requires the same.

Exit 0 when both hold, 1 otherwise.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
STDLIB = ROOT / "src" / "stdlib"

# Wholly #ifdef-guarded opt-in backends: no symbols unless the flag is given.
OPT_IN_BACKENDS = {"db_postgres.c", "db_mysql.c"}

MANIFEST_FILES = [
    (ROOT / "src" / "stdlib_deps.c", r"([A-Za-z0-9_]+\.c)"),
    (ROOT / "src" / "llvm.c", r"%s/([A-Za-z0-9_]+\.c)"),
    (ROOT / "Makefile", r"src/stdlib/([A-Za-z0-9_]+\.c)"),
]

DEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
SKIP_PREFIX = ("static", "typedef", "extern", "#", "/", "*", "}")


def external_defs(path):
    """External function definitions at column 0 in one translation unit.

    A definition, not a prototype: the token after the closing paren is '{',
    not ';'.  Definitions guarded by #if/#else inside ONE file are counted
    once — only one branch survives preprocessing, so they cannot collide.
    """
    names = set()
    lines = path.read_text(errors="replace").splitlines()
    for i, line in enumerate(lines):
        if not line or line[0] in " \t":
            continue
        if line.startswith(SKIP_PREFIX):
            continue
        m = DEF_RE.match(line)
        if not m:
            continue
        blob = " ".join(lines[i:i + 6])
        after = blob.split(")", 1)
        if len(after) < 2:
            continue
        rest = after[1]
        if "{" not in rest:
            continue
        if ";" in rest.split("{", 1)[0]:
            continue
        names.add(m.group(1))
    return names


def main():
    sources = sorted(STDLIB.glob("*.c"))
    if not sources:
        print(f"FAIL: no .c sources under {STDLIB}")
        return 1

    failures = 0

    # ── 1. cross-file symbol collisions ──────────────────────────────────
    owner = {}
    collisions = {}
    for path in sources:
        for name in external_defs(path):
            if name in owner:
                collisions.setdefault(name, {owner[name]}).add(path.name)
            else:
                owner[name] = path.name
    if collisions:
        print(f"FAIL: {len(collisions)} symbol(s) defined in more than one "
              f"src/stdlib/*.c — a globbing link cannot succeed:")
        for name in sorted(collisions):
            print(f"  {name:<40} {', '.join(sorted(collisions[name]))}")
        failures += 1
    else:
        print(f"PASS: {len(sources)} sources, {len(owner)} external "
              f"definitions, no symbol defined twice")

    # ── 2. every source is named by the manifest ─────────────────────────
    named = set()
    for path, pattern in MANIFEST_FILES:
        if path.exists():
            named |= set(re.findall(pattern, path.read_text(errors="replace")))
    orphans = sorted(p.name for p in sources
                     if p.name not in named and p.name not in OPT_IN_BACKENDS)
    if orphans:
        print(f"FAIL: {len(orphans)} source(s) in src/stdlib/ that no manifest "
              f"names — stale, or unreachable to every curated build:")
        for name in orphans:
            print(f"  {name}")
        failures += 1
    else:
        print(f"PASS: every source is named by the manifest "
              f"({len(OPT_IN_BACKENDS)} opt-in backends excepted)")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
