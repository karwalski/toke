#!/usr/bin/env python3
"""
verify_project_facts.py — derive every countable project-scale fact from the
tree (Story 132.14).

Why this exists
---------------
Story 132.8 tried to source the whitepaper's project-scale numbers and could
verify none of them.  Every one was checkable in seconds and several were
wrong: "65 EBNF productions" (53), "38 standard library modules" (57),
"62+ conformance tests" / "90/90" (222 + 6), "99 epics, 500+ stories" (132 and
2,373).  Worse, two normative documents disagreed on the size of the character
set — the RFC said 56, the spec and the positioning brief said 55, and the
compiler said neither.

A number that nobody can reproduce is worse than no number.  This script is the
reproduction: it reads the tree, derives each fact, and prints the command that
derives it.  `docs/metrics-baseline.md` carries the same numbers in a "Project
facts" table, and `--check` fails when the two drift apart.

Usage:
    python3 scripts/verify_project_facts.py              # human-readable table
    python3 scripts/verify_project_facts.py --json       # machine-readable sheet
    python3 scripts/verify_project_facts.py --check      # diff against metrics-baseline.md
    python3 scripts/verify_project_facts.py --probe      # cross-check the character
                                                         # set against the built tkc
Exit: 0 clean, 1 on drift (--check) or on an internal inconsistency (--probe).
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Sibling repo that holds the corpus freeze manifests.
CORPUS = os.path.normpath(os.path.join(ROOT, "..", "toke-corpus"))

# Printable ASCII that the default profile rejects in structural position.
# Derived below from src/lexer.c; asserted against the binary under --probe.
LOWERCASE = 26
DIGITS = 10


def _read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# Character set — src/lexer.c is the ground truth
# ---------------------------------------------------------------------------

def charset_facts():
    """Derive the default-profile alphabet from the lexer.

    The lexer decides the character set: every character it will not accept in
    a structural position gets E1003.  Three places in lex() matter:

      * `"` opens a string literal (handled before the symbol switch),
      * `#` is accepted with W1020 only as Python-comment *recovery* — it is a
        diagnostic affordance, not a member of the language alphabet,
      * the symbol switch: each `case 'c':` is a member unless that arm emits
        E1003 for PROFILE_DEFAULT (which is what `[` and `]` do).

    Letters are a-z; the lexer tolerates A-Z but the parser rejects an
    uppercase identifier (E2002 "expected identifier"), so uppercase is not in
    the alphabet.  `_` is rejected outright in PROFILE_DEFAULT (no-underscore,
    story 113.2a).
    """
    src = _read(os.path.join(ROOT, "src", "lexer.c"))
    # Isolate the single-character symbol switch inside lex().
    start = src.index("/* 5. Single-character symbols. */")
    end = src.index("default: {", start)
    body = src[start:end]

    arms = [m for m in re.finditer(r"case '(\\?.)':", body)]
    members, rejected = [], []
    for i, m in enumerate(arms):
        ch = m.group(1).replace("\\", "")
        arm = body[m.end():arms[i + 1].start() if i + 1 < len(arms) else len(body)]
        # An arm that emits E1003 with no profile guard other than
        # PROFILE_DEFAULT rejects the character in the default profile.
        if "LEX_E1003" in arm and "PROFILE_DEFAULT" in arm:
            rejected.append(ch)
        else:
            members.append(ch)

    symbols = sorted(set(members) | {'"'})
    total = LOWERCASE + DIGITS + len(symbols)
    return {
        "charset_symbols": len(symbols),
        "charset_symbol_list": "".join(symbols),
        "charset_total": total,
        "charset_lowercase": LOWERCASE,
        "charset_digits": DIGITS,
        "charset_rejected_in_default": "".join(sorted(set(rejected) | set("_\\'`,?"))),
        # `^` and `~` were reserved-and-unassigned in the March 2026 RFC draft;
        # story 114.8 assigned them.  Nothing in the set is reserved today.
        "charset_reserved_unassigned": 0,
    }


def probe_charset(facts):
    """Cross-check the derived alphabet against the built compiler."""
    tkc = os.path.join(ROOT, "tkc")
    if not os.path.exists(tkc):
        print("probe: tkc not built — skipping (run `make` first)")
        return 0
    accepted = set()
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "probe.tk")
        for o in range(33, 127):
            c = chr(o)
            with open(p, "w", encoding="utf-8") as fh:
                fh.write("m=probe;f=main():$i64{let x=1;%s}" % c)
            r = subprocess.run([tkc, "--check", p], capture_output=True, text=True)
            if "E1003" not in (r.stdout + r.stderr):
                accepted.add(c)
    # The alphabet is lowercase + digits + symbols; the probe also accepts A-Z
    # (parser-rejected) and '#' (warning-only recovery), so exclude those.
    probed = {c for c in accepted if not c.isupper() and not c.isdigit()
              and not c.islower() and c != "#"}
    derived = set(facts["charset_symbol_list"])
    if probed != derived:
        print("probe MISMATCH: lexer.c derives %r, the binary accepts %r"
              % ("".join(sorted(derived)), "".join(sorted(probed))))
        return 1
    print("probe OK: src/lexer.c and the built tkc agree on %d symbols (%s)"
          % (len(derived), "".join(sorted(derived))))
    return 0


# ---------------------------------------------------------------------------
# The rest of the fact sheet
# ---------------------------------------------------------------------------

def keyword_facts():
    src = _read(os.path.join(ROOT, "src", "lexer.c"))
    block = re.search(r"KEYWORDS_DEFAULT\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    words = re.findall(r'\{\s*"([^"]+)"\s*,\s*(\w+)', block)
    reserved = [w for w, kind in words if kind != "TK_BOOL_LIT"]
    literals = [w for w, kind in words if kind == "TK_BOOL_LIT"]
    # m= i= t= f= are declaration heads: plain identifiers to the lexer,
    # disambiguated by the parser.  The published "14 keywords" counts them.
    heads = ["m", "i", "t", "f"]
    return {
        "keywords": len(reserved) + len(heads),
        "keywords_lexer_reserved": len(reserved),
        "keywords_declaration_heads": len(heads),
        "keyword_list": " ".join(heads + reserved),
        "boolean_literals": len(literals),
    }


def grammar_facts():
    path = os.path.join(ROOT, "docs", "spec", "grammar.ebnf")
    heads = re.findall(r"^([A-Za-z][A-Za-z0-9_]*)\s*=", _read(path), re.M)
    return {"grammar_productions": len(set(heads))}


def stdlib_facts():
    mods = glob.glob(os.path.join(ROOT, "stdlib", "*.tki"))
    return {"stdlib_modules": len(mods)}


def conformance_facts():
    yamls = glob.glob(os.path.join(ROOT, "test", "**", "*.yaml"), recursive=True)
    shells = glob.glob(os.path.join(ROOT, "test", "conform", "*.sh"))
    by_series = {}
    for y in yamls:
        by_series[os.path.basename(os.path.dirname(y))] = \
            by_series.get(os.path.basename(os.path.dirname(y)), 0) + 1
    return {
        "conformance_cases_yaml": len(yamls),
        "conformance_cases_shell": len(shells),
        "conformance_cases_total": len(yamls) + len(shells),
        "conformance_by_series": by_series,
    }


def diagnostic_facts():
    """Documented codes, cross-checked against the codes src/ can actually emit.

    `scripts/check_error_codes.py` already owns the definition of "live" (a
    code defined by a `#define` or passed to diag_emit) and is the gate in
    `make ci`; reuse it rather than growing a second, subtly different one.
    """
    documented = set(re.findall(
        r"^###\s+([EW]\d{4})",
        _read(os.path.join(ROOT, "docs", "reference", "errors.md")), re.M))
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import check_error_codes as cec
    defined = cec.collect_defined()
    live = set(defined) | set(cec.collect_emitted(defined))
    return {
        "diagnostic_codes_documented": len(documented),
        "diagnostic_codes_in_src": len(live),
        "diagnostic_codes_undocumented": sorted(live - documented),
    }


def corpus_facts():
    out = {}
    freeze = os.path.join(CORPUS, "regen", "freeze", "freeze_129_summary.json")
    if os.path.exists(freeze):
        d = json.loads(_read(freeze))
        out["corpus_records_v04_frozen"] = d["total"]
        out["corpus_freeze_id"] = d["freeze"]
    v02 = os.path.join(CORPUS, "corpus", "manifest.json")
    if os.path.exists(v02):
        out["corpus_records_v02_2026_04"] = json.loads(_read(v02))["total_entries"]
    return out


def tracker_facts():
    t = _read(os.path.join(ROOT, "docs", "progress.md"))
    epics = {h.split(".")[0] for h in
             re.findall(r"^#{2,3}\s+Epic\s+(\d+(?:\.\d+)?)", t, re.M)}
    stories = set(re.findall(r"^\|\s*(\d+\.\d+[a-z0-9.]*)\s*\|", t, re.M))
    return {"epics": len(epics), "stories": len(stories)}


# Each fact carries the command that derives it, so the metrics file can print
# it and any reader can re-run it.
COMMANDS = {
    "charset_total":
        "python3 scripts/verify_project_facts.py --probe  # 26 lowercase + 10 digits + symbols, from src/lexer.c",
    "charset_symbols":
        "sed -n '/5. Single-character symbols/,/default: {/p' src/lexer.c | grep -c \"case '\"  # minus [ ] which E1003 in default, plus \"",
    "keywords":
        "sed -n '/KEYWORDS_DEFAULT/,/};/p' src/lexer.c  # 10 reserved words + m= i= t= f= declaration heads",
    "grammar_productions":
        "grep -cE '^[A-Za-z][A-Za-z0-9_]*[[:space:]]*=' docs/spec/grammar.ebnf",
    "stdlib_modules":
        "ls stdlib/*.tki | wc -l",
    "conformance_cases_yaml":
        "find test -name '*.yaml' | wc -l",
    "conformance_cases_shell":
        "ls test/conform/*.sh | wc -l",
    "conformance_cases_total":
        "find test -name '*.yaml' | wc -l; ls test/conform/*.sh | wc -l",
    "diagnostic_codes_documented":
        "grep -cE '^### [EW][0-9]{4}' docs/reference/errors.md",
    "diagnostic_codes_in_src":
        "python3 scripts/check_error_codes.py --list  # codes src/ defines or emits",
    "corpus_records_v04_frozen":
        "jq .total ../toke-corpus/regen/freeze/freeze_129_summary.json",
    "corpus_records_v02_2026_04":
        "jq .total_entries ../toke-corpus/corpus/manifest.json",
    "epics":
        "grep -oE '^#{2,3} Epic [0-9]+' docs/progress.md | awk '{print $3}' | sort -u | wc -l",
    "stories":
        "grep -oE '^\\| [0-9]+\\.[0-9]+[a-z0-9.]* \\|' docs/progress.md | sort -u | wc -l",
}

# Facts that the "Project facts" table in docs/metrics-baseline.md must carry
# verbatim.  --check fails when the file and the tree disagree.
CHECKED = [
    "charset_total", "charset_symbols", "keywords", "grammar_productions",
    "stdlib_modules", "conformance_cases_yaml", "conformance_cases_shell",
    "conformance_cases_total", "diagnostic_codes_documented",
    "diagnostic_codes_in_src", "corpus_records_v04_frozen",
    "corpus_records_v02_2026_04",
    # `epics` and `stories` are deliberately NOT gated: they move with every
    # commit to docs/progress.md, so pinning them in the table would fail CI on
    # unrelated work. The table prints them with a date and the command; rule 4
    # of check_metrics_claims.py still reads the *live* value, so a published
    # "99 epics, 500+ stories" is caught exactly.
]


def collect():
    facts = {}
    for fn in (charset_facts, keyword_facts, grammar_facts, stdlib_facts,
               conformance_facts, diagnostic_facts, corpus_facts, tracker_facts):
        facts.update(fn())
    return facts


def check_against_baseline(facts):
    path = os.path.join(ROOT, "docs", "metrics-baseline.md")
    text = _read(path)
    m = re.search(r"<!-- PROJECT-FACTS BEGIN -->(.*?)<!-- PROJECT-FACTS END -->",
                  text, re.S)
    if not m:
        print("ERROR: docs/metrics-baseline.md has no PROJECT-FACTS block.\n"
              "Regenerate it: python3 scripts/verify_project_facts.py --json")
        return 1
    block = m.group(1)
    stated = dict(re.findall(r"`([a-z0-9_]+)`\s*\|\s*\*\*([\d,]+)\*\*", block))
    bad = []
    for key in CHECKED:
        if key not in facts:
            continue
        if key not in stated:
            bad.append((key, facts[key], "absent from the table"))
        elif int(stated[key].replace(",", "")) != facts[key]:
            bad.append((key, facts[key], stated[key]))
    if bad:
        print("ERROR: docs/metrics-baseline.md 'Project facts' has drifted from "
              "the tree:\n")
        for key, actual, claimed in bad:
            print("  %-30s tree says %-8s file says %s" % (key, actual, claimed))
            print("      derive with: %s" % COMMANDS.get(key, "(see script)"))
        print("\nFix: re-run `python3 scripts/verify_project_facts.py` and update "
              "the table in docs/metrics-baseline.md. Story 132.14.")
        return 1
    print("project facts OK: %d counts in docs/metrics-baseline.md match the tree."
          % len(CHECKED))
    return 0


def main():
    facts = collect()
    if "--json" in sys.argv:
        print(json.dumps({"facts": facts, "commands": COMMANDS}, indent=2,
                         sort_keys=True))
        return 0
    if "--check" in sys.argv:
        return check_against_baseline(facts)
    if "--probe" in sys.argv:
        return probe_charset(facts)
    width = max(len(k) for k in facts)
    for k in sorted(facts):
        v = facts[k]
        print("%-*s  %s" % (width, k, v if not isinstance(v, list) else v or "-"))
        if k in COMMANDS:
            print("%-*s    $ %s" % (width, "", COMMANDS[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
