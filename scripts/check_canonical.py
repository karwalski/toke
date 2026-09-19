#!/usr/bin/env python3
"""
check_canonical.py — canonical facts block drift gate (Story 132.1 / 132.12).

Why this exists
---------------
Epic 132 criterion 2: **one** canonical facts block lives in a single source file
(`docs/about/canonical.md` + `docs/about/canonical.json`) and is reproduced *word
for word* on the home page, `/llms.txt`, the README of every toke repo, the
Hugging Face model card and the PyPI/npm/VS Code/Ollama descriptions. A copy that
has been re-worded is how "LL(1)", "13 keywords" and "52% fewer tokens" survived
for months on surfaces nobody was re-reading.

This gate stops a copy from drifting, and stops the two facts the spec has
already retired from coming back.

Rule 1 — BLOCK DRIFT. Every declared surface must reproduce the canonical blocks
it carries, verbatim modulo line wrapping (and, for HTML/template surfaces, tags
and entities). A near-miss is reported as drift with the first differing
fragment; a total absence is reported as missing.

Rule 2 — STALE FACTS (story 132.12). "LL(1)" and "13 keywords" are contradicted
by our own normative spec: `docs/spec/toke-spec-v0.4.md` §E (the v0.3 strict-LL(1)
claim "was not accurate for the real grammar"; the verified property is
backtrack-free with bounded lookahead of up to 3 tokens) and §A (the keyword set
is 14: m i t f let if el lp br rt as mt sc mut). Either string fails unless the
sentence — or a line within CORRECTION_LINES of it — marks it as retired,
historical or quoted. The mechanical argument is unaffected and is what should be
written instead: a small backtrack-free grammar with bounded lookahead is still
cheap to constrain during decoding and cheap to parse.

Usage:  python3 scripts/check_canonical.py [paths...] [--strict] [--list]
Exit:   0 clean (pending-story surfaces and files warn only), 1 on a violation.
        --strict also fails on the pending-story entries.
"""
import difflib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKSPACE = os.path.dirname(ROOT)          # sibling repos live beside this one
CANON_JSON = os.path.join(ROOT, "docs", "about", "canonical.json")
CANON_MD = "docs/about/canonical.md"
CORRECTION_LINES = 1                       # how far a correction marker may sit

# ---------------------------------------------------------------- Rule 1 ----
# Surfaces that must carry the block. `story` marks a surface owned by another
# story: it warns, naming the owner, and only fails under --strict.
SURFACES = [
    # the source of truth itself — the .md must agree with the .json
    {"path": CANON_MD,
     "blocks": ["one_liner", "paragraph", "token_efficiency_short_form",
                "disambiguation", "subprojects"],
     "facts": ["purpose_one_sentence"]},

    {"path": "README.md", "blocks": ["paragraph"]},

    {"path": "../toke-website/templates/index.tkt", "blocks": ["one_liner", "disambiguation"],
     "story": "132.2 — site home page + disambiguation + llms.txt"},
    {"path": "../toke-website/sites/tokelang.dev/llms.txt", "blocks": ["paragraph"],
     "story": "132.2 — site home page + disambiguation + llms.txt"},

    # repo READMEs, rewritten from the canonical block by story 132.10. These are
    # repository front pages, not registry descriptions: 132.4 owns the PyPI, npm,
    # VS Code, Ollama and Hugging Face fields, which are not files in this workspace.
    #
    # 132.14 corrected the paragraph's character-set count (55 -> 59) at the source and
    # 132.10 re-copied it into every README below, so these are enforced, not pending.
    {"path": "../toke-model/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-eval/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-mcp/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-ooke/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-tokenizer/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-corpus/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-spec/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-console/README.md", "blocks": ["paragraph"]},
    {"path": "../toke-test-programs/README.md", "blocks": ["paragraph"]},
    {"path": "../homebrew-toke/README.md", "blocks": ["one_liner"]},

    # toke-website is owned by the website stories; 132.10 may not edit that tree.
    {"path": "../toke-website/README.md", "blocks": ["paragraph"],
     "story": "132.9 / 132.2 — website roadmap + home page"},
]

NEAR_MISS = 0.55        # similarity above which an absent block is "drifted"

# ---------------------------------------------------------------- Rule 2 ----
DEFAULT_TARGETS = ["docs", "README.md", "PROJECT_STATUS.md", "AGENTS.md"]
SCAN_EXT = (".md", ".txt", ".tkt", ".html", ".json", ".ebnf", ".gbnf")

