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
it carries, verbatim modulo line wrapping, markdown code-span backticks (story
132.23) and, for HTML/template surfaces, tags and entities. A near-miss is
reported as drift with the first differing fragment; a total absence is reported
as missing.

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
historical or quoted. Claim and correction are both read after inline markup is
removed (story 132.23), so a tag, a code span, a link or an entity neither hides
a correction nor smuggles a stale claim past the gate. The mechanical argument is unaffected and is what should be
written instead: a small backtrack-free grammar with bounded lookahead is still
cheap to constrain during decoding and cheap to parse.

Usage:  python3 scripts/check_canonical.py [paths...] [--strict] [--list]
Exit:   0 clean (pending-story surfaces and files warn only), 1 on a violation.
        --strict also fails on the pending-story entries.
"""
import difflib
import html
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

# Story 132.29 — a NEGATION of the retired fact itself: "it is NOT strict LL(1)",
# "there are not 13 keywords". This is built per stale fact from that fact's own
# pattern (see negation_of), so it is the claim being denied and not some other
# "not" in the paragraph: the negator has to sit within 40 characters of the
# claim with no sentence boundary between them.
NEGATOR = r"\b(?:not|no longer|never|isn['’]?t|aren['’]?t|wasn['’]?t|weren['’]?t)\b"

# A marker that the claim is being retired, quoted or historicised rather than
# made. It has to be *about* the claim: a bare "not" or "never" elsewhere in the
# sentence ("no backtracking", "the parser never needs...") is not a correction,
# and treating it as one is exactly how the stale wording survived.
CORRECTION = re.compile(
    r"never write|"
    r"\bretire(d|s)?\b|\bsupersede(d|s)?\b|\bwithdraw(n|s)?\b|"
    r"(is|was|were|as) wrong\b|\|\s*wrong\s*\||"
    r"\binaccurate\b|not accurate|\bhistorical(ly)?\b|\bformerly\b|"
    r"\bpreviously\b|\bobsolete\b|\bstale\b|\bcorrect(ed|ion)\b|"
    r"editor.s note|\b132\.12\b|except a small|except a closed|\bquot(e|ed|es)\b|"
    r"propagated|describe[sd] toke as|as published|\bassert(s|ed)\b|\bclaim(s|ed)?\b|"
    r"\bis 14\b|\bare 14\b|14 keywords|inherited|no longer", re.I)

_NEGATION_CACHE = {}


def negation_of(pat):
    """Regex matching an explicit denial of `pat`'s claim, wrapped or not."""
    if pat.pattern not in _NEGATION_CACHE:
        _NEGATION_CACHE[pat.pattern] = re.compile(
            NEGATOR + r"[^.\n]{0,40}(?:" + pat.pattern + ")", re.I)
    return _NEGATION_CACHE[pat.pattern]


# Story 132.23 — MARKUP BLINDNESS. This is a different defect from 132.29's
# line-wrap blindness: there the correction was on another *line*, here it is
# behind a *tag*. Rule 2 matched raw source text, so inline markup defeated both
# halves of the test. The built v0.4 spec page states the correction as
# `The keyword set is <strong>14</strong>` and the CORRECTION marker `\bis 14\b`
# could not see it, so a page that gets the fact right was reported as drift.
# The same blindness runs the other way and is the dangerous half: a genuinely
# stale `<strong>13</strong> keywords`, `` `13 keywords` `` or `[LL(1)](...)`
# slipped past the gate entirely. Markup is presentation; the claim is the text
# a reader sees. Both the claim line and the correction window are de-marked-up
# before matching, so emphasis, a code span, a link or an entity changes nothing
# about what the gate reads — in either direction.
INLINE_TAG = re.compile(r"</?[A-Za-z][A-Za-z0-9:.-]*(?:\s[^<>]*)?/?>")
MD_LINK = re.compile(r"\[([^\]\n]*)\]\([^()\s]*\)")


def demarkup(text):
    """The text a reader sees: inline markup removed, entities resolved.

    A tag becomes a space and the run is collapsed, so `is <strong>14</strong>`
    reads as "is 14" and `<strong>13</strong> keywords` reads as "13 keywords".
    Only a well-formed tag is stripped: a bare `<` in prose or code ("a < b > c")
    is left alone, because deleting it could *hide* a claim, and this function
    must never make the gate blinder than the raw text. Tags go before entities
    are resolved, so an escaped, literal `&lt;strong&gt;` stays visible text.
    """
    text = INLINE_TAG.sub(" ", text)
    text = MD_LINK.sub(r"\1", text)
    text = text.replace("`", "")                    # a code span is a delimiter
    text = html.unescape(text)
    text = text.replace("*", "").replace("_", "")   # emphasis is not content
    return re.sub(r"[^\S\n]+", " ", text.replace("\u00a0", " "))


