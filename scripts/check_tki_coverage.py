#!/usr/bin/env python3
"""
Story 102.19 Phase 3: Automated .tki/C consistency check.

Verifies every function declared in a .tki file has a corresponding
C implementation (tk_{module}_{function}_w) in src/stdlib/*.c files.

Exit code 0 if all pass, 1 if any fail.
"""

import json
import os
import re
import sys
from difflib import get_close_matches
from pathlib import Path

# Resolve paths relative to the repo root (parent of scripts/)
REPO_ROOT = Path(__file__).resolve().parent.parent
TKI_DIR = REPO_ROOT / "stdlib"
C_DIR = REPO_ROOT / "src" / "stdlib"


def extract_module_short(module_name: str) -> str:
    """Extract short module name from qualified name.

    std.str -> str, std.io -> io, std.llm_tool -> llm_tool
    """
    parts = module_name.split(".")
    if parts[0] == "std" and len(parts) >= 2:
        return "_".join(parts[1:])
    return "_".join(parts)


def extract_func_short(func_name: str) -> str:
    """Extract the function part from a qualified function name.

    str.len -> len, str.from_int -> from_int, time.to_parts -> to_parts
    """
    parts = func_name.split(".", 1)
    if len(parts) == 2:
        return parts[1]
    return func_name


def expected_symbol(module_short: str, func_short: str) -> str:
    """Compute the expected C symbol: tk_{module}_{function}_w"""
    return f"tk_{module_short}_{func_short}_w"


def load_c_symbols(c_dir: Path) -> tuple[set[str], list[str]]:
    """Load all C files content and extract defined symbols.

    Returns:
        - set of all symbol-like identifiers found as function definitions
        - list of all raw C file contents concatenated (for fuzzy search)
    """
    all_symbols = set()
    all_content_lines = []

    if not c_dir.exists():
        return all_symbols, all_content_lines

    for c_file in sorted(c_dir.glob("*.c")):
        content = c_file.read_text(errors="replace")
        all_content_lines.append(content)
        # Match function definitions: return_type tk_module_func_w(...)
        # Pattern: word characters followed by _w and opening paren at start-ish of line
        for match in re.finditer(r'\b(tk_\w+_w)\s*\(', content):
            all_symbols.add(match.group(1))

    return all_symbols, all_content_lines


def find_similar(symbol: str, all_symbols: set[str]) -> list[str]:
    """Find similar symbols for MISMATCH reporting."""
    matches = get_close_matches(symbol, list(all_symbols), n=3, cutoff=0.6)
    return matches


def main():
    tki_files = sorted(TKI_DIR.glob("*.tki"))
    if not tki_files:
        print(f"ERROR: No .tki files found in {TKI_DIR}")
        sys.exit(1)

    # Pre-load all C symbols
    c_symbols, _ = load_c_symbols(C_DIR)

    total = 0
    passed = 0
    failed = 0
    mismatches = 0
    failures = []

    for tki_file in tki_files:
        try:
            data = json.loads(tki_file.read_text())
        except (json.JSONDecodeError, OSError) as e:
            print(f"ERROR: Cannot parse {tki_file.name}: {e}")
            continue

        module_name = data.get("module", "")
        module_short = extract_module_short(module_name)
        exports = data.get("exports", [])

        for export in exports:
            if export.get("kind") != "func":
                continue

            func_name = export.get("name", "")
            func_short = extract_func_short(func_name)
            symbol = expected_symbol(module_short, func_short)
            total += 1

            if symbol in c_symbols:
                print(f"  PASS: {tki_file.name} :: {func_name} -> {symbol}")
                passed += 1
            else:
                similar = find_similar(symbol, c_symbols)
                if similar:
                    print(f"  MISMATCH: {tki_file.name} :: {func_name} -> {symbol}")
                    print(f"            similar: {', '.join(similar)}")
                    mismatches += 1
                    failures.append((tki_file.name, func_name, symbol, similar))
                else:
                    print(f"  FAIL: {tki_file.name} :: {func_name} -> {symbol} (not found)")
                    failed += 1
                    failures.append((tki_file.name, func_name, symbol, []))

    # Summary
    print()
    print("=" * 60)
    print(f"Summary: {total} functions checked")
    print(f"  PASS:     {passed}")
    print(f"  FAIL:     {failed}")
    print(f"  MISMATCH: {mismatches}")
    print("=" * 60)

    if failures:
        print()
        print("Failures:")
        for tki_name, func_name, symbol, similar in failures:
            line = f"  {tki_name} :: {func_name} -> {symbol}"
            if similar:
                line += f" (similar: {', '.join(similar)})"
            print(line)
        print()
        sys.exit(1)
    else:
        print("\nAll .tki declarations have matching C implementations.")
        sys.exit(0)


if __name__ == "__main__":
    main()
