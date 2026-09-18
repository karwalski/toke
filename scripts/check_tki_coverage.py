#!/usr/bin/env python3
"""
check_tki_coverage.py — .tki <-> C glue consistency check.

Story 102.19 Phase 3 (created), reworked in 131.38.

For every {"kind": "func"} export in stdlib/*.tki this script computes the C
symbol the compiler will actually emit for a call to that function and
verifies the symbol is *defined* in src/stdlib/*.c.

The symbol is derived by mirroring src/llvm.c resolve_stdlib_call(), whose
tables are parsed from llvm.c at run time so the checker cannot drift from
the compiler:

  1. explicit per-module maps      if (!strcmp(mod,"str")) { if (!strcmp(method,"len")) return "tk_str_len_w"; ... }
  2. per-module snprintf patterns  mem -> tk_mem_%s, stack -> tk_stack_%s_w, ...
  3. sub-namespace prefixes        row.str -> tk_row_str_w   (sub_namespaces[] in llvm.c)
  4. generic fallback              tk_<module>_<method>_w

A resolver target that is not a tk_* symbol (e.g. math.sin -> "sin") is
accepted as a libc symbol.

Quarantine: scripts/check_tki_skiplist.txt lists exports that are known to be
unresolved and need a real implementation decision.  One entry per line:

    <file>.tki::<export name>   # <date> <reason> [<story>]

Quarantined exports are reported as "quarantined (N)" and do not fail the
check.  A skip-list entry whose export now resolves is *stale* and fails the
check (remove the line).

Exit code 0 iff no FAIL and no stale skip-list entries.

Usage:
    python3 scripts/check_tki_coverage.py        # summary + failures only
    python3 scripts/check_tki_coverage.py -v     # also list every PASS and
                                                 # every _w glue symbol no .tki declares
"""

import json
import re
import sys
from difflib import get_close_matches
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TKI_DIR = REPO_ROOT / "stdlib"
C_DIR = REPO_ROOT / "src" / "stdlib"
LLVM_C = REPO_ROOT / "src" / "llvm.c"
SKIPLIST = REPO_ROOT / "scripts" / "check_tki_skiplist.txt"

C_KEYWORDS = {"return", "else", "case", "goto", "sizeof", "if", "while",
              "for", "switch", "do", "typedef"}

# type-first function definition: `[static] [const] type [*] tk_name(`
DEF_RE = re.compile(
    r'^(?:(?:static|inline|extern)\s+)*(?:(?:const|unsigned|struct)\s+)*'
    r'([A-Za-z_]\w*)\s*\**\s*(tk_\w+)\s*\('
)
PROTO_END_RE = re.compile(r'\)\s*;\s*$')


# ── compiler tables (parsed from src/llvm.c) ────────────────────────────────

def load_resolver_tables(llvm_path: Path):
    """Return (explicit, patterns, sub_namespaces) mirroring resolve_stdlib_call().

    explicit:       {(mod, method): symbol}
    patterns:       {mod: "tk_mod_%s[_w]"}
    sub_namespaces: {"row", ...}
    """
    explicit, patterns, subns = {}, {}, set()
    if not llvm_path.exists():
        print(f"WARNING: {llvm_path} not found; using generic tk_<mod>_<method>_w rule only")
        return explicit, patterns, subns
    src = llvm_path.read_text(errors="replace")

    m = re.search(r'^static const char \*resolve_stdlib_call\(', src, re.M)
    if not m:
        print("WARNING: resolve_stdlib_call() not found in llvm.c; using generic rule only")
    else:
        end = src.find("\n}\n", m.start())
        body = src[m.start():end if end > 0 else len(src)]
        cur = None
        for line in body.splitlines():
            mm = re.search(r'!strcmp\(mod,\s*"([\w.]+)"\)', line)
            if mm:
                cur = mm.group(1)
            mm = re.search(r'!strcmp\(method,\s*"([\w.]+)"\)\)\s*return\s*"(\w+)"', line)
            if mm and cur:
                explicit[(cur, mm.group(1))] = mm.group(2)
            mm = re.search(r'snprintf\(\w+,\s*sizeof \w+,\s*"(tk_\w+%s(?:_w)?)",\s*method\)', line)
            if mm and cur:
                patterns[cur] = mm.group(1)

    mm = re.search(r'sub_namespaces\[\]\s*=\s*\{([^}]*)\}', src)
    if mm:
        subns = set(re.findall(r'"(\w+)"', mm.group(1)))
    return explicit, patterns, subns


def expected_symbol(mod: str, prefix: str, method: str, explicit, patterns, subns) -> str:
    """Symbol the compiler emits for `<prefix>.<method>` exported by module `mod`."""
    if prefix in subns and prefix != mod:
        return f"tk_{prefix}_{method}_w"
    if (mod, method) in explicit:
        return explicit[(mod, method)]
    if mod in patterns:
        return patterns[mod].replace("%s", method)
    return f"tk_{mod}_{method}_w"


# ── C definitions ───────────────────────────────────────────────────────────

