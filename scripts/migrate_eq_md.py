#!/usr/bin/env python3
"""migrate_eq_md.py — v0.4 equality `=` -> `==` inside Markdown ```toke blocks.

Doc code samples are embedded as fenced ```toke / ```tk blocks in .md files, and
many are FRAGMENTS (no `m=`/`f=main`), so neither the AST migrator nor a bare
`tkc --check` surfaces the equality error reliably. This tool:

  1. Extracts each ```toke|```tk block.
  2. Classifies it: full program (starts with `m=`), top-level defs (has `f=`/`t=`
     but no `m=`), or a statement fragment.
  3. Wraps fragments in the minimal harness needed to make the equality `=` reach
     the parser, drives the compiler, and collects E2002 "use `==` for equality"
     offsets (filtered by message AND verified the byte is `=` and not already
     `==`), mapping wrapped-space offsets back to block-space.
  4. Rewrites just those `=` -> `==` in the block, reinserts the block.

Byte-accurate throughout. Dry run by default; --write to apply. Blocks whose
equality errors can't be resolved after migration are reported for manual review.

Usage: python3 scripts/migrate_eq_md.py [--write] <file.md> [...]
"""
import sys, re, json, subprocess, os, tempfile

TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))
EQ = ord('=')
FENCE = re.compile(r"(^|\n)(```)(toke|tk)([ \t]*\n)(.*?)(\n```)", re.DOTALL)
MAX_PASSES = 40


def _pseudocomment_ranges(b):
    """Byte ranges covered by invalid `-- ...` line-comment annotations (toke only
    has `(* *)` block comments, so these are not real code — the `=` inside them is
    illustrative, not an operator, and must never be migrated). A range runs from a
    ` -- ` (or line-leading `--`) to end of line."""
    ranges = []
    i, n = 0, len(b)
    line_start = 0
    while i < n:
        if b[i] == ord('\n'):
            line_start = i + 1
            i += 1
            continue
        # ` -- ` or `--` at line start
        if b[i] == ord('-') and i + 1 < n and b[i + 1] == ord('-') and \
           (i == line_start or (i > 0 and b[i - 1] in b' \t')):
            eol = b.find(b'\n', i)
            eol = n if eol == -1 else eol
            ranges.append((i, eol))
            i = eol
            continue
        i += 1
    return ranges


def _in_ranges(o, ranges):
    return any(lo <= o < hi for lo, hi in ranges)


def _eq_offsets(text):
    """Byte offsets (in `text`) of `=` operators the compiler flags as needing `==`,
    excluding any inside invalid `-- ...` pseudo-comments."""
    with tempfile.NamedTemporaryFile("w", suffix=".tk", delete=False) as f:
        f.write(text); path = f.name
    try:
        r = subprocess.run([TKC, path, "--check"], capture_output=True, text=True)
    finally:
        os.unlink(path)
    b = text.encode("utf-8")
    skip = _pseudocomment_ranges(b)
    offs = []
    for line in (r.stdout + "\n" + r.stderr).splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("error_code") != "E2002":
            continue
        msg = d.get("message", "")
        if "equality" not in msg and "assignment" not in msg:
            continue          # E2002 is overloaded; only the `=`/`==` message counts
        o = d.get("span_start", d.get("pos", {}).get("offset"))
        if isinstance(o, int) and 0 <= o < len(b) and b[o] == EQ and (o + 1 >= len(b) or b[o + 1] != EQ) \
           and not _in_ranges(o, skip):
            offs.append(o)
    return sorted(set(offs), reverse=True)


def _wrap(block):
    """Return (wrapped_text, prefix_byte_len) that makes `block`'s equality `=` parse."""
    s = block.lstrip()
    if s.startswith("m="):
        return block, 0                                   # full program: no wrap
    if re.search(r"(^|\n)\s*[fт]=|\b[ft]=\w+\(", block) or re.search(r"(^|\n)\s*f=", block):
        prefix = "m=doc;\ni=io:std.io;\n"                  # top-level defs
        return prefix + block, len(prefix.encode("utf-8"))
    prefix = "m=doc;\ni=io:std.io;\nf=main():i64{\n"       # statement fragment
    suffix = "\nrt 0;\n}\n"
    return prefix + block + suffix, len(prefix.encode("utf-8"))


def migrate_block(block):
    """Return (new_block, n_changed, unresolved) for one code block."""
    b = bytearray(block.encode("utf-8"))
    changed = 0
    for _ in range(MAX_PASSES):
        wrapped, plen = _wrap(b.decode("utf-8"))
        offs = _eq_offsets(wrapped)
        # map wrapped offsets back into block space; drop any inside the wrapper
        blen = len(b)
        block_offs = sorted({o - plen for o in offs if plen <= o - plen < blen}, reverse=True)
        # additional guard: the mapped byte in the block must be '='
        block_offs = [o for o in block_offs if b[o] == EQ and (o + 1 >= blen or b[o + 1] != EQ)]
        if not block_offs:
            break
        for o in block_offs:
            b.insert(o + 1, EQ)
            changed += 1
    # re-check: did we leave any equality `=` unresolved (fragment we couldn't wrap)?
    wrapped, plen = _wrap(b.decode("utf-8"))
    unresolved = len(_eq_offsets(wrapped)) > 0
    return b.decode("utf-8"), changed, unresolved


def migrate_file(path, write):
    src = open(path, encoding="utf-8").read()
    total = 0; unresolved_blocks = 0
    def repl(m):
        nonlocal total, unresolved_blocks
        block = m.group(5)
        new_block, n, unresolved = migrate_block(block)
        total += n
        if unresolved:
            unresolved_blocks += 1
        return m.group(1) + m.group(2) + m.group(3) + m.group(4) + new_block + m.group(6)
    out = FENCE.sub(repl, src)
    if write and total:
        open(path, "w", encoding="utf-8").write(out)
    flag = " [UNRESOLVED blocks: %d]" % unresolved_blocks if unresolved_blocks else ""
    if total or unresolved_blocks:
        print(f"{path}: {total} equality '=' -> '=='{flag}")
    return total, unresolved_blocks


def main():
    write = "--write" in sys.argv
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    total = 0; unresolved = 0
    for p in files:
        t, u = migrate_file(p, write)
        total += t; unresolved += u
    print(f"total: {total} equality operators, {unresolved} unresolved blocks"
          f"{' (written)' if write else ' (dry run)'}")


if __name__ == "__main__":
    main()