def near_text(lines, i):
    """The window around line `i`, re-joined into the paragraph it belongs to.

    Story 132.29. Prose is hard-wrapped, so a sentence that retires a fact puts
    the negation and the fact on different lines:

        The grammar is backtrack-free with bounded lookahead of up to 3 tokens
        — it is NOT strict LL(1) (spec v0.4 §A keywords, §E grammar).

    Scoring each line on its own cannot see that "NOT", so the gate failed the
    one document that stated the retirement correctly. Re-joining the window
    restores the sentence. What keeps this from excusing real drift is that the
    proximity rules are unchanged: a negator still has to sit within 40
    characters of the claim with no `.` between them, so a "not" belonging to a
    neighbouring sentence is still not a correction — and a blank line is a
    paragraph boundary that is never joined across.

    Story 132.23 then de-marks-up the re-joined window, so a correction that
    is emphasised, linked or written as a code span still reads as one.
    """
    lo = max(0, i - CORRECTION_LINES)
    hi = min(len(lines), i + CORRECTION_LINES + 1)
    window = lines[lo:hi]
    k = i - lo                                   # the matched line, in `window`
    start = k
    while start > 0 and window[start - 1].strip():
        start -= 1
    end = k + 1
    while end < len(window) and window[end].strip():
        end += 1
    text = " ".join(ln.strip() for ln in window[start:end])
    return demarkup(text)                           # 132.23: markup is not content


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
    # Story 132.23 — a backtick is a markdown *delimiter*, not content. The
    # canonical paragraph and disambiguation blocks write `.tk` as a code span;
    # an HTML or template surface renders that as <code>.tk</code>, which the
    # tag-strip above reduces to a bare `.tk`. Comparing the two left a faithful
    # copy stuck at ~98% — indistinguishable from real drift, which is the whole
    # point of the rule. Stripped from BOTH sides so the comparison is of words.
    text = text.replace("`", "")
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
        near = near_text(lines, i)
        # 132.23 — match what a reader sees, not the source: `13 keywords` and
        # <strong>13</strong> keywords are the same claim. The RAW line is still
        # what gets reported, so the human sees the text that has to change.
        claim = demarkup(line)
        for pat, label, fix in STALE:
            if not pat.search(claim):
                continue
            if CORRECTION.search(near) or negation_of(pat).search(near):
                continue
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


# --------------------------------------------------------------- selftest ----
# Story 132.29. Rule 2 was loosened so that a sentence which explicitly retires
# a fact stops being reported as that fact. A loosened gate is only worth having
# if it still fails on real drift, so the cases below pin BOTH halves, and the
# negative half is the point: every ACCEPT case here is a correction the gate
# must let through, and every REJECT case is wording that must keep failing.
SELFTEST = [
    # (accepted?, name, text)
    (True, "wrapped negation (toke-corpus/regen/syntax_card.md:26-27)",
     "The grammar is backtrack-free with bounded lookahead of up to 3 tokens — it is NOT\n"
     "strict LL(1) (spec v0.4 §A keywords, §E grammar, §G character set)."),
    (True, "negation on one line",
     "The grammar is not LL(1)."),
    (True, "explicitly retired",
     "The LL(1) claim was retired by the v0.4 spec."),
    (True, "wrapped keyword-count negation",
     "There are 14 keywords, so the v0.3 wording was not\n"
     "13 keywords as published."),

    # ---- the negative half: genuine drift, which must still fail ----
    (False, "bare claim", "The grammar is LL(1)."),
    (False, "bare claim, wrapped",
     "The grammar is context-free and\nLL(1) (every production is decidable)."),
    (False, "bare keyword count", "toke has 13 keywords."),
    (False, "one-token lookahead", "The parser needs exactly one token of lookahead."),
    (False, "negation of something else in the previous sentence",
     "The parser does not backtrack. The grammar is LL(1)."),
    (False, "negation of something else, wrapped",
     "The parser does not backtrack over the token stream\n"
     "and the grammar is LL(1)."),
    (False, "negation across a paragraph break",
     "That is not what we do\n\nThe grammar is LL(1)."),
    (False, "negation too far from the claim",
     "It is not the case that the grammar described in ADR-0001 is strict LL(1)."),
    (False, "correction two lines away, outside the window",
     "The v0.3 spec said the grammar is LL(1).\n"
     "It shipped that way for months.\n"
     "That claim was retired."),

    # ---- story 132.23: the same two halves, now behind inline markup ----
    # Accepted: a correction the reader can see but the raw source hides.
    (True, "correction in a <strong> tag (toke-website built v0.4 spec page, 170-171)",
     "<p>The keyword set is <strong>14</strong> (verified against the lexer keyword table):\n"
     "<code>m i t f let if el lp br rt as mt sc mut</code>. The v0.3 &quot;13 keywords&quot; wording\n"
     "(and the <code>grammar.ebnf</code> header) is retired."),
    (True, "correction in a code span",
     "The keyword set is `14` (verified against the lexer keyword table).\n"
     "The v0.3 13 keywords list is the one that shipped."),
    (True, "correction behind a markdown link",
     "The keyword set is [14](/docs/spec/toke-spec-v0.4#a).\n"
     "The v0.3 13 keywords list is the one that shipped."),
    (True, "correction separated by a non-breaking space entity",
     "The keyword set is&nbsp;14.\nThe v0.3 13 keywords list is the one that shipped."),
    (True, "negation inside emphasis tags",
     "The grammar is <em>not</em> <strong>LL(1)</strong>."),

    # ---- and the half that matters: drift dressed up in markup still fails ----
    (False, "stale claim in a <strong> tag",
     "toke has <strong>13</strong> keywords."),
    (False, "stale claim in a code span",
     "toke has `13 keywords`."),
    (False, "stale claim behind a markdown link",
     "The grammar is [LL(1)](/docs/decisions/ADR-0001)."),
    (False, "stale claim in emphasis, wrapped across a line",
     "The grammar is context-free and\n<em>LL(1)</em> (every production is decidable)."),
    (False, "stale claim as an HTML-entity quotation with no correction",
     "The spec calls the grammar &quot;<b>LL(1)</b>&quot; throughout."),
    (False, "markup negation of a different clause in the previous sentence",
     "The parser does <strong>not</strong> backtrack. The grammar is <em>LL(1)</em>."),
    (False, "markup negation too far from the claim",
     "It is <strong>not</strong> the case that the grammar described in ADR-0001 "
     "is strict <em>LL(1)</em>."),
    (False, "a tag is not a paragraph break: the correction is still two lines away",
     "<p>The v0.3 spec said the grammar is <em>LL(1)</em>.</p>\n"
     "<p>It shipped that way for months.</p>\n"
     "<p>That claim was retired.</p>"),
    (False, "an escaped, literal tag is text and hides nothing",
     "Write &lt;strong&gt;13 keywords&lt;/strong&gt; in the card."),
    (False, "a bare less-than in prose is not a tag and must not eat the claim",
     "When a < b > c holds, the grammar is LL(1)."),
]

