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

Rule 1b — REGISTRY SURFACES (story 132.4). A registry description is a field on
someone else's website. Where it is fed by a file we commit (a package manifest,
a PyPI long description, a model card) that file is an enforced surface above.
Where it is only a web form, it is listed in AWAITING_PUBLISH and warns until the
owner publishes it — the nine entries there are the standing record that prepared
text is not the same thing as live text. `--strict` fails on them.

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

    # ------------------------------------------------------------ registries --
    # Story 132.4. A registry description is not a repo README: it is a field on
    # someone else's website. Where that field is fed by a file we commit — a
    # package manifest, a long description, a model card — the file is an
    # enforced surface here, so the text cannot drift between releases and a
    # publish is a release rather than a retype. Where the field exists only as a
    # web form (every GitHub description, and each registry's live page until the
    # next release), it is in AWAITING_PUBLISH below and warns instead.
    {"path": "../toke-tokenizer/python/pyproject.toml", "blocks": ["one_liner"]},
    {"path": "../toke-tokenizer/python/README.md", "blocks": ["one_liner"]},
    {"path": "../toke-tokenizer/docs/hf-model-card.md",
     "blocks": ["one_liner", "paragraph"]},
    {"path": "../toke-mcp/package.json", "blocks": ["one_liner"]},
    {"path": "../toke-mcp/npm-tkc/package.json", "blocks": ["one_liner"]},
    {"path": "../toke-mcp/vscode-toke/package.json", "blocks": ["one_liner"]},
    {"path": "../toke-mcp/vscode-toke/README.md", "blocks": ["one_liner", "paragraph"]},
    {"path": "../toke-model/huggingface/README.md", "blocks": ["one_liner", "paragraph"]},
    {"path": "../toke-model/ollama/README.md", "blocks": ["one_liner"]},
]

# ------------------------------------------------------- awaiting publish ----
# Story 132.4 prepared the text for nine registry surfaces; publishing any of
# them is an outward-facing action the owner has to take, with credentials this
# repository does not hold. Each entry warns — naming what is live today, where
# the prepared text lives, and the command that applies it — and keeps warning
# until the owner publishes and the entry is deleted. Under --strict they fail,
# which is how story 132.5 can assert "every registry surface is live".
#
# The prose, the live text each one replaces and the owner checklist are in
# docs/about/registry-descriptions.md.
AWAITING_PUBLISH = [
    {"surface": "GitHub karwalski/toke — description, homepage, 5 topics",
     "live": "description empty, no topics, no homepage (2026-09-19)",
     "source": "docs/about/registry-descriptions.md §1",
     "apply": "gh repo edit karwalski/toke --description ... (see §1)"},
    {"surface": "GitHub — the other 14 toke repos",
     "live": "every public repo description empty (2026-09-19)",
     "source": "scripts/about/github_repo_descriptions.py",
     "apply": "python3 scripts/about/github_repo_descriptions.py | sh"},
    {"surface": "PyPI toke-tokenizer",
     "live": "0.1.0 still advertises the withdrawn \"approximately 52% token "
             "reduction\" and does not say the wheel is the v0.3 tokenizer",
     "source": "../toke-tokenizer/python/pyproject.toml + python/README.md (0.1.1)",
     "apply": "python3 -m build && python3 -m twine upload dist/toke_tokenizer-0.1.1*"},
    {"surface": "npm @tokelang/mcp-server",
     "live": "0.1.0 description capitalises \"Toke\" and carries no one-liner",
     "source": "../toke-mcp/package.json (0.1.1)",
     "apply": "cd ~/tk/toke-mcp && npm publish --access public"},
    {"surface": "npm @tokelang/tkc",
     "live": "never published",
     "source": "../toke-mcp/npm-tkc/package.json (0.3.0)",
     "apply": "cd ~/tk/toke-mcp/npm-tkc && npm publish --access public"},
    {"surface": "VS Code marketplace tokelang.toke-language",
     "live": "0.2.2 display name and description capitalise \"Toke\"; listing "
             "never says what toke is",
     "source": "../toke-mcp/vscode-toke/package.json + README.md (0.2.3)",
     "apply": "cd ~/tk/toke-mcp/vscode-toke && npx vsce publish"},
    {"surface": "Hugging Face karwalski/toke (model card)",
     "live": "card carries the withdrawn 52% headline, the retired keyword count, "
             "a 55-character alphabet and an unqualified 100%",
     "source": "../toke-model/huggingface/README.md",
     "apply": "huggingface-cli upload karwalski/toke "
              "~/tk/toke-model/huggingface/README.md README.md"},
    {"surface": "Hugging Face karwalski/toke-tokenizer (model card)",
     "live": "card carries the withdrawn 52% and the per-example 19-vs-49 figure",
     "source": "../toke-tokenizer/docs/hf-model-card.md",
     "apply": "huggingface-cli upload karwalski/toke-tokenizer "
              "~/tk/toke-tokenizer/docs/hf-model-card.md README.md"},
    {"surface": "Ollama karwalski/toke",
     "live": "not published (404, 2026-09-19)",
     "source": "../toke-model/ollama/README.md § Registry description",
     "apply": "ollama push karwalski/toke, then paste the description from the "
              "source file"},
]

