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
  * docs/spec/errors.md's generated table, section titles or never-emitted list
    disagree with the above (127.154; see check_spec).

Removed codes (feature deleted) are allow-listed so their historical doc entry —
if any — is flagged for deletion rather than silently accepted.

Usage: python3 scripts/check_error_codes.py [--list] [--write-spec]
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
    # The trailing \b matters: without it `### E4071x` or
    # `### E9005_TEMPORARILY_BROKEN` would satisfy the check for E4071/E9005,
    # because a prefix match is not a documented section (Story 127.124).
    return set(re.findall(r"^###\s+([EW]\d{4})\b", read(ERRORS_MD), re.M))


# ── Second registry: docs/spec/errors.md (story 127.154) ──────────────────
#
# docs/spec/errors.md is published as "canonical error codes and their
# meanings" (docs/spec/index.md), and until 127.154 nothing read it. Measured
# 2026-09-24 it disagreed with the emitters on the MEANING of five codes —
# E1005, E2005, E2015, E4026, E5002 — listed five codes nothing emits, and
# omitted twenty-one live ones. Every one of the five emitter meanings is
# unchanged since the repository's first commit, so the spec copy drifted;
# no code was reused.
#
# A second hand-kept copy that is merely checked is still a second copy, so
# the spec's summary table is GENERATED from the two things that are
# authoritative — the live code set (src) and each code's title in
# docs/reference/errors.md (itself gated above) — between two marker lines.
# `--write-spec` regenerates it; the check regenerates it in memory and fails
# on any difference. The Stage column is derived from src/diag.c's
# phase_for_code (123.12a); the Conformance column is carried over from the
# file (it is not derivable from src) and a new code gets `n/s`.
#
# Outside the generated block the gate also asserts:
#   * every `### CODE -- Title` section in the spec names a code in the table
#     or in the "never emitted" table, under the same title (a section that
#     says one thing while the table says another is the E4026 defect again);
#   * no never-emitted code is live. Those five numbers (E2006, E2011, E4001,
#     E4020, E4021) were specified with a meaning and never implemented; a new
#     diagnostic that picked one up would silently inherit a meaning the spec
#     already publishes for it (AGENTS.md §6: never change a code's meaning).

SPEC_MD = os.path.join(ROOT, "docs", "spec", "errors.md")
SPEC_BEGIN = ("<!-- BEGIN error-code table: generated by "
              "scripts/check_error_codes.py --write-spec from src and "
              "docs/reference/errors.md; edit Conformance only -->")
SPEC_END = "<!-- END error-code table -->"
NEVER_BEGIN = "<!-- BEGIN never-emitted codes -->"
NEVER_END = "<!-- END never-emitted codes -->"
ROW_RE = re.compile(r"^\|\s*([EW]\d{4})\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|"
                    r"\s*(.*?)\s*\|\s*(.*?)\s*\|\s*$", re.M)
SECTION_RE = re.compile(r"^###\s+([EW]\d{4})\s+--\s+(.*?)\s*$", re.M)
STAGE_NORMAL = {"typecheck": "type_check", "arena": "arena_check"}


def reference_facts():
    """code -> (title, stage) from docs/reference/errors.md sections."""
    ref = read(ERRORS_MD)
    parts = re.split(r"^###\s+([EW]\d{4})\b.*$", ref, flags=re.M)
    facts = {}
    for i in range(1, len(parts), 2):
        code, body = parts[i], parts[i + 1]
        t = re.match(r"\s*\*\*(.*?)\*\*", body)
        st = re.search(r"^\|\s*Stage\s*\|\s*([^|]*?)\s*\|", body, re.M)
        stage = (st.group(1).split()[0] if st and st.group(1).split() else "")
        facts[code] = (t.group(1).strip() if t else "",
                       STAGE_NORMAL.get(stage, stage))
    return facts


def _code_key(code):
    return (int(code[1:]), code[0])


def render_spec_table(existing_rows, live, defined, emitted, facts):
    stage_of = emitted_stage_fn()
    lines = [SPEC_BEGIN, "",
             "| Code | Severity | Stage | Short description | Conformance test(s) |",
             "|------|----------|-------|-------------------|---------------------|"]
    for code in sorted(live, key=_code_key):
        title, _ref_stage = facts.get(code, ("", ""))
        old = existing_rows.get(code)
        # 123.12a: Stage is the `stage` every JSON diagnostic for this code
        # carries (src/diag.c phase_for_code), not a hand-kept column.
        stage = stage_of(int(code[1:]), code[0])
        conf = old[3] if old and old[3] else "n/s"
        sev = "error" if code.startswith("E") else "warning"
        lines.append(f"| {code} | {sev} | {stage} | {title} | {conf} |")
    reserved = sorted((set(defined) - emitted) & live, key=_code_key)
    lines += ["",
              "**Legend:** `--` = no dedicated conformance test yet (code is "
              "exercised by compiler but not by a D/G-series test); `n/s` = "
              "not surveyed for this table — see `docs/reference/errors.md`.",
              "",
              "Codes " + ", ".join(f"**{c}**" for c in reserved) +
              " are defined in headers but not yet emitted by any `diag_emit` "
              "call -- they are reserved for planned functionality.",
              "", SPEC_END]
    return "\n".join(lines)