# Files owned by another story: warn, never fail, until that story lands and the
# entry is deleted. Keep this list SHORT and always keyed to a story number.
PENDING = {
    "docs/whitepaper/toke-research-language.md": "132.8 — whitepaper v2 + RFC alignment",
    "spec/rfc/draft-karwalski-toke-lang-00.md": "132.8 — whitepaper v2 + RFC alignment",
}
PENDING_PREFIXES = (
    ("docs/whitepaper/", "132.8 — whitepaper v2 + RFC alignment"),
    ("spec/rfc/", "132.8 — whitepaper v2 + RFC alignment"),
    ("../toke-website/", "132.9 / 132.2 — website roadmap + home page"),
    ("../toke-spec/", "132.8 — RFC alignment; the other toke-spec documents are "
                      "historical archives of v0.2/v0.3"),
)

# Not live claim surfaces: the tracker, historical backlogs, third-party reviews
# quoted as published, decision records and audits that record what was believed
# at the time, and the v0.3 spec (historical by definition — it carries a dated
# editor's note instead of an edit).
SKIP_PREFIXES = (
    "docs/progress.md",
    "docs/epics_and_stories.md",
    "docs/about/reviews/",
    "docs/decisions/",
    "docs/audits/",
    "docs/spec/toke-spec-v0.3.md",
    "docs/about/canonical.json",      # the rule table names the forbidden strings
    "docs/about/canonical.md",        # ditto, with its correction note
)

STALE = [
    (re.compile(r"\bLL\(1\)"), "LL(1)",
     "backtrack-free (with bounded lookahead of up to 3 tokens) — toke-spec-v0.4.md §E"),
    (re.compile(r"\b(13|thirteen)[\s-]+keywords?\b", re.I), "13 keywords",
     "14 keywords: m i t f let if el lp br rt as mt sc mut — toke-spec-v0.4.md §A"),
    (re.compile(r"\bexactly one token of lookahead\b", re.I), "one-token lookahead",
     "bounded lookahead of up to 3 tokens on an enumerated set of productions"),
]

# A marker that the claim is being retired, quoted or historicised rather than
# made. It has to be *about* the claim: a bare "not" or "never" elsewhere in the
# sentence ("no backtracking", "the parser never needs...") is not a correction,
# and treating it as one is exactly how the stale wording survived.
CORRECTION = re.compile(
    r"\bnot\b[^.\n]{0,40}LL\(1\)|never write|"
    r"\bretire(d|s)?\b|\bsupersede(d|s)?\b|\bwithdraw(n|s)?\b|"
    r"(is|was|were|as) wrong\b|\|\s*wrong\s*\||"
    r"\binaccurate\b|not accurate|\bhistorical(ly)?\b|\bformerly\b|"
    r"\bpreviously\b|\bobsolete\b|\bstale\b|\bcorrect(ed|ion)\b|"
    r"editor.s note|\b132\.12\b|except a small|except a closed|\bquot(e|ed|es)\b|"
    r"propagated|describe[sd] toke as|as published|\bassert(s|ed)\b|\bclaim(s|ed)?\b|"
    r"\bis 14\b|\bare 14\b|14 keywords|inherited|no longer", re.I)


def load_canonical():
    with open(CANON_JSON, encoding="utf-8") as fh:
        return json.load(fh)


def block_text(canon, key):
    if key in canon.get("blocks", {}):
        return canon["blocks"][key]["value"]
    if key in canon:
        return canon[key]["value"]
    raise KeyError("no canonical block named %r" % key)


ENTITIES = {"&mdash;": "—", "&ndash;": "–", "&amp;": "&", "&quot;": '"',
            "&#39;": "'", "&rsquo;": "’", "&lsquo;": "‘",
            "&ldquo;": "“", "&rdquo;": "”", "&nbsp;": " ",
            "&times;": "×", "&hellip;": "…"}


def normalise(text, markup=False):
    """Collapse the differences a faithful copy is allowed to have.

    Line wrapping, markdown blockquote markers and leading list bullets are not
    drift. For HTML/template surfaces, tags and character entities are not drift
    either. Wording, punctuation and numbers are.
    """
    if markup:
        for ent, ch in ENTITIES.items():
            text = text.replace(ent, ch)
        text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"^[ \t]*>[ \t]?", "", text, flags=re.M)     # blockquote
    text = re.sub(r"^[ \t]*[-*][ \t]+", "", text, flags=re.M)  # bullet
    text = text.replace(" ", " ")
    return re.sub(r"\s+", " ", text).strip()


