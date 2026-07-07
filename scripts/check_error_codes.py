#!/usr/bin/env python3
"""
check_error_codes.py — diagnostic-code documentation drift gate (Story 123.12).

Every diagnostic code the compiler can emit must be documented in
docs/reference/errors.md, and errors.md must not document codes that no longer
exist. This extends the 116.13/119 doc-gate discipline to error codes.

Sources of truth:
  * Defined codes  — `#define [LEX_]?[EW]NNNN NNNN /* description */` in src/*.h
                     and src/*.c.
  * Emitted codes  — any [EW]NNNN token referenced (not defined) in a src .c file
                     (i.e. actually passed to diag_emit / eerr).
  * Documented     — `### [EW]NNNN` section headers in docs/reference/errors.md.

A code is "live" if it is defined or emitted. The gate fails when:
  * a live code is undocumented, or
  * errors.md documents a code that is neither live nor on REMOVED_CODES.

Removed codes (feature deleted) are allow-listed so their historical doc entry —
if any — is flagged for deletion rather than silently accepted.

Usage: python3 scripts/check_error_codes.py [--list]
Exit: 0 if in sync, 1 on drift.
"""
import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ERRORS_MD = os.path.join(ROOT, "docs", "reference", "errors.md")

# Codes intentionally removed from the compiler (feature deleted). They must NOT
# appear in errors.md; listed here so the gate explains *why* rather than just
# flagging an "unknown documented code".
REMOVED_CODES = {
    "E4050": "spawn/await removed (D4=B)",
    "E4051": "spawn/await removed (D4=B)",
    "E4052": "spawn/await removed (D4=B)",
}

CODE_RE = re.compile(r"\b(?:LEX_)?([EW]\d{4})\b")
DEFINE_RE = re.compile(
    r"#define\s+(?:LEX_)?([EW]\d{4})\s+\d{4}\s*(?:/\*\s*(.*?)\s*\*/)?"
)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def collect_defined():
    """code -> description (may be '') from #define lines across src."""
    defined = {}
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.h"), recursive=True) + \
            glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True):
        for m in DEFINE_RE.finditer(read(path)):
            code, desc = m.group(1), (m.group(2) or "").strip()
            # Keep the first non-empty description we see.
            if code not in defined or (not defined[code] and desc):
                defined[code] = desc
    return defined


def collect_emitted(defined):
    """Codes referenced (not defined) in a .c file — i.e. actually emitted."""
    emitted = set()
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.c"), recursive=True):
        txt = read(path)
        # Strip #define lines so a definition doesn't count as an emission.
        txt = "\n".join(l for l in txt.splitlines() if not l.lstrip().startswith("#define"))
        for m in CODE_RE.finditer(txt):
            emitted.add(m.group(1))
    return emitted


def collect_documented():
    return set(re.findall(r"^###\s+([EW]\d{4})", read(ERRORS_MD), re.M))


def main():
    defined = collect_defined()
    emitted = collect_emitted(defined)
    documented = collect_documented()
    live = (set(defined) | emitted) - set(REMOVED_CODES)

    undocumented = sorted(live - documented)
    stale = sorted(documented - live)

    if "--list" in sys.argv:
        print(f"defined={len(defined)} emitted={len(emitted)} "
              f"documented={len(documented)} live={len(live)}")

    ok = True
    if undocumented:
        ok = False
        print("ERROR: diagnostic codes emitted/defined but NOT documented in "
              "docs/reference/errors.md:")
        for c in undocumented:
            print(f"  {c}  {defined.get(c, '(emitted)')}")
    if stale:
        ok = False
        print("ERROR: docs/reference/errors.md documents codes that are not live:")
        for c in stale:
            why = REMOVED_CODES.get(c, "no matching #define or emission — stale?")
            print(f"  {c}  ({why})")

    if ok:
        print(f"error-code docs in sync: {len(documented)} documented, "
              f"{len(live)} live.")
        return 0
    print("\nFix: document new codes in docs/reference/errors.md, or remove stale "
          "entries. See Story 123.12.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