def spec_block(text):
    b, e = text.find(SPEC_BEGIN), text.find(SPEC_END)
    if b < 0 or e < b:
        return None
    return b, e + len(SPEC_END)


def check_spec(live, defined, emitted, write=False):
    """Return a list of error strings (empty when the spec agrees)."""
    errs = []
    text = read(SPEC_MD)
    span = spec_block(text)
    if span is None:
        return [f"{os.path.relpath(SPEC_MD, ROOT)}: generated table markers "
                "not found"]
    block = text[span[0]:span[1]]
    existing = {m.group(1): m.groups()[1:] for m in ROW_RE.finditer(block)}
    facts = reference_facts()
    want = render_spec_table(existing, live, defined, emitted, facts)
    if block != want:
        if write:
            text = text[:span[0]] + want + text[span[1]:]
            with open(SPEC_MD, "w", encoding="utf-8") as fh:
                fh.write(text)
            print(f"rewrote the generated table in {os.path.relpath(SPEC_MD, ROOT)}")
        else:
            have = {c: r[2] for c, r in existing.items()}
            # 127.267: the stage is per (number, severity) -- W1001 is
            # type_check, E1001 lex.  render_spec_table() passes the severity;
            # this listing used to drop it and so reported W1001 as drift
            # ("the compiler emits 'lex'") whenever any other drift was present.
            stage_of = emitted_stage_fn()
            for c in sorted(set(have) | live, key=_code_key):
                if c not in live:
                    errs.append(f"{c}: in the spec table but not live")
                elif c not in have:
                    errs.append(f"{c}: live but missing from the spec table")
                elif have[c] != facts.get(c, ("",))[0]:
                    errs.append(f"{c}: spec says {have[c]!r}, "
                                "docs/reference/errors.md says "
                                f"{facts.get(c, ('',))[0]!r}")
                elif existing[c][1] != stage_of(int(c[1:]), c[0]):
                    errs.append(f"{c}: spec Stage {existing[c][1]!r}, the "
                                "compiler emits "
                                f"{stage_of(int(c[1:]), c[0])!r}")
            if not errs:
                errs.append("generated table is out of date (legend or "
                            "reserved list)")
            errs.append("run: python3 scripts/check_error_codes.py --write-spec")

    table_titles = {c: facts.get(c, ("",))[0] for c in live}

    # Never-emitted codes: the hand-kept table between its markers. These
    # numbers were specified and never implemented; they stay reserved.
    rb, re_ = text.find(NEVER_BEGIN), text.find(NEVER_END)
    if rb < 0 or re_ < rb:
        errs.append("never-emitted-codes markers not found")
        never = {}
    else:
        never = {m.group(1): m.group(4)
                   for m in ROW_RE.finditer(text[rb:re_])}
    for c in sorted(never, key=_code_key):
        if c in live:
            errs.append(f"{c}: listed as NEVER EMITTED in the spec, with the "
                        "meaning above, but the compiler now emits it — give "
                        "the new diagnostic a new number, or move the row "
                        "into the table if this IS that meaning")

    for m in SECTION_RE.finditer(text):
        code = m.group(1)
        title = re.sub(r"\s*\((?:reserved|never emitted)\)\s*$", "",
                       m.group(2))
        want_t = table_titles.get(code, never.get(code))
        if want_t is None:
            errs.append(f"{code}: has a section '### {code} -- {m.group(2)}' "
                        "but is neither live nor listed as never emitted")
        elif title != want_t:
            errs.append(f"{code}: section title {title!r} disagrees with the "
                        f"registry {want_t!r}")
    return errs