# Rule 1 (story 132.23). The canonical blocks write `.tk` as a markdown code
# span; an HTML surface renders it as <code>.tk</code>. Before the backtick was
# dropped from both sides, a byte-faithful HTML copy scored ~98% and was
# reported as DRIFTED, so the one signal the rule exists to give — this copy has
# been re-worded — could not be told from a formatting artefact. The negative
# case is the point: a copy that really has been re-worded must still be caught.
SELFTEST_RULE1 = [
    # (should_match?, name, canonical text, surface text, markup?)
    (True, "code span vs <code> rendering",
     "not Tokelau or its `.tk` country-code domain, and not tokelang.com",
     "<p>not Tokelau or its <code>.tk</code> country-code domain, and not "
     "tokelang.com</p>", True),
    (True, "code span vs code span (markdown surface)",
     "not Tokelau or its `.tk` country-code domain",
     "Preamble.\n\nnot Tokelau or its `.tk` country-code domain\n", False),
    (True, "entity and line wrapping are still not drift",
     "toke — a language",
     "<p>toke &mdash;\na language</p>", True),
    (False, "a re-worded HTML copy is still drift",
     "not Tokelau or its `.tk` country-code domain, and not tokelang.com",
     "<p>not Tokelau or the <code>.tk</code> domain, and not tokelang.com</p>", True),
    (False, "a changed number is still drift",
     "It has 14 keywords and a 59-character set",
     "<p>It has <strong>13</strong> keywords and a 59-character set</p>", True),
]


def selftest_rule1():
    """Rule 1 — a faithful copy matches; a re-worded one still does not."""
    bad = 0
    for should, name, want_raw, surface, markup in SELFTEST_RULE1:
        want = normalise(want_raw)
        hay = normalise(surface, markup)
        ok = (want in hay) if should else (want not in hay)
        if not ok:
            bad += 1
            _, ratio = best_window(hay, want)
            print("SELFTEST FAIL [rule 1 %s] %s: %.0f%% match"
                  % ("match" if should else "drift", name, ratio * 100))
    return bad


def selftest():
    import tempfile
    bad = selftest_rule1()
    for accept, name, text in SELFTEST:
        with tempfile.NamedTemporaryFile("w", suffix=".md", encoding="utf-8",
                                         delete=False) as fh:
            fh.write(text + "\n")
            tmp = fh.name
        try:
            found = check_stale(tmp, "selftest.md")
        finally:
            os.unlink(tmp)
        ok = (not found) if accept else bool(found)
        if not ok:
            bad += 1
            print("SELFTEST FAIL [%s] %s: %s"
                  % ("accept" if accept else "reject", name,
                     "flagged: %s" % found[0][2] if found else "not flagged"))
    total = len(SELFTEST) + len(SELFTEST_RULE1)
    if bad:
        print("\nselftest: %d of %d cases wrong — the canonical-facts gate is not "
              "behaving as documented." % (bad, total))
        return 1
    print("selftest: %d cases OK — %d Rule 2 (%d corrections accepted, %d stale "
          "claims still rejected) and %d Rule 1 (%d faithful copies matched, "
          "%d re-wordings still caught)."
          % (total, len(SELFTEST),
             sum(1 for c in SELFTEST if c[0]), sum(1 for c in SELFTEST if not c[0]),
             len(SELFTEST_RULE1),
             sum(1 for c in SELFTEST_RULE1 if c[0]),
             sum(1 for c in SELFTEST_RULE1 if not c[0])))
    return 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
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