def load_c_definitions(c_dir: Path) -> dict[str, str]:
    """Map every tk_* function *defined* in src/stdlib/*.c to its file."""
    defs: dict[str, str] = {}
    for c_file in sorted(c_dir.glob("*.c")):
        for line in c_file.read_text(errors="replace").splitlines():
            m = DEF_RE.match(line)
            if not m or m.group(1) in C_KEYWORDS:
                continue
            if PROTO_END_RE.search(line):
                continue  # prototype, not a definition
            defs.setdefault(m.group(2), c_file.name)
    return defs


# ── skip-list ───────────────────────────────────────────────────────────────

def load_skiplist(path: Path) -> dict[str, str]:
    """{ "file.tki::name": reason }"""
    skips: dict[str, str] = {}
    if not path.exists():
        return skips
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, reason = line.partition("#")
        key = key.strip()
        if "::" in key:
            skips[key] = reason.strip()
    return skips


# ── main ────────────────────────────────────────────────────────────────────

def main() -> int:
    verbose = "-v" in sys.argv or "--verbose" in sys.argv

    tki_files = sorted(TKI_DIR.glob("*.tki"))
    if not tki_files:
        print(f"ERROR: No .tki files found in {TKI_DIR}")
        return 1

    explicit, patterns, subns = load_resolver_tables(LLVM_C)
    defs = load_c_definitions(C_DIR)
    skips = load_skiplist(SKIPLIST)

    total = passed = 0
    failures: list[tuple[str, str, str, list[str]]] = []
    quarantined: list[tuple[str, str, str, str]] = []
    stale_skips: list[str] = []
    duplicates: list[str] = []
    resolved_symbols: set[str] = set()
    seen_keys: set[str] = set()

    for tki_file in tki_files:
        try:
            data = json.loads(tki_file.read_text())
        except (json.JSONDecodeError, OSError) as e:
            print(f"ERROR: Cannot parse {tki_file.name}: {e}")
            return 1

        module = data.get("module", "")
        mod = module[4:] if module.startswith("std.") else module  # mirrors llvm.c
        for export in data.get("exports", []):
            if export.get("kind") != "func":
                continue
            name = export.get("name", "")
            prefix, _, method = name.partition(".")
            if not method:
                prefix, method = mod, name
            key = f"{tki_file.name}::{name}"
            if key in seen_keys:
                duplicates.append(key)
            seen_keys.add(key)

            symbol = expected_symbol(mod, prefix, method, explicit, patterns, subns)
            resolved_symbols.add(symbol)
            total += 1

            ok = symbol in defs or not symbol.startswith("tk_")  # non-tk_ => libc target
            if ok:
                passed += 1
                if verbose:
                    where = defs.get(symbol, "libc")
                    print(f"  PASS: {key} -> {symbol} ({where})")
                if key in skips:
                    stale_skips.append(key)
                continue

            if key in skips:
                quarantined.append((tki_file.name, name, symbol, skips[key]))
                continue

            note = []
            if "." in symbol:
                note.append("module name contains '.', symbol is not a valid C identifier")
            similar = get_close_matches(symbol, list(defs), n=3, cutoff=0.6)
            failures.append((tki_file.name, name, symbol, similar + note))

    # (b) glue wrappers that no .tki export resolves to — informational
    undeclared = sorted(s for s in defs if s.endswith("_w") and s not in resolved_symbols)

    # ── report ──────────────────────────────────────────────────────────────
    if failures:
        print("FAIL (declared in .tki, symbol not defined in src/stdlib/*.c):")
        for tki_name, name, symbol, similar in failures:
            line = f"  {tki_name} :: {name} -> {symbol}"
            if similar:
                line += f"  (similar: {', '.join(similar)})"
            print(line)
        print()
    if stale_skips:
        print("STALE skip-list entries (now resolve; remove from scripts/check_tki_skiplist.txt):")
        for k in stale_skips:
            print(f"  {k}")
        print()
    if duplicates:
        print("WARNING duplicate exports in .tki:")
        for k in duplicates:
            print(f"  {k}")
        print()
    if verbose and quarantined:
        print("Quarantined:")
        for tki_name, name, symbol, reason in quarantined:
            print(f"  {tki_name} :: {name} -> {symbol}  # {reason}")
        print()
    if verbose and undeclared:
        print("_w glue symbols no .tki export resolves to (informational):")
        for s in undeclared:
            print(f"  {s} ({defs[s]})")
        print()

    print("=" * 60)
    print(f"check-tki: {total} .tki func exports, {len(defs)} tk_* definitions in src/stdlib/*.c")
    print(f"  PASS:        {passed}")
    print(f"  FAIL:        {len(failures)}")
    print(f"  quarantined: {len(quarantined)}  ({SKIPLIST.relative_to(REPO_ROOT)})")
    print(f"  stale skips: {len(stale_skips)}")
    print(f"  undeclared _w glue (no .tki export resolves to it): {len(undeclared)}  [informational, -v to list]")
    print("=" * 60)

    if failures or stale_skips:
        return 1
    print(f"All non-quarantined .tki declarations resolve to defined C symbols "
          f"({passed} pass, {len(quarantined)} quarantined).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