# ── Per-section fields vs what the compiler emits (123.12a) ───────────────
#
# Each `### CODE` section in docs/reference/errors.md carries a field table
# with Severity and Stage.  Both are facts the compiler decides mechanically
# and puts in every JSON diagnostic: the letter is chosen by severity
# (diag.c: `(sev == DIAG_ERROR) ? 'E' : 'W'`) and `stage` by the code's range
# (diag.c: phase_for_code).  So those two fields are GENERATED facts, and the
# gate derives them from src/diag.c itself rather than from a copy here.
# Measured 2026-09-26: 21 of 64 sections gave a Stage the JSON never carries
# ("typecheck", "arena", or the pass that happens to emit the code, e.g. E2030
# documented as name_resolution while every E2030 record says "parse"), so a
# consumer filtering diagnostics by `stage` from these docs got it wrong.
# The first word of the Stage field must be the emitted `stage`; anything
# after it (e.g. "(emitted by name resolution)") is free prose.

DIAG_C = os.path.join(ROOT, "src", "diag.c")
PHASE_RE = re.compile(r"if\s*\(code\s*>=\s*(\d+)\s*&&\s*code\s*<=\s*(\d+)\)"
                      r"\s*return\s*\"(\w+)\";")


def emitted_stage_fn():
    """Build code-number -> stage from phase_for_code() in src/diag.c."""
    txt = read(DIAG_C)
    # The signature grew a severity parameter (127.223: W1001 shares its
    # number with E1001 but is raised by the type checker), so match any
    # parameter list and read per-severity overrides as well as ranges.
    body = re.search(r"phase_for_code\([^)]*\)\s*\{(.*?)\n\}", txt, re.S)
    if not body:
        raise SystemExit("check_error_codes: phase_for_code() not found in "
                         "src/diag.c -- the gate cannot derive `stage`")
    ranges = [(int(a), int(b), st) for a, b, st in PHASE_RE.findall(body.group(1))]
    overrides = {(int(n), 'W' if sv == 'WARNING' else 'E'): st
                 for n, sv, st in re.findall(
                     r"if\s*\(code\s*==\s*(\d+)\s*&&\s*sev\s*==\s*DIAG_(WARNING|ERROR)\)\s*return\s*\"(\w+)\"",
                     body.group(1))}
    tail = re.findall(r"return\s*\"(\w+)\";\s*$", body.group(1).rstrip())
    fallback = tail[-1] if tail else ""
    if not ranges:
        raise SystemExit("check_error_codes: no ranges parsed from phase_for_code()")

    def stage(n, kind='E'):
        if (n, kind) in overrides:
            return overrides[(n, kind)]
        for a, b, st in ranges:
            if a <= n <= b:
                return st
        return fallback
    return stage


def check_fields(live):
    """Severity/Stage field rows in both registries vs the emitter."""
    errs = []
    stage_of = emitted_stage_fn()
    for path in (ERRORS_MD, SPEC_MD):
        rel = os.path.relpath(path, ROOT)
        parts = re.split(r"^###\s+([EW]\d{4})\b.*$", read(path), flags=re.M)
        for i in range(1, len(parts), 2):
            code, body = parts[i], parts[i + 1]
            if code not in live:
                continue   # never-emitted spec sections carry no JSON facts
            sv = re.search(r"^\|\s*(?:\*\*)?Severity(?:\*\*)?\s*\|\s*([^|]*?)\s*\|",
                           body, re.M)
            st = re.search(r"^\|\s*(?:\*\*)?Stage(?:\*\*)?\s*\|\s*([^|]*?)\s*\|",
                           body, re.M)
            want_sev = "error" if code[0] == "E" else "warning"
            want_st = stage_of(int(code[1:]), code[0])
            if not sv or sv.group(1) != want_sev:
                errs.append(f"{rel} {code}: Severity is "
                            f"{sv.group(1) if sv else 'missing'!r}, the "
                            f"compiler emits {want_sev!r}")
            got = st.group(1).split()[0] if st and st.group(1).split() else None
            if got != want_st:
                errs.append(f"{rel} {code}: Stage is "
                            f"{st.group(1) if st else 'missing'!r}, every JSON "
                            f"diagnostic says \"stage\":\"{want_st}\" "
                            "(src/diag.c phase_for_code)")
    return errs


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

    field_errs = check_fields(live)
    if field_errs:
        ok = False
        print("ERROR: error-code field tables disagree with what the "
              "compiler emits (123.12a):")
        for e in field_errs:
            print(f"  {e}")

    spec_errs = check_spec(live, defined, emitted,
                           write="--write-spec" in sys.argv)
    if spec_errs:
        ok = False
        print("ERROR: docs/spec/errors.md disagrees with the live registry "
              "(127.154):")
        for e in spec_errs:
            print(f"  {e}")

    if ok:
        print(f"error-code docs in sync: {len(documented)} documented, "
              f"{len(live)} live; Severity/Stage fields match the emitter; "
              "docs/spec/errors.md table agrees.")
        return 0
    print("\nFix: document new codes in docs/reference/errors.md, or remove stale "
          "entries. See Story 123.12.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
