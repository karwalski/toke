#!/usr/bin/env python3
"""migrate_eq.py — A3 (Epic 116): surgically rewrite equality `=` -> `==`.

toke v0.4 splits `=` (assignment/binding) from `==` (equality). This migration
is AST-driven and layout-preserving: it parses each file with `tkc --dump-ast`,
finds every NODE_BINARY_EXPR whose operator is a lone `=` (equality — `<=`/`>=`/
`!=` start with other bytes, and `==` is skipped), and inserts a second `=` at
exactly that operator position. Binding/assignment/declaration/loop `=` are
untouched (they are not BINARY_EXPR nodes), so no reformatting and no risk to
the surrounding code.

Offsets from the compiler are BYTE offsets, so this operates on bytes throughout
(a char-indexed version silently mis-migrated files with non-ASCII comment chars
like `—`/`→`).

Usage:
    python3 scripts/migrate_eq.py [--write] <file.tk> [<file.tk> ...]
Without --write it reports the count per file (dry run).
"""
import sys, json, subprocess, os

TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))
EQ, LP, SEMI = ord('='), b'lp', ord(';')


def _in_three_clause_loop_header(src, o):
    """A `=` that is the init/step of a 3-clause `lp(init;cond;step)` is an
    ASSIGNMENT, not equality — the `=` ambiguity can make the parser represent a
    loop step as an equality BINARY_EXPR, so guard textually. True iff `o` sits
    directly inside an `lp(...)` header that contains a top-level `;` (3-clause).
    A while-guard `lp(x==5)` has no top-level `;` and is not excluded. `src` is
    bytes."""
    depth = 0; i = o - 1
    while i >= 0:
        c = src[i]
        if c == ord(')'): depth += 1
        elif c == ord('('):
            if depth == 0: break
            depth -= 1
        i -= 1
    if i < 0:
        return False
    op = i
    j = op - 1
    while j >= 0 and src[j] in b' \t': j -= 1
    k = j
    while k >= 0 and chr(src[k]).isalnum(): k -= 1
    if src[k + 1:j + 1] != LP:
        return False
    depth = 0; i = op
    while i < len(src):
        c = src[i]
        if c == ord('('): depth += 1
        elif c == ord(')'):
            depth -= 1
            if depth == 0: break
        elif c == SEMI and depth == 1:
            return True
        i += 1
    return False


def equality_offsets(src, ast):
    """`src` is bytes; returns byte offsets of lone equality `=` operators."""
    offs = []
    def walk(n):
        if not isinstance(n, dict):
            return
        if n.get("kind") == "BINARY_EXPR":
            o = n.get("pos", {}).get("offset")
            if o is not None and 0 <= o < len(src) and src[o] == EQ \
               and (o + 1 >= len(src) or src[o + 1] != EQ) \
               and not _in_three_clause_loop_header(src, o):
                offs.append(o)
        for v in n.values():
            if isinstance(v, list):
                for c in v:
                    walk(c)
            elif isinstance(v, dict):
                walk(v)
    walk(ast)
    return sorted(set(offs), reverse=True)


def migrate(path):
    src = open(path, "rb").read()
    r = subprocess.run([TKC, path, "--dump-ast"], capture_output=True, text=True)
    try:
        ast = json.loads(r.stdout)
    except Exception:
        return None, "ast-parse-failed"
    offs = equality_offsets(src, ast)
    if not offs:
        return src, 0
    b = bytearray(src)
    for o in offs:               # right-to-left keeps earlier offsets valid
        b.insert(o + 1, EQ)
    return bytes(b), len(offs)


def main():
    write = "--write" in sys.argv
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    total = 0
    for p in files:
        out, n = migrate(p)
        if out is None:
            print(f"SKIP {p}: {n}")
            continue
        total += n
        print(f"{p}: {n} equality '=' -> '=='")
        if write and n:
            open(p, "wb").write(out)
    print(f"total: {total} equality operators{' (written)' if write else ' (dry run)'}")


if __name__ == "__main__":
    main()