def best_window(haystack, needle):
    """Closest same-length window of `haystack` to `needle`, and its ratio."""
    n = len(needle)
    if n == 0 or len(haystack) < 8:
        return "", 0.0
    best, ratio = "", 0.0
    step = max(1, n // 12)
    for start in range(0, max(1, len(haystack) - n + 1), step):
        window = haystack[start:start + n]
        r = difflib.SequenceMatcher(None, window, needle).quick_ratio()
        if r <= ratio:
            continue
        r = difflib.SequenceMatcher(None, window, needle).ratio()
        if r > ratio:
            best, ratio = window, r
    return best, ratio


def first_difference(got, want):
    sm = difflib.SequenceMatcher(None, got, want)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        return ("published: ...%s...\n                expected:  ...%s..."
                % (got[max(0, i1 - 24):i2 + 24].strip(),
                   want[max(0, j1 - 24):j2 + 24].strip()))
    return ""


def resolve(path):
    if path.startswith("../"):
        return os.path.normpath(os.path.join(WORKSPACE, path[3:]))
    return os.path.join(ROOT, path)


def check_surfaces(canon):
    """Rule 1 — every declared surface reproduces its blocks verbatim."""
    failures, warnings = [], []
    for surface in SURFACES:
        rel = surface["path"]
        full = resolve(rel)
        owner = surface.get("story")
        sink = warnings if owner else failures
        if not os.path.exists(full):
            sink.append((rel, 0, "surface not present; the block has nowhere to live",
                         "", owner))
            continue
        markup = full.endswith((".tkt", ".html", ".htm"))
        try:
            hay = normalise(open(full, encoding="utf-8", errors="replace").read(), markup)
        except OSError as exc:
            sink.append((rel, 0, "unreadable: %s" % exc, "", owner))
            continue
        for key in surface.get("blocks", []) + surface.get("facts", []):
            want = normalise(block_text(canon, key))
            if want in hay:
                continue
            got, ratio = best_window(hay, want)
            if ratio >= NEAR_MISS:
                sink.append((rel, 0, "the %r block has DRIFTED (%.0f%% match)"
                             % (key, ratio * 100), first_difference(got, want), owner))
            else:
                sink.append((rel, 0, "the %r block is MISSING" % key, "", owner))
    return failures, warnings


def owner_of(rel):
    if rel in PENDING:
        return PENDING[rel]
    for prefix, story in PENDING_PREFIXES:
        if rel.startswith(prefix):
            return story
    return None


def check_stale(path, rel):
    """Rule 2 — the two retired facts, unless the line marks them as retired."""
    findings = []
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    for i, line in enumerate(lines):
        near = "\n".join(lines[max(0, i - CORRECTION_LINES):i + CORRECTION_LINES + 1])
        near = near.replace("*", "").replace("_", "")   # markdown emphasis is not content
        for pat, label, fix in STALE:
            if pat.search(line) and not CORRECTION.search(near):
                findings.append((rel, i + 1, 'stale fact "%s" — write %s' % (label, fix),
                                 line.strip()[:200], owner_of(rel)))
                break
    return findings


def scan_files(targets):
    out = []
    for t in targets:
        p = t if os.path.isabs(t) else os.path.join(ROOT, t)
        if not os.path.exists(p) and os.path.exists(t):
            p = os.path.abspath(t)
        if os.path.isfile(p):
            out.append(p)
        elif os.path.isdir(p):
            for dirpath, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in (".git", "node_modules", "__pycache__")]
                for f in sorted(files):
                    if f.endswith(SCAN_EXT):
                        out.append(os.path.join(dirpath, f))
    return sorted(set(out))


def relpath(path):
    rel = os.path.relpath(path, ROOT)
    return rel if not rel.startswith("..") else "../" + os.path.relpath(path, WORKSPACE)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    strict = "--strict" in sys.argv
    targets = argv or DEFAULT_TARGETS

    canon = load_canonical()
    failures, warnings = check_surfaces(canon)

    scanned = 0
    for path in scan_files(targets):
        rel = relpath(path)
        if rel.startswith(SKIP_PREFIXES):
            continue
        scanned += 1
        for item in check_stale(path, rel):
            (warnings if item[4] else failures).append(item)

    if strict:
        failures += warnings
        warnings = []

    if "--list" in sys.argv:
        print("scanned %d files against %s" % (scanned, os.path.relpath(CANON_JSON, ROOT)))

    for rel, lineno, why, detail, owner in warnings:
        where = "%s:%d" % (rel, lineno) if lineno else rel
        print("WARN  %s: %s" % (where, why))
        if detail:
            print("      %s" % detail)
        print("      (owned by story %s)" % owner)

    if failures:
        print("\nERROR: the canonical facts block has drifted, or a retired fact is "
              "published as current:\n")
        for rel, lineno, why, detail, owner in failures:
            where = "%s:%d" % (rel, lineno) if lineno else rel
            print("  %s: %s" % (where, why))
            if detail:
                print("      %s" % detail)
        print("\nFix: copy the block from docs/about/canonical.md / canonical.json word "
              "for word (story 132.1), and write the accurate facts from "
              "docs/spec/toke-spec-v0.4.md §A and §E (story 132.12). If a fact itself "
              "has changed, change canonical.json and canonical.md FIRST, then re-copy "
              "downstream.")
        return 1

    print("canonical facts OK: %d surface(s) checked, %d files scanned, "
          "%d pending-story warning(s)." % (len(SURFACES), scanned, len(warnings)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