NEAR_MISS = 0.55        # similarity above which an absent block is "drifted"

# ---------------------------------------------------------------- Rule 2 ----
DEFAULT_TARGETS = ["docs", "README.md", "PROJECT_STATUS.md", "AGENTS.md"]
SCAN_EXT = (".md", ".txt", ".tkt", ".html", ".json", ".ebnf", ".gbnf",
            # story 132.15: the sibling repos publish facts from code too — MCP tool
            # descriptions (`toke-mcp/tools/*.js`), generator system prompts
            # (`toke-corpus/scripts/*.py`) and console page copy (`*.php`).
            ".py", ".js", ".php")

# Story 132.15: the guard also runs across the sibling repos, by passing their
# paths (absolute, or relative to this repo: `../toke-spec`). Claim surfaces are
# prose and metadata, so the walk prunes dependency trees and generated data —
# `../toke-corpus` alone holds 493,593 corpus JSON records.
EXCLUDE_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", "site-packages",
    ".venv", "venv", ".tox", ".mypy_cache", ".pytest_cache", ".ruff_cache",
    ".cache", ".next", "dist", "build", "target", "vendor", "coverage",
    "corpus", "clean", "data", "store", "logs", "results", "output", "outputs",
    "checkpoints", "training-data",
}

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
    # 132.15 swept the rest of toke-spec: the v0.2/v0.3 documents now carry dated
    # archive banners and the live ones were corrected, so only the RFC is still pending.
    ("../toke-spec/rfc/", "132.8 — RFC alignment (the toke-spec RFC draft is v0.3-era)"),
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
    # canonical.json is NOT skipped: its "forbidden" object names the retired strings,
    # and only that object is exempt (see claims_table_lines below) — the rest of the
    # file is a live claim surface and is scanned like any other.
    "docs/about/canonical.md",        # the rule table, with its correction note
)

ARCHIVE_BANNER = re.compile(r"\*\*Archived\s+20\d\d-\d\d-\d\d", re.I)
ARCHIVE_HEAD = 40

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


def check_awaiting():
    """Story 132.4 — registry fields whose publish the owner still has to run.

    These are not drifted copies; they are correct text that is not live yet.
    Reported in the warning shape used everywhere else so that --strict turns
    them into failures once story 132.5 expects them all published.
    """
    out = []
    for entry in AWAITING_PUBLISH:
        out.append((entry["surface"], 0,
                    "awaiting an owner-executed publish — live today: %s" % entry["live"],
                    "prepared text: %s\n      apply:         %s"
                    % (entry["source"], entry["apply"]),
                    "132.4 — registry descriptions (publish, then delete this entry)"))
    return out


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
    # Story 132.15: a document that opens with `**Archived YYYY-MM-DD ...**` declares
    # itself a dated record rather than a live claim surface, the way
    # `docs/spec/toke-spec-v0.3.md` does. The banner has to name the correction — the
    # sibling-repo banners written by 132.15 all do — and it lives in the document,
    # where a reader sees it.
    if ARCHIVE_BANNER.search("\n".join(lines[:ARCHIVE_HEAD])):
        return findings
    # Story 132.15: a JSON claims table (canonical.json's "forbidden" object, a
    # "retired_claim" record) names the strings being retired — "LL(1)" and "13
    # keywords" are its KEYS. Only the table's own lines are exempt, located by brace
    # matching in check_metrics_claims.claims_table_lines, so the same wording asserted
    # anywhere else in the file still fails.
    table_lines = frozenset()
    if path.endswith(".json"):
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from check_metrics_claims import claims_table_lines
            table_lines = claims_table_lines(path)
        except Exception:                                    # pragma: no cover
            table_lines = frozenset()
    for i, line in enumerate(lines):
        if (i + 1) in table_lines:
            continue
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
                dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
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
    warnings += check_awaiting()

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
          "%d pending-story warning(s) (%d of them registry fields awaiting an "
          "owner-executed publish, story 132.4)."
          % (len(SURFACES), scanned, len(warnings), len(AWAITING_PUBLISH)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
