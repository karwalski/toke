#!/usr/bin/env python3
"""
check_metrics_claims.py — token-efficiency claim gate (Story 132.6).

Why this exists
---------------
"52% average token reduction vs cl100k" shipped for months with none of the
TEMSpec (`toke-spec/docs/temspec.md`) §6.3 reporting fields attached. Read
literally it is a *tokenizer-vs-tokenizer* number on identical toke text; read
the way every reader actually read it, it says "toke needs half the tokens of
Python", which is false — under one shared tokenizer toke costs ~1.34x Python
(`docs/metrics-baseline.md`). A second family of claims ("42% reduction vs
Python") was worse: it counted the toke side with a *toke-trained* tokenizer and
the Python side with cl100k, i.e. it measured the tokenizer's training bias.

This gate stops both from coming back.

Rule 1 — QUALIFIERS (TEMSpec §6.3). A sentence that states a percentage about
tokens in comparative terms ("X% fewer tokens", "X% token reduction", "X% more
tokens than ...") must name a tokenizer and a sample size. N may come from the
sentence itself or from the surrounding context block (same section, up to
CONTEXT_LINES above).

Rule 4 — COUNTS OF THINGS (story 132.14). "65 EBNF productions" (53), "38
standard library modules" (57), "62+ conformance tests" (228), "12 keywords"
(14), "a 55-character set" (59) — every one of these shipped, every one was
checkable in seconds, and none of them was checked. A number immediately
preceding one of the counted nouns must match the fact sheet derived by
`scripts/verify_project_facts.py`, or carry a dated exemption
(`facts-exempt: YYYY-MM-DD ...` on the line or within two lines of it) marking
it as a historical snapshot. A date or era label excuses a count only when it
sits in the count's own sentence (story 132.88) or, in a table, its own cell
or the row's date-only cell (132.93); a date elsewhere dates something else.
The same holds for a supersession or era cue ("withdrawn", "previously",
"v0.2"): in a table it excuses a count only from the count's own cell, a
status-only cell in its row, its column's header or a note below the table
(132.96).

Rule 2 — LANE CROSSING (story 132.13). A toke-trained tokenizer (Toke-16K,
tokenizer_v03, proxy8k, the toke SentencePiece models) must never be named in a
comparison alongside a non-toke baseline language. Sentences that *forbid* the
practice are recognised and allowed.

Usage:  python3 scripts/check_metrics_claims.py [paths...] [--self] [--strict] [--list]
        python3 scripts/check_metrics_claims.py --selftest
        A path argument resolves against the current directory, like any other
        command-line tool (story 132.79); with no path, the defaults below are
        scanned in this repository wherever the command is run from.
        --self scans those defaults AS WELL AS the given paths (story 132.108):
        `make check-claims-all` passes it, because it names only the sibling
        repositories and skips their mirrors of toke's docs as "scanned at the
        source" -- without --self the source was scanned by nothing.
Exit:   0 clean (pending-story files warn only), 1 on a violation.
        --strict also fails on the pending-story files.

--selftest (story 132.41) is the gate's negative control and `make check-metrics`
runs it first. It feeds fixed inputs through check_file(): real defects this
guard has shipped past, which must keep failing, and wordings that must keep
passing (132.47/132.48/132.60/132.61 added fourteen). Run it after any change to a rule — switching rule 1 off
makes it exit 1, which is how it was verified. A gate nobody has watched fail is
not a gate.
"""
import functools
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTEXT_LINES = 30

# Default scan set: the published doc tree plus the repo's front-page files.
# 127.261: plus the Makefile -- its comments and help text are read by every
# contributor, and it stated "56-character"/"12 keywords" with no gate seeing it
# (it has no extension, so neither this default nor a directory walk reached it).
DEFAULT_TARGETS = ["docs", "README.md", "PROJECT_STATUS.md", "Makefile"]

# Story 132.15: the guard is run across the sibling repos too, by passing their
# paths (absolute, or relative to the current directory -- `make check-claims-all`
# runs from this repo's root, so `../toke-spec` is the sibling). Claim surfaces are
# prose and metadata, so the walk prunes dependency trees and generated data —
# without this, `../toke-corpus` alone is 493,593 corpus JSON records and
# `../toke-mcp/node_modules` reports a third party's "415 error code".
EXCLUDE_DIRS = {
    ".git", ".hg", ".svn", "node_modules", "__pycache__", "site-packages",
    ".venv", "venv", ".tox", ".mypy_cache", ".pytest_cache", ".ruff_cache",
    ".cache", ".next", "dist", "build", "target", "vendor", "coverage",
    # generated data, not claims: corpus shards, run outputs, checkpoints
    "corpus", "clean", "data", "store", "logs", "results", "output", "outputs",
    "checkpoints", "training-data",
}

# Prose and metadata surfaces. `.json` catches package metadata and tool
# descriptions (`toke-mcp/package.json`, plugin manifests); the code extensions catch
# the places 132.15 actually found withdrawn claims still shipping — MCP tool
# descriptions (`toke-mcp/tools/*.js`), generator system prompts
# (`toke-corpus/scripts/*.py`), a Hugging Face Space that synthesised toke token counts
# from Python's by multiplying by 0.875, and console page copy (`*.php`).
SCAN_EXT = (".md", ".txt", ".tkt", ".html", ".json", ".py", ".js", ".php",
            ".yaml", ".yml", ".toml")

# Files owned by another story: reported as warnings, never fail the build,
# until that story lands and the entry is deleted. Keep this list SHORT and
# always keyed to a story number.
PENDING = {
    "docs/whitepaper/toke-research-language.md": "132.8 — whitepaper v2 + RFC alignment",
    "docs/about/positioning-2026-09.md": "132.7 — repositioning brief (in flight)",
}

# Same idea, by prefix — used for the sibling repos swept by story 132.15.
#
# Story 132.42 removed ("../toke-website/", "132.2 / 132.9 / 132.16 — website
# copy"). That one line exempted THE ENTIRE PUBLIC WEBSITE — every template,
# every static page, every rendered doc — from rule one. It passed with 44
# detected, permitted failures, among them a live "30+ standard library
# modules" on the home page and a withdrawn 52% headline in built HTML. A
# pending entry is meant to be one file with one owner for one sprint; a
# whole-repo prefix aimed at the most public surface the project has is not a
# pending entry, it is an exemption, and it held for long enough that the
# findings it was hiding were rediscovered by a separate audit.
#
# Do not re-add a repository-wide prefix here. If a specific file is mid-story,
# name that file in PENDING above, with its story number.
#
# 132.61: a sibling-repo prefix is written `<repo>/<path>` and matched against
# repo_key() -- the repository's IDENTITY, read from git -- never against a path
# relative to this checkout. `../toke-spec/rfc/` only ever matched when this
# repo and toke-spec were both checked out side by side under their own names;
# from a worktree of either, the entry silently stopped applying.
PENDING_PREFIXES = (
    ("toke-spec/rfc/", "132.8 — RFC alignment (the toke-spec draft is v0.3-era)"),
)


@functools.lru_cache(maxsize=None)
def _repo_of(dirpath):
    """(repo name, repo root) for a directory, from git's own record.

    A worktree's `.git` is a file naming `<main>/.git/worktrees/<wt>`, so the
    name is the MAIN checkout's, whatever the worktree directory is called.
    """
    d = dirpath
    while True:
        g = os.path.join(d, ".git")
        if os.path.isdir(g):
            return os.path.basename(d), d
        if os.path.isfile(g):
            try:
                line = open(g, encoding="utf-8").read().strip()
            except OSError:
                line = ""
            m = re.match(r"gitdir:\s*(.+?)/\.git(?:/worktrees/[^/]+)?/?$", line)
            if m:
                return os.path.basename(m.group(1)), d
            return os.path.basename(d), d
        parent = os.path.dirname(d)
        if parent == d:
            return None, None
        d = parent


def repo_key(path):
    """`<repo>/<path inside it>` -- stable across checkouts and worktrees."""
    norm = os.path.normpath(os.path.abspath(path))
    name, root = _repo_of(os.path.dirname(norm) if not os.path.isdir(norm) else norm)
    if not name:
        return os.path.relpath(norm, ROOT)
    return name + "/" + os.path.relpath(norm, root).replace(os.sep, "/")


def pending_owner(rel, path=None):
    if rel in PENDING:
        return PENDING[rel]
    key = repo_key(path) if path else None
    if key and key in PENDING:
        return PENDING[key]
    for prefix, story in PENDING_PREFIXES:
        if rel.startswith(prefix) or (key and key.startswith(prefix)):
            return story
    return None

# Not public claim surfaces: internal trackers, third-party review records that
# quote someone else's numbers verbatim, and the measurement records that define
# the lanes (they necessarily print every lane side by side).
SKIP_PREFIXES = (
    "docs/progress.md",              # the tracker; story text quotes claims to fix them
    "docs/epics_and_stories.md",     # historical backlog
    "docs/about/reviews/",           # third-party reviews, quoted as published
    "docs/decisions/",               # decision records quoting the options considered
    "docs/metrics-baseline.md",      # the source of truth; it defines the wording
    "docs/about/samples-v04.md",     # the per-lane dataset, with its own do-not rules
    "docs/reference/token-comparison.md",  # v0.3 dataset, requalified in place by 132.6
)

PCT = re.compile(r"(?<![\w.])\d{1,3}(?:\.\d+)?\s?%")
TOKEN_WORD = re.compile(r"\btoken(s|izer|izers|ization|ised|ized)?\b", re.I)
# comparative cue — without one, a percentage near "token" is a utilisation,
# coverage or fertility statistic, not a claim about being cheaper.
COMPARATIVE = re.compile(
    r"\b(reduction|reduce[sd]?|fewer|more|less|saving|savings|cheaper|denser|"
    r"compression|vs\.?|versus|than|compared|beats?|below|above|improvement)\b", re.I)
# the claim has to be about toke; third-party results carry their own citation
TOKE_SUBJECT = re.compile(
    r"\btoke\b|\btk\b|toke-?16k|toke-?bpe|tokenizer_v03|proxy8k|purpose-built|"
    r"\bGate [12]\b|\bour\b|\bwe\b", re.I)
CITATION = re.compile(
    r"arXiv|et al\.|doi|https?://|\b(ICML|NeurIPS|ICLR|EMNLP|ACL|ISSTA|PLDI|OSDI)\b|"
    r"\breports?\b|\bpublish(es|ed)\b|\(20\d\d\)", re.I)
# a stated target/threshold is not a published result, so §6.3 does not bite
UNMEASURED = re.compile(
    r"\b(target(s|ed|ing)?|goal|aim(s|ed)?|projected|expected|estimate[sd]?|"
    r"threshold|criteri(a|on)|require[sd]?|gate|must (be|exceed|reach)|"
    r"hypothes(is|ised|ized))\b", re.I)
MEASURED = re.compile(
    r"\b(measured|achieve[sd]?|observed|result(s|ed)?|reproduce[sd]?|"
    r"PASS\b|recorded|reports? that we)\b", re.I)

TOKENIZERS = re.compile(
    r"\b(cl100k(_base)?|o200k(_base)?|tiktoken|sentencepiece|sp8k|sp32k|"
    r"toke-?16k|toke-?bpe|tokenizer_v03|proxy8k|qwen[\s.]?2\.5[- ]?coder|"
    # 131.50: the superword and byte lanes of scripts/patterns/count_tokens.py
    r"superword8k|bytepatch|byte-patch|byte256|"
    r"byte-level BPE|8K? ?(vocab|BPE|SentencePiece)|16K? ?(vocab|BPE))\b", re.I)

# Sample size: an explicit N, or a count followed by a unit noun.
N_FIELD = re.compile(
    r"(\bN\s?=\s?[\d,]+)|"
    r"\b[\d,]{1,9}\s+(?:[a-z-]+\s+){0,3}(benchmark(s| programs)?|programs?|tasks?|"
    r"records?|pairs?|examples?|samples?|solutions?|files?|modules?)\b", re.I)

# Rule 2: tokenizers trained on toke text — valid on the toke side only.
TOKE_TRAINED = re.compile(
    # 131.50: superword8k and bytepatch are trained on the same toke corpus as proxy8k
    r"\b(toke-?16k|toke-?bpe|tokenizer_v03|proxy8k|superword8k|bytepatch|toke's own (BPE|tokenizer)|"
    r"toke BPE|purpose-built (16K |8K )?(BPE|SentencePiece|tokenizer)|toke tokenizer|"
    # 132.15: TEMSpec v1.0 §6.2 wrote it as "the toke-specific BPE tokenizer"
    r"toke-specific (BPE )?tokenizer|custom SentencePiece)\b", re.I)
BASELINE_LANG = r"(Python|Java|JavaScript|TypeScript|Go|Rust|C\+\+|C#)"
# the crossing is only a crossing when the two are actually compared
CROSS_COMPARE = re.compile(
    r"\b(vs\.?|versus|than|compared (to|with)|against|over|beats?)\s+"
    r"(the\s+|equivalent\s+|a\s+)?" + BASELINE_LANG + r"\b", re.I)
# a sentence that forbids or reports the lane-crossing error rather than making it
PROHIBITION = re.compile(
    r"\b(never|must not|cannot|can't|do not|don't|withdrawn|withdraw|invalid|"
    r"out of domain|out-of-domain|training bias|methodology error|error behind|"
    r"not be (run|applied|quoted)|only ever|one tokenizer on both sides|"
    r"two different tokenizers|same tokenizer on both|controls for tokenizer)\b", re.I)

# Rule 3 — headline figures that were withdrawn (132.6 / 132.13). They may be
# *described* as past claims, but only alongside an explicit supersession marker.
WITHDRAWN = [
    (re.compile(r"\b52(\.\d+)?\s?%"), "the v0.3 Toke-16K \"52% token reduction\" headline"),
    (re.compile(r"\b42\s?%\s*(token\s*)?(reduction|fewer)"), "the \"42% reduction vs Python\" claim"),
    (re.compile(r"\b31\s?%\s*(fewer|token)"), "the \"31% fewer tokens than Python\" claim"),
    (re.compile(r"\b48\s?%\s*(fewer|token)"), "the \"48% fewer tokens than Go\" claim"),
    # 132.15: the Gate-2 review package published "63.0% fewer tokens than Python",
    # "73.5% vs Java", "84.9% vs C" and a "73.8% mean reduction" — all with a tokenizer
    # and an N attached, so rules 1 and 2 passed them. The direction is what is wrong:
    # under one shared tokenizer toke costs 1.34x [1.22, 1.48] the tokens of equivalent
    # Python (N = 60). No "toke needs fewer tokens than <baseline language>" claim is
    # supportable at any percentage.
    (re.compile(r"\b\d{1,3}(\.\d+)?\s?%\s*(fewer|less)\s+tokens?\s+than\s+"
                r"(equivalent\s+|the\s+|a\s+)*" + BASELINE_LANG, re.I),
     "a \"N% fewer tokens than <baseline language>\" claim — the measured direction is "
     "the opposite (toke costs 1.34x the cl100k_base tokens of equivalent Python, N = 60)"),
]
# a third party's own published figure is theirs to state; Rule 3 polices *our*
# withdrawn headlines, so an attributed number is exempt from it (Rules 1 and 2
# still apply).
ATTRIBUTED = re.compile(
    r"\b(KERN|KARN|NERD|Sigil|Vyxal|third[- ]party|they publish|their (own )?"
    r"(claim|figure|number|report))\b", re.I)

# Strikethrough (`~~"40-75% fewer tokens"~~`, 132.36). 132.47: in any of its spellings, markdown or rendered HTML. Used to
# REMOVE the struck text before the rest of the sentence is judged: a struck
# figure is withdrawn, the sentence around it is not.
STRUCK_SPAN = re.compile(r"~~[^~]+~~|<(del|s|strike)\b[^>]*>.*?</\1\s*>", re.I | re.S)

SUPERSEDED = re.compile(
    r"\b(supersede[sd]?|withdraw(n|s)?|no longer|historical|does not reconcile|"
    r"not reproducible|not creditable|not supportable|retired|corrected|formerly|"
    r"used to|previously|invites the misreading|against ourselves|lossy)\b", re.I)

WINDOW = 90  # chars either side of a percentage that count as "next to" it
# a comparator immediately before the number makes it a threshold, not a result
THRESHOLD_PREFIX = re.compile(
    r"(>=|≥|>|<=|≤|<|at least|no less than|minimum of|greater than|more than)\s*$",
    re.I)

SENTENCE_SPLIT = re.compile(
    r"(?<![Vv]s\.)(?<!e\.g\.)(?<!i\.e\.)(?<!etc\.)(?<!cf\.)(?<!approx\.)"
    r"(?<=[.!?])\s+(?=[A-Z(`*\"])")


# ---------------------------------------------------------------- Rule 4 ----
# Counts of things (story 132.14). The authority is the tree, read by
# scripts/verify_project_facts.py; docs/metrics-baseline.md § Project facts
# holds the same numbers and is gated by `verify_project_facts.py --check`.
#
# Each entry is (pattern, label, accepted-values-key). The pattern must capture
# the number in group "n". Patterns are deliberately narrow — they require the
# qualifying noun phrase ("conformance tests", not "tests") so that a tutorial
# saying "3 modules" or a benchmark saying "42 programs" does not fire.
import functools


@functools.lru_cache(maxsize=1)
def _facts():
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    try:
        import verify_project_facts as vpf
        return vpf.collect()
    except Exception as exc:                       # pragma: no cover
        print("WARN  rule 4 disabled: %s" % exc)
        return {}


def _accepted(facts):
    """noun-key -> the set of numbers that may legitimately appear."""
    if not facts:
        return {}
    return {
        "keywords": {facts["keywords"], facts["keywords_lexer_reserved"]},
        "productions": {facts["grammar_productions"]},
        "stdlib_modules": {facts["stdlib_modules"]},
        "conformance": {facts["conformance_cases_yaml"],
                        facts["conformance_cases_shell"],
                        facts["conformance_cases_total"]} |
                       set(facts.get("conformance_by_series", {}).values()),
        "diagnostic_codes": {facts["diagnostic_codes_documented"],
                             facts["diagnostic_codes_in_src"]},
        "epics": {facts["epics"]},
        "stories": {facts["stories"]},
        # 132.60: only LIVE corpus counts here. The historical ones are in
        # HISTORICAL below and pass only when their own block dates them.
        "corpus_records": {facts.get("corpus_records_v04_frozen"), 1583},
        "charset": {facts["charset_total"], 86},   # 86 = the legacy profile
    }


# Story 132.60. A fact sheet value that describes a PAST state is not a live
# count. `corpus_records_v02_2026_04` (46,754, the v0.2-era corpus of
# 2026-04-01) and the 25,953-program v0.3 tokenizer training set were in the
# accepted set above, so "This produced 46,754 validated programs"
# (docs/about/competitive-matrix.md), stated in the present tense as toke's
# corpus, passed rule 4 by matching a number that is only true of April.
# From a worktree, where the fact sheet cannot read ../toke-corpus, the same
# line passed instead because the 9-line `near` window happened to reach an
# LL(1)-retirement date in the next list item. A historical value is accepted
# only when its OWN block (paragraph, list item or table row) says it is
# historical.
def _historical(facts):
    return {
        "corpus_records": {facts.get("corpus_records_v02_2026_04"), 25953} - {None},
    }


HISTORICAL_CUE = re.compile(r"\bv0\.[0-3]\b|\bera\b|\bhistorical\b", re.I)


# "Phase 2 keywords", "Gate 1 stories", "Tier 0 modules" are labels, not counts
# (story 132.15 — `toke-corpus/scripts/phase2_syntax_audit.py` read as "2 keywords").
# 132.36: the separator may be a HYPHEN. `toke-ooke/docs/public-api.md` says
# "toke's Profile-1 character set excludes `_`" and this gate read it as a claim
# of a ONE-character alphabet — a label the list below already exempts, missed
# only because it was written `Profile-1` rather than `Profile 1`.
_LABEL_WORDS = ("Phase", "Profile", "Tier", "Gate", "Epic", "Wave")
NOT_LABEL = "".join("(?<!%s%s)" % (w, sep)          # re.I makes these case-blind
                    for w in _LABEL_WORDS for sep in (" ", "-"))

# A count scoped to a subset is not a claim about the project's total: "ooke depends on
# 11 stdlib modules", "completed 15 of 20 stories" (story 132.15).
SUBSET_PREFIX = re.compile(
    r"\b(depends? on|depend on|uses|using|imports?|requires?|only|just|of|out of|"
    r"remaining|missing|adds?|added|covers?|needs?|touch(es|ed)?)\s+(the\s+)?[*_`\s]*$", re.I)

# `epics`/`stories` are common words; only police them where the sentence is actually
# talking about this project's tracker.
TRACKER_CUE = re.compile(
    r"\b(progress\.md|backlog|tracker|roadmap|this project|the project|toke|epics and "
    r"stories|delivered|shipped|closed)\b", re.I)

COUNT_RULES = [
    (re.compile(r"(?<![\w.])" + NOT_LABEL + r"(?P<n>\d[\d,]*)\s*\+?\s*(?:reserved\s+)?keywords?\b", re.I),
     "keywords", "keywords"),
    # 132.14 found "keywords (13)" slipping past the rule above because the number
    # follows the noun; 132.15 found the same shape in a live generation prompt
    # (`toke-test-programs/infra/worker-generate.py`: "KEYWORDS (13)").
    (re.compile(r"\bkeywords?\s*\(\s*(?P<n>\d[\d,]*)\s*\)", re.I),
     "keywords", "keywords"),
    (re.compile(r"(?<![\w.])(?P<n>\d[\d,]*)\s*\+?\s*(?:EBNF\s+|grammar\s+)?productions?\b", re.I),
     "EBNF productions", "productions"),
    (re.compile(r"(?<![\w.])(?P<n>\d[\d,]*)\s*\+?\s*(?:stdlib|standard[- ]library)\s+modules?\b", re.I),
     "standard library modules", "stdlib_modules"),
    # 132.88: the parenthesised form, "The standard library (57 modules)" /
    # "stdlib (38 modules)", named the stdlib without putting it next to the
    # number. A bare "(12 modules)" is NOT matched: ooke and loke have modules too.
    # A dot inside a name (`std.*`) is not a sentence end, so it may be crossed.
    # `stdlib/` is a directory path (a docs tree, an old site section), not the library.
    (re.compile(r"\b(?:stdlib|standard[- ]library)\b(?!/)(?:[^.|()\n]|\.(?!\s)){0,24}\(\s*"
                r"(?:currently|now|today)?\s*(?P<n>\d[\d,]*)\s*\+?\s*modules?\b", re.I),
     "standard library modules", "stdlib_modules"),
    (re.compile(r"(?<![\w.])(?P<n>\d[\d,]*)\s*\+?\s*conformance\s+(?:tests?|cases?)\b", re.I),
     "conformance tests", "conformance"),
    # 132.88: "(currently 228 cases" -- a qualifier before the number, and a
    # parenthesis that does not close straight after the noun -- was unmatched.
    (re.compile(r"conformance\s+(?:suite|tests?|cases?)[^.|]{0,20}?\(\s*"
                r"(?:currently|now|today)?\s*(?P<n>\d[\d,]*)\s*\+?\s*"
                r"(?:(?:tests?|cases?)\b|\))", re.I),
     "conformance tests", "conformance"),
    (re.compile(r"(?<![\w.])(?P<n>\d[\d,]*)\s*\+?\s*(?:distinct\s+)?(?:diagnostic|error)\s+codes?\b", re.I),
     "diagnostic codes", "diagnostic_codes"),
    (re.compile(r"(?<![\w.])" + NOT_LABEL + r"(?P<n>\d[\d,]*)\s*\+?\s*epics\b", re.I), "epics", "epics"),
    (re.compile(r"(?<![\w.])" + NOT_LABEL + r"(?P<n>\d[\d,]*)\s*\+?\s*stories\b", re.I), "stories", "stories"),
    (re.compile(r"(?<![\w.])(?P<n>\d[\d,]*)\s*\+?\s*(?:corpus\s+records?|validated\s+(?:training\s+)?programs?|audited\s+records?)\b", re.I),
     "corpus records", "corpus_records"),
    # 132.61: "55-char syntax" (toke-website templates/ooke.tkt) passed both
    # guards because the noun was abbreviated. `char` counts as `character`.
    (re.compile(r"(?<![\w.])" + NOT_LABEL +
                r"(?P<n>\d[\d,]*)(?:[- ]characters?|-chars?)\b"
                r"(?=[^.|]{0,60}\b(?:alphabet|character set|charset|profile|syntax|"
                r"ASCII|default mode|set\b)|\s*(?:default|legacy)\b)", re.I),
     "character set", "charset"),
    (re.compile(r"(?<![\w.])" + NOT_LABEL +
                r"(?P<n>\d[\d,]*)\s*\+?\s*characters?\b"
                r"(?=[^.|]{0,60}\b(?:alphabet|character set|charset|profile|syntax|"
                r"ASCII)\b)", re.I),
     "character set", "charset"),
]

# A count attributed to another language ("Python has 35 keywords") is a
# comparison, not a claim about toke.
OTHER_LANG = re.compile(
    r"\b(Python|JavaScript|Java|Go|Rust|C\+\+|C#|TypeScript|Ruby|Swift|Kotlin|"
    r"Mojo|Kern|KERN)\b[^.|]{0,40}$", re.I)

# Rule 4 only: a stated intention to reach a number is a plan, not a count.
FUTURE_TARGET = re.compile(
    r"\b(expand|grow|scale|increase|extend|reach|raise|plan to|roadmap|"
    r"by 20[3-9]\d|will (be|have|reach))\b", re.I)

# A dated exemption marks a number as a deliberate historical snapshot.
FACTS_EXEMPT = re.compile(r"facts-exempt:\s*20\d\d-\d\d-\d\d", re.I)
# ...as does a whole-file archive banner (story 132.15). A document that opens with
# `**Archived YYYY-MM-DD ...**` in its first ARCHIVE_HEAD lines declares itself a dated
# record of what was true then — the sibling repos are full of them (toke-spec is an
# archive by charter). The banner must say so in the document, where a reader sees it;
# a skip list in a script in another repo would not. It exempts counts and withdrawn
# headlines, NOT rule 1 or rule 2: a live token-efficiency claim stays a live claim,
# and a lane crossing is a methodology error at any date.
ARCHIVE_BANNER = re.compile(r"\*\*Archived\s+20\d\d-\d\d-\d\d", re.I)
ARCHIVE_HEAD = 40
# ...as does an explicit era label next to the number.
DATED_SNAPSHOT = re.compile(
    r"\b(v0\.[123][- ]era|as of \d|snapshot|superseded|withdrawn|historical|"
    r"formerly|used to|no longer|retired|was measured|April 2026|March 2026|"
    r"20\d\d-\d\d-\d\d)\b", re.I)

# Rule 4 only polices live claim surfaces. Trackers, dated decision records,
# third-party reviews and the superseded v0.3 spec record what was believed at
# the time and are not claims being made today.
FACTS_SKIP = SKIP_PREFIXES + (
    "docs/audits/",
    "docs/security/audit-120/",
    "docs/spec/toke-spec-v0.3.md",
    "docs/about/changelog.md",
    "docs/whitepaper/",
)


PROSE_EXT = (".md", ".txt", ".html", ".htm", ".tkt")
TABLE_ROW = re.compile(r"^\s*(?:\||<t[rdh]\b)", re.I)


TABLE_CELL_SEP = re.compile(r"(?<!\\)\||</?t[dhr]\b[^>]*>", re.I)
# A cell that holds nothing but a date or an era label is the row's date
# column ("| 2026-05-25 | 57 stdlib modules |"): it dates every cell in its row.
DATE_CELL = re.compile(
    r"^[\s*_`]*(?:20\d\d-\d\d-\d\d|(?:January|February|March|April|May|June|July|"
    r"August|September|October|November|December)\s+20\d\d|v0\.[0-3](?:[- ]era)?|"
    r"20\d\d)[\s*_`]*$", re.I)


def is_table_row(line):
    """A markdown row, or a line holding HTML cells ("<table><tr><td>...")."""
    return bool(TABLE_ROW.match(line) or re.search(r"<t[dh]\b", line, re.I))


def cell_start(line, pos):
    """Offset where the table cell holding `pos` begins."""
    begin = 0
    for m in TABLE_CELL_SEP.finditer(line):
        if m.end() > pos:
            break
        begin = m.end()
    return begin


def table_dating_unit(line, start, end):
    """132.93: the text a date must sit in to date a count in a table row.

    132.88 took the whole row, so a date in any cell excused a count in every
    other: toke-spec prior-art.md:80 carried "57 stdlib modules" (Ecosystem
    column) as live, excused by "(N = 60, 2026-09-19)" in the Token Density
    cell, which dates a different measurement. The unit is now the count's
    own cell, plus any cell that is only a date (a date column dates its row).
    """
    cells, pos = [], 0
    for m in TABLE_CELL_SEP.finditer(line):
        cells.append((pos, m.start()))
        pos = m.end()
    cells.append((pos, len(line)))
    own = [line[a:b] for a, b in cells if a <= start and end <= b]
    dates = [line[a:b] for a, b in cells if DATE_CELL.match(line[a:b])]
    return " ".join(own + dates) if own else line


# 132.96: a cell that is nothing but a status -- "Superseded", "**Withdrawn
# 2026-09-18**", "historical", "superseded by 132.4" -- is the row's status
# column, as a date-only cell is its date column: it marks every cell in its
# row. "Nothing but": once the cue, dates, story numbers and markup are
# removed, at most one word is left ("v0.2 grammar notes" leaves two, so it is
# prose about something else, not a status).
STATUS_CELL_LEFTOVER_WORDS = 1


def _cells(line):
    """[(start, end)] of the table cells in `line`."""
    cells, pos = [], 0
    for m in TABLE_CELL_SEP.finditer(line):
        cells.append((pos, m.start()))
        pos = m.end()
    cells.append((pos, len(line)))
    return cells


def _status_cell(text, cue):
    t = re.sub(r"<[^>]*>|[*_`]", " ", text).strip()
    if not re.match(r"\W{0,3}" + cue.pattern, t, cue.flags):
        return False
    rest = re.sub(r"[^A-Za-z\s]", " ", cue.sub(" ", t))
    return len(rest.split()) <= STATUS_CELL_LEFTOVER_WORDS


def excusing_unit(line, start, end, near, cue):
    """132.96: the text a supersession or era cue must sit in to excuse THIS
    count.

    Rule 4 read SUPERSEDED and HISTORICAL_CUE over the whole line and over
    `near`, which for a table row is the row plus its header (and, for an HTML
    table written on one line, the whole table), so "withdrawn" or "previously"
    in ANY cell excused a count in every other. For a table row the unit is
    now, as 132.93 made it for a date: the count's own cell; any cell in the
    row that is only a status ("Superseded", "Withdrawn 2026-09-18"); the
    markdown header cell of the count's own column; and any line of `near` that is not
    a table row (a note attached below the table). Outside a table the unit is
    unchanged: the line and `near`.
    """
    if not is_table_row(line):
        return line + "\n" + near
    cells = _cells(line)
    col = next((k for k, (a, b) in enumerate(cells) if a <= start and end <= b),
               None)
    if col is None:
        return line + "\n" + near
    parts = [line[cells[col][0]:cells[col][1]]]
    parts += [line[a:b] for a, b in cells if _status_cell(line[a:b], cue)]
    for other in near.split("\n"):
        if other == line or not other.strip():
            continue
        if not is_table_row(other):
            parts.append(other)          # a note attached to the table
            continue
        ocells = _cells(other)
        # another line of the same HTML row: its status-only cells. (Not a
        # header: a "Superseded by" column header is not a status.)
        if re.search(r"<td\b", other, re.I):
            parts += [other[a:b] for a, b in ocells
                      if _status_cell(other[a:b], cue)]
        # a markdown header row: the header of the count's own column only
        elif other.lstrip().startswith("|") and line.lstrip().startswith("|") \
                and len(ocells) == len(cells):
            parts.append(other[ocells[col][0]:ocells[col][1]])
    return "\n".join(parts)


def rule3_unit(line, s, m, near):
    """132.98: the text a supersession cue must sit in to excuse a withdrawn
    headline restated in sentence `s` of `line` (`m` is the headline's match
    in `s`).

    Rule 3 searched SUPERSEDED over `near`, which for a table row is the row
    plus its header (and for a one-line HTML table, the whole table), so
    "withdrawn" or "previously" in any cell excused a withdrawn headline in
    every other -- the scope 132.96 removed from rule 4. A table row now uses
    rule 4's unit (`excusing_unit`): the headline's own cell, a status-only
    cell, its column's header, a note line below the table. Outside a table
    `near` keeps its meaning.
    """
    if not is_table_row(line):
        return near
    off = line.find(s)
    if off < 0:                 # not locatable: keep the pre-132.98 reading
        return near
    unit = excusing_unit(line, off + m.start(), off + m.end(), near, SUPERSEDED)
    # A cell in the column the table itself heads "Status" is the row's
    # status however many words it carries ("**Withdrawn, not requalified**"
    # in docs/whitepaper/toke-research-language.md §9 -- a corrections table
    # whose whole purpose is to withdraw the headline in its first column).
    if line.lstrip().startswith("|"):
        cells = _cells(line)
        for other in near.split("\n"):
            if other == line or not other.lstrip().startswith("|"):
                continue
            hcells = _cells(other)
            if len(hcells) != len(cells):
                continue
            for k, (a, b) in enumerate(hcells):
                if re.sub(r"[*_`\s]", "", other[a:b]).lower() == "status":
                    unit += "\n" + line[cells[k][0]:cells[k][1]]
    return unit


def dating_unit(line, start, end, block=None, idx=0):
    """132.88: the text a date must sit in to date THIS count.

    DATED_SNAPSHOT used to be searched over the whole structural block, so any
    date in a paragraph excused every count in it: toke-spec's prior-art.md
    carried "57 stdlib modules" as live, excused by an unrelated "(N = 60,
    2026-09-19)" earlier in the same blockquote. A date, era label or
    "superseded" dates the count only when it is in the count's own sentence
    -- or, for a table, its own row. Sentences may wrap across lines, so the
    sentence is found in the block (the line's structural unit), not the line.
    Only PROSE wraps: in JSON, code or config each line is its own unit (the
    caller passes no block), or `"analysis_date": "2026-05-25",` two lines up
    would read as the start of the count's sentence.
    """
    if is_table_row(line):
        return table_dating_unit(line, start, end)
    if block is None:
        block, idx = [line], 0
    before = " ".join(block[:idx])
    after = " ".join(block[idx + 1:])
    off = len(before) + (1 if before else 0)
    text = " ".join(x for x in (before, line, after) if x)
    s, e = off + start, off + end
    lo, hi = 0, len(text)
    for m in SENTENCE_SPLIT.finditer(text):
        if m.end() <= s:
            lo = m.end()
        elif m.start() >= e:
            hi = m.start()
            break
    return text[lo:hi]


def count_findings(line, near, block=None, idx=0):
    out = []
    accepted = _accepted(_facts())
    if not accepted:
        return out
    if FACTS_EXEMPT.search(near):
        return out
    for pat, label, key in COUNT_RULES:
        for m in pat.finditer(line):
            try:
                n = int(m.group("n").replace(",", ""))
            except ValueError:
                continue
            ok = accepted.get(key, set())
            if n in ok:
                continue
            # 132.60: a past value, dated as past in its own block
            if n in _historical(_facts()).get(key, set()) and \
                    HISTORICAL_CUE.search(excusing_unit(
                        line, m.start(), m.end(), near, HISTORICAL_CUE)):
                continue
            # a number given as an explicitly dated or superseded snapshot, a
            # stated target, or another language's figure is a record or a
            # comparison, not a claim about toke today. 132.88: the date must
            # sit in the count's own sentence or table row, not anywhere in
            # the block.
            if DATED_SNAPSHOT.search(dating_unit(line, m.start(), m.end(),
                                                 block, idx)):
                continue
            # 132.96: in a table, only the count's own cell, a status-only
            # cell, its column's header or a note below the table
            if SUPERSEDED.search(excusing_unit(line, m.start(), m.end(),
                                               near, SUPERSEDED)):
                continue
            # 132.93: in a table row a target is stated by the count's own
            # cell -- "Gate-1 tasks" in the Token Density cell of toke-spec
            # prior-art.md:80 read as UNMEASURED and excused "57 stdlib
            # modules" in the Ecosystem cell, alongside that cell's date.
            unit = table_dating_unit(line, m.start(), m.end()) \
                if is_table_row(line) else line
            if (UNMEASURED.search(unit) or FUTURE_TARGET.search(unit)) \
                    and not MEASURED.search(unit):
                continue
            # 132.93: another language named in the count's own cell, not
            # the cell before it (markdown's '|' already stopped the match;
            # an HTML cell boundary did not)
            if OTHER_LANG.search(line[cell_start(line, m.start())
                                      if is_table_row(line) else 0:m.start()]):
                continue
            if SUBSET_PREFIX.search(line[max(0, m.start() - 30):m.start()]):
                continue
            if key in ("epics", "stories") and not TRACKER_CUE.search(line) \
                    and not TRACKER_CUE.search(near):
                continue
            out.append((
                "%s: %d does not match the fact sheet (%s)" % (
                    label, n, ", ".join(str(v) for v in sorted(x for x in ok if x))),
                m.group(0).strip(), m.span()))
    return out


# ------------------------------------------------------ structural units ----
# Story 132.60. `near` (the window a supersession note or a date may sit in)
# was lines i-4..i+4, and the rule-1 N context was the 30 lines above. A line
# count knows nothing about the document: it reached across list items, table
# rows and paragraphs, so a live figure passed because the NEXT list item said
# "retired on 2026-07-02", and two withdrawal-table rows passed rule 1 because
# a sample size happened to sit 30 lines up. Both windows are now the
# STRUCTURAL unit the line belongs to: a paragraph, one list item, one table
# row, one heading -- or, in HTML, the block element it opens.
LIST_ITEM = re.compile(r"^\s*(?:[-*+]|\d+[.)])\s+")
HTML_BLOCK = re.compile(
    r"^\s*<(?:p|li|tr|h[1-6]|table|thead|tbody|ul|ol|div|blockquote|dt|dd|"
    r"section|article|pre)\b", re.I)
FENCE = re.compile(r"^\s*(```|~~~)")


def block_spans(lines, is_html=False):
    """[(start, end)] per line: the half-open span of its structural unit."""
    spans = [None] * len(lines)
    start = 0
    in_fence = False

    def close(end):
        for k in range(start, end):
            spans[k] = (start, end)

    for i, ln in enumerate(lines):
        if FENCE.match(ln):
            in_fence = not in_fence
        st = ln.strip()
        boundary = False
        own_line = False
        if not in_fence:
            if not st:
                boundary = True
            elif st.startswith("|") or st.startswith("#"):
                own_line = True
            elif LIST_ITEM.match(ln) or (is_html and HTML_BLOCK.match(ln)):
                boundary = True
        if own_line:
            close(i)
            spans[i] = (i, i + 1)
            start = i + 1
            continue
        if boundary:
            close(i)
            start = i if st else i + 1
            if not st:
                spans[i] = (i, i + 1)
    close(len(lines))
    return spans


TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{3,}")
# A table whose first header cell says its rows are claims being retired
# (docs/about/canonical.md "| Never | Write instead |") is a claims table in
# prose form: its rows name retired strings, they do not assert them.
RETIRING_HEADER = re.compile(
    r"^\s*\|\s*\**\s*(never|withdrawn|retired|stale|was|claim|do not|don't|"
    r"what we (no longer|stopped))\b", re.I)
NOTE_LEAD = re.compile(r"^\W{0,6}(superseded|withdrawn|retired|corrected|archived|"
                       r"historical|note|update[d]?)\b", re.I)


_TAG = re.compile(r"<[^>]+>")


def _governing_html(lines, spans, i):
    """governing() for a rendered page: the same three relations, by tag."""
    lo, hi = spans[i]
    parts, retiring = [], False
    st = lines[lo].strip().lower()
    if re.match(r"<t[rdh]\b", st):
        k = lo
        while k > 0 and "<table" not in lines[k].lower():
            k -= 1
        for j in range(k, lo):
            if re.search(r"<t[rh]\b", lines[j], re.I):
                head = _TAG.sub(" | ", lines[j]).strip()
                parts.append(head)
                retiring = bool(RETIRING_HEADER.match("| " + head.lstrip("| ")))
                break
    elif st.startswith("<li"):
        k = lo
        while k > 0 and not re.match(r"\s*<(ul|ol)\b", lines[k], re.I):
            k -= 1
        if k > 0:
            plo, _ = spans[k - 1]
            parts.append("\n".join(lines[plo:k]))
    j = hi
    while j < len(lines) and not _TAG.sub("", lines[j]).strip():
        j += 1
    if j < len(lines) and NOTE_LEAD.match(_TAG.sub("", lines[j]).strip()):
        nlo, nhi = spans[j]
        parts.append("\n".join(lines[nlo:nhi]))
    return "\n".join(parts), retiring


def governing(lines, spans, i, is_html=False):
    """Text that structurally governs line i's unit, beyond the unit itself.

    - a table row: the table's header row (the row above the |---| rule);
    - a list item: the lead-in paragraph directly above the list;
    - any unit: a note block directly after it that OPENS with a supersession
      or correction marker ("**Superseded (2026-09-18).** ...") -- the
      attached-note case the old +4-line window existed for.
    Returns (text, retiring_table).
    """
    if is_html:
        return _governing_html(lines, spans, i)
    lo, hi = spans[i]
    parts, retiring = [], False
    st = lines[i].strip()
    if st.startswith("|"):
        k = i
        while k > 0 and lines[k - 1].strip().startswith("|"):
            k -= 1
        if k + 1 < len(lines) and TABLE_SEP.match(lines[k + 1]):
            parts.append(lines[k])
            retiring = bool(RETIRING_HEADER.match(lines[k]))
        # 132.99: each markdown row is its own unit, so "the note after the
        # unit" was the next ROW for every row but the last, and a note below
        # the table excused only the last row. The note follows the table.
        while hi < len(lines) and lines[hi].strip().startswith("|"):
            hi += 1
    elif LIST_ITEM.match(lines[lo]):
        k = lo
        while k > 0:
            plo, _ = spans[k - 1]
            if not lines[k - 1].strip():
                k -= 1
                continue
            if LIST_ITEM.match(lines[plo]):
                k = plo
                continue
            parts.append("\n".join(lines[plo:k]))
            break
    j = hi
    while j < len(lines) and not lines[j].strip():
        j += 1
    if j < len(lines) and NOTE_LEAD.match(lines[j].strip()):
        nlo, nhi = spans[j]
        parts.append("\n".join(lines[nlo:nhi]))
    return "\n".join(parts), retiring


def sentences(line):
    """Split a line into claim-sized units. Table cells split on '|' too, and
    (132.60) on HTML cell boundaries, so a rendered table row is judged cell by
    cell exactly as its markdown source is."""
    for cell in re.split(r"\||</?t[dh]\b[^>]*>", line):
        for s in SENTENCE_SPLIT.split(cell):
            s = s.strip()
            if s:
                yield s


URL_TEXT = re.compile(r"\b(?:https?://|www\.)[^\s)\]>\"']+", re.I)


def claims_token_percentage(s):
    """True when the sentence states a measured percentage about toke's tokens.

    Exempt: third-party results that carry a citation, and stated
    targets/thresholds — TEMSpec §6.3 governs published *results*.
    """
    # 132.94: a URL is an address, not the sentence's subject. Pointing the
    # patterns catalogue's issue links at github.com/karwalski/toke made "toke"
    # the subject of "token-tied but 10.2% slower than c" (a speed figure).
    # The URL still counts as a CITATION below.
    bare = URL_TEXT.sub(" ", s)
    if not TOKE_SUBJECT.search(bare):
        return False
    if CITATION.search(s) and not re.search(r"\btoke\b", bare, re.I):
        return False
    if UNMEASURED.search(s) and not MEASURED.search(s):
        return False
    # 132.36: a figure the sentence has STRUCK THROUGH and calls withdrawn is a
    # withdrawal notice, not a published claim. `toke-website/design/
    # handoff-20260623/README.md` opens with a withdrawal banner naming the
    # stories, strikes both figures, and says no replacement belongs in a design
    # pack — and this gate still demanded a tokenizer and an N for the struck
    # text, i.e. it asked the document to requalify a number it had just
    # retired. Both halves are required: strikethrough AND a supersession word
    # in the same sentence. A live percentage stays a live claim.
    #
    # 132.47: that early return also excused every OTHER percentage in the
    # sentence -- "~~52%~~ (withdrawn); toke is 40% fewer tokens than Python"
    # passed whole. Now only the struck text is dropped (markdown ~~ or HTML
    # <del>/<s>), and what is left is judged like any other sentence.
    if STRUCK_SPAN.search(s) and SUPERSEDED.search(s):
        s = STRUCK_SPAN.sub(" ", s)
    for m in PCT.finditer(s):
        before = s[max(0, m.start() - 24):m.start()]
        if THRESHOLD_PREFIX.search(before):
            continue  # ">= 10%" is a gate threshold, not a published result
        lo, hi = max(0, m.start() - WINDOW), min(len(s), m.end() + WINDOW)
        w = s[lo:hi]
        if TOKEN_WORD.search(w) and COMPARATIVE.search(w):
            return True
    return False


# ---------------------------------------------------------------------------
# Generated mirrors (story 132.36)
# ---------------------------------------------------------------------------
# A directory that is a RENDERED OR COPIED mirror of docs this guard already
# scans at their source. Scanning the mirror reports the same page twice and —
# worse — the copy loses the FACTS_SKIP exemptions its source carries, so a
# changelog or the superseded v0.3 spec comes back as a live claim just because
# it was copied to a different path. Both mirrors below are written by a script
# and by nothing else (`scripts/sync_docs_content.sh`, `ooke build`), so there is
# nowhere in them to make a correction: the fix always belongs upstream.
#
# This is a path list, not a name list: `content/docs` cannot be excluded by the
# directory name `docs` without blinding the guard to every real doc tree.
# 132.61: recognised by STRUCTURE only -- the directory sits beside the script
# that writes it -- never by the checkout's name. The name-based list that used
# to come first ("toke-website/content/docs") also matched any unrelated tree
# that happened to be called that, and missed every worktree.
GENERATED_MIRRORS = ("build-docs", os.path.join("content", "docs"))


def is_generated_mirror(path):
    norm = os.path.normpath(os.path.abspath(path))
    for tail in GENERATED_MIRRORS:
        if norm.endswith(os.sep + tail):
            site = norm[:-len(os.sep + tail)]
            if os.path.isfile(os.path.join(site, "scripts", "build_static.sh")):
                return True
    return False


# ---------------------------------------------------------------------------
# Deployed trees (story 132.46)
# ---------------------------------------------------------------------------
# `build` is in EXCLUDE_DIRS by NAME, which is right for a Python wheel's
# build/ and wrong for exactly one directory: toke-website/build/ is what the
# deploy rsyncs to the server, i.e. every page a visitor gets. Until 132.46 the
# guard scanned the intermediate build-docs/ and pruned the deployed build/ by
# name; 132.36 then excluded build-docs/ as a mirror, which left the deployed
# tree scanned by nothing at all. It is scanned now.
#
# build/docs/<section>/<slug>/index.html is rendered from
# ${TOKE}/docs/<dir>/<slug>.md, and a rendered page must be judged by the
# policy of the source it came from: its archive banner is no longer in the
# first 40 lines once a nav header is wrapped round it, and SKIP_PREFIXES /
# FACTS_SKIP are keyed to source paths. Without this mapping the rendered copy
# of the superseded v0.3 spec is a live claim again (132.36's complaint). The
# section map is READ from the website's sync script, never retyped.
# The deployed tree is recognised by what makes it one -- a `build/` whose
# parent holds the script that produces it -- not by the checkout's directory
# name, so a worktree of the website is scanned the same as the main checkout.
DEPLOY_SCRIPT = os.path.join("scripts", "build_static.sh")


def is_deployed_tree(path):
    path = os.path.normpath(os.path.abspath(path))
    return (os.path.basename(path) == "build" and
            os.path.isfile(os.path.join(os.path.dirname(path), DEPLOY_SCRIPT)))


# 132.63: a deployed tree is only evidence about the sources it was built
# from. The main checkout's build/ was found dated 2026-09-22 and differing from
# a fresh build, so a scan of it described an old site -- green or red, the
# verdict was about the wrong thing. Freshness is decided by the site's OWN
# gate (scripts/check_build_fresh.py: build/ must be newer than static/,
# templates/, content/, sites/ and pages/), never re-implemented here. A stale
# tree is treated like a missing one: named, NOT scanned, and a failure under
# --strict. Rebuild it (`make build` in the site) and it is scanned again.
FRESH_SCRIPT = os.path.join("scripts", "check_build_fresh.py")
STALE_TREES = {}          # build path -> the freshness gate's own message


@functools.lru_cache(maxsize=None)
def deployed_tree_fresh(build_path):
    """(fresh?, message) for a deployed build/, from the site's own gate."""
    import subprocess
    site = os.path.dirname(os.path.normpath(os.path.abspath(build_path)))
    script = os.path.join(site, FRESH_SCRIPT)
    if not os.path.isfile(script):
        return True, "no freshness gate in this site; scanned as found"
    r = subprocess.run([sys.executable, script], capture_output=True, text=True)
    msg = (r.stdout + r.stderr).strip().splitlines()
    return r.returncode == 0, "; ".join(m.strip() for m in msg[:3])


def scan_deployed_tree(build_path):
    """True when the walk should descend into this deployed tree."""
    fresh, msg = deployed_tree_fresh(build_path)
    if not fresh:
        STALE_TREES[os.path.normpath(os.path.abspath(build_path))] = msg
    return fresh


def report_stale_trees(strict):
    """Print each stale deployed tree; return how many count as failures."""
    for path, msg in sorted(STALE_TREES.items()):
        print("SKIP  deployed tree is STALE, so not scanned: %s\n      %s\n"
              "      (run `make build` in that site; --strict fails on this)"
              % (os.path.relpath(path, ROOT), msg))
    return len(STALE_TREES) if strict else 0


@functools.lru_cache(maxsize=None)
def _docs_section_map(site_root):
    """{site section: toke docs dir} from scripts/sync_docs_content.sh."""
    sync = os.path.join(site_root, "scripts", "sync_docs_content.sh")
    try:
        text = open(sync, encoding="utf-8").read()
    except OSError:
        return {}
    m = re.search(r"^SECTIONS=\((.*?)^\)", text, re.M | re.S)
    if not m:
        return {}
    return dict(re.findall(r'"([\w-]+):([\w/-]+)"', m.group(1)))


@functools.lru_cache(maxsize=None)
def _slug_index(docs_dir):
    """{slug: [source .md]} under ROOT/docs/<docs_dir>, recursively: ooke
    flattens subdirectories, so docs/about/reviews/kern-2026-08.md is served
    at /docs/about/kern-2026-08/."""
    idx = {}
    base = os.path.join(ROOT, "docs", docs_dir)
    for dirpath, _dirs, files in os.walk(base):
        for f in files:
            if f.endswith(".md"):
                idx.setdefault(f[:-3], []).append(os.path.join(dirpath, f))
    return idx


def deployed_source(path):
    """The toke docs source of a rendered page in a deployed tree, or None."""
    norm = os.path.normpath(os.path.abspath(path))
    if not norm.endswith(os.sep + "index.html"):
        return None
    parts = norm.split(os.sep)
    for i in range(len(parts) - 1, 0, -1):
        if parts[i] == "build" and is_deployed_tree(os.sep.join(parts[:i + 1])):
            rest = parts[i + 1:-1]            # docs, <section>, <slug...>
            if len(rest) < 3 or rest[0] != "docs":
                return None
            docs_dir = _docs_section_map(os.sep.join(parts[:i])).get(rest[1])
            if not docs_dir:
                return None
            hits = _slug_index(docs_dir).get(rest[-1], [])
            return hits[0] if len(hits) == 1 else None
    return None


_NUM = re.compile(r"\d[\d,.]*%?")


def in_source(sent, src):
    """True when every number in a rendered finding also appears in its
    source file -- i.e. the claim is the source's, not the renderer's."""
    try:
        text = open(src, encoding="utf-8", errors="replace").read()
    except OSError:
        return False
    nums = _NUM.findall(re.sub(r"<[^>]+>", " ", html.unescape(sent)))
    return bool(nums) and all(n in text for n in nums)


def md_files(targets):
    out = []
    for t in targets:
        # 132.79: main() hands over absolute paths. A relative one here is
        # resolved against the cwd, never against ROOT: resolving `test` against
        # this repo scanned toke's test/ when the caller stood in toke-mcp.
        p = os.path.abspath(t)
        if os.path.isfile(p):
            out.append(p)
        else:
            for dirpath, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs
                           if (d not in EXCLUDE_DIRS
                               or (is_deployed_tree(os.path.join(dirpath, d))
                                   and scan_deployed_tree(os.path.join(dirpath, d))))
                           and not is_generated_mirror(os.path.join(dirpath, d))]
                for f in sorted(files):
                    if f.endswith(SCAN_EXT):
                        out.append(os.path.join(dirpath, f))
    return sorted(set(out))


# A claims table names the strings it is retiring. `docs/about/canonical.json`'s
# "forbidden" object has "LL(1)" and "13 keywords" as KEYS, each paired with an
# "instead" and a source; a withdrawn-claims table does the same. Those strings are the
# claims being retired, not claims being made — but the rest of the file is a live claim
# surface and must still be scanned, so this exempts the strings, not the file.
CLAIMS_TABLE = re.compile(r"forbidden|withdrawn|retired|deprecated|banned|do[ _-]?not",
                          re.I)
CLAIM_VALUE_KEY = re.compile(r"^(claim|claims|withdrawn|retired_claim|stale|was|not)$",
                             re.I)
# a claim record's own fields — never the claim
METADATA_KEY = re.compile(
    r"^(instead|replacement|fix|source|sources|story|stories|why|reason|note|notes|"
    r"date|retired_on|withdrawn_on|superseded_on)$", re.I)


@functools.lru_cache(maxsize=128)
def claims_table_lines(path):
    """1-based line numbers that fall inside a claims table in a JSON file.

    A claims table is an object under a key like "forbidden" / "withdrawn" /
    "retired_claim": its keys ARE the retired claims and its values quote them, so a
    string there is a claim being retired, not one being made. The region is located
    by brace matching, so the same string asserted anywhere else in the file is still
    checked — the exemption is the table, not the wording.
    """
    try:
        text = open(path, encoding="utf-8").read()
        import json
        json.loads(text)                      # only trust well-formed JSON
    except Exception:
        return frozenset()
    lines = set()
    for m in re.finditer(r'"([A-Za-z0-9_ -]+)"\s*:\s*[{\[]', text):
        if not CLAIMS_TABLE.search(m.group(1)):
            continue
        open_ch = text[m.end() - 1]
        close_ch = "}" if open_ch == "{" else "]"
        depth, i, in_str, esc = 0, m.end() - 1, False, False
        while i < len(text):
            c = text[i]
            if in_str:
                if esc:
                    esc = False
                elif c == "\\":
                    esc = True
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == open_ch:
                depth += 1
            elif c == close_ch:
                depth -= 1
                if depth == 0:
                    break
            i += 1
        first = text.count("\n", 0, m.start()) + 1
        last = text.count("\n", 0, min(i, len(text) - 1)) + 1
        lines.update(range(first, last + 1))
    return frozenset(lines)


def is_archived(lines):
    """True when the document opens with a dated archive banner (story 132.15)."""
    return bool(ARCHIVE_BANNER.search("\n".join(lines[:ARCHIVE_HEAD])))


def check_file(path):
    rel = os.path.relpath(path, ROOT)
    # 132.46: a rendered page in a deployed tree takes its source's policy
    src = deployed_source(path)
    if src:
        rel = os.path.relpath(src, ROOT)
    if rel.startswith(SKIP_PREFIXES):
        return []
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []
    # 132.46: judge HTML by the text a visitor reads. Rendered pages carry
    # `&gt;= 10%`, which THRESHOLD_PREFIX cannot see as the `>= 10%` it is.
    if path.endswith((".html", ".tkt")):   # 132.60: templates carry entities too
        lines = [html.unescape(ln) for ln in lines]
    archived = is_archived(lines)
    if src and not archived:
        try:
            archived = is_archived(open(src, encoding="utf-8",
                                        errors="replace").read().splitlines())
        except OSError:
            pass
    table_lines = claims_table_lines(path) if path.endswith(".json") else frozenset()
    is_markup = path.endswith((".html", ".htm", ".tkt"))
    prose = path.endswith(PROSE_EXT)       # 132.88: where a sentence may wrap
    spans = block_spans(lines, is_markup)
    findings = []
    for i, line in enumerate(lines):
        in_claims_table = (i + 1) in table_lines
        # 132.60: `near` -- the window a supersession note, a date or an era
        # marker may EXCUSE a figure from -- is the line's structural unit
        # (block_spans) plus what governs it (governing), not a line count.
        lo, hi = spans[i]
        # bounded, so a file with no blank lines (a JSON blob, a JS bundle) is
        # not one quadratic block
        lo, hi = max(lo, i - CONTEXT_LINES), min(hi, i + CONTEXT_LINES + 1)
        gov, retiring_table = governing(lines, spans, i, is_markup)
        near = "\n".join([gov, "\n".join(lines[lo:hi])])
        # the rule-1 N context keeps its documented scope (TEMSpec: "the
        # surrounding context block, up to CONTEXT_LINES above"): an N stated
        # once for a section ("Basis for every number below: N = 2,000")
        # governs the section. 132.60 is about `near`, which excuses.
        context = "\n".join(lines[max(0, i - CONTEXT_LINES):i + 1])
        for s in sentences(line):
            if in_claims_table:
                continue          # this line is inside the file's own claims table
            # Rule 1 — unqualified percentage claim about tokens
            if claims_token_percentage(s):
                missing = []
                if not TOKENIZERS.search(s):
                    missing.append("tokenizer name")
                if not (N_FIELD.search(s) or N_FIELD.search(context)):
                    missing.append("sample size (N)")
                if missing:
                    findings.append((i + 1, "missing " + " and ".join(missing), s))
            # Rule 3 — a withdrawn headline restated without its supersession
            if TOKE_SUBJECT.search(s) and TOKEN_WORD.search(s) and not archived \
                    and not retiring_table:
                for pat, label in WITHDRAWN:
                    if ATTRIBUTED.search(s):
                        break
                    m = pat.search(s)
                    if m and not (SUPERSEDED.search(s)
                                  or SUPERSEDED.search(rule3_unit(line, s, m, near))):
                        findings.append(
                            (i + 1, "restates %s with no supersession note" % label, s))
                        break
            # Rule 2 — toke-trained tokenizer used against a non-toke baseline
            if TOKE_TRAINED.search(s) and CROSS_COMPARE.search(s) \
                    and not PROHIBITION.search(s):
                findings.append(
                    (i + 1,
                     "lane crossing: a toke-trained tokenizer compared against a "
                     "non-toke baseline", s))
        # Rule 4 — a count of things that disagrees with the tree
        if not archived and not retiring_table and not rel.startswith(FACTS_SKIP):
            for why, frag, _span in count_findings(
                    line, near, lines[lo:hi] if prose else None, i - lo):
                if in_claims_table:
                    continue
                findings.append((i + 1, why, frag))
    return findings


# ------------------------------------------------------------- self-test ----
# Story 132.41/132.42. This guard had NO negative control. It was read as proof
# for months on the strength of an exit code, in a session where seven separate
# checks were found reporting success while testing nothing — including a doc
# gate iterating over a directory that does not exist. An exit code proves the
# process ran; only a deliberately broken input proves the gate can still fail.
#
# Each case is (accept?, name, text). `accept` cases are wordings that MUST pass
# — they are what stops the gate being tightened into noise nobody can satisfy.
# The rest are real defects this guard has actually shipped past or caught, one
# per rule, and they MUST keep failing.
SELFTEST = [
    # -- Rule 1: a token percentage needs its tokenizer and its N --------------
    (False, "rule 1: bare percentage about tokens",
     "toke is 52% fewer tokens than Python."),
    (False, "rule 1: tokenizer named, N missing",
     "Measured with the toke-16K tokenizer, toke is 31.1% denser than Python."),
    (True, "rule 1: fully qualified",
     "Tokenizer-vs-tokenizer on identical toke source: toke-16K vs cl100k, "
     "12.5% reduction, N = 500 programs."),
    (True, "rule 1: a threshold, not a result",
     "Gate 2 requires token reduction >= 15% on the held-out suite."),
    (True, "rule 1: a percentage that is not about tokens",
     "The conformance suite passes 100% of its cases."),

    # -- Rule 2: a toke-trained tokenizer against a non-toke baseline ----------
    (False, "rule 2: lane crossing",
     "toke-16K on toke source uses 48.2% fewer tokens than cl100k on Go."),

    # -- 131.50: the superword and byte-patch lanes are tokenizers too ---------
    (True, "131.50: superword lane, fully qualified",
     "Under superword8k, toke form b uses 12.0% fewer tokens than toke form a "
     "(same-tokenizer reduction, N = 40 functions)."),
    (False, "131.50: byte-patch lane with no N",
     "Under bytepatch the toke rewrite uses 9.5% fewer tokens."),
    (False, "131.50: superword8k is toke-trained -- never against Python",
     "superword8k on toke vs Python: 21.0% more tokens, N = 60 programs."),

    # -- Rule 3: a withdrawn headline restated with no supersession note -------
    (False, "rule 3: the withdrawn 52% headline, restated bare",
     "toke needs 52% fewer tokens than Python."),
    # NOTE the qualifiers: a supersession note excuses rule 3 and nothing else.
    # Written bare ("toke needs 52% fewer tokens than Python. (Withdrawn...)")
    # this case FAILS rule 1, which is correct and was pinned here by watching
    # it happen.
    (True, "rule 3: the same headline, marked superseded and still qualified",
     "toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens. "
     "(Withdrawn; superseded by the Gate 1 re-measurement.)"),

    # -- Rule 4: a count of things that disagrees with the tree ----------------
    (False, "rule 4: a stdlib module count that is not the tree's",
     "toke ships 30+ standard library modules."),

    # -- the exemptions, which must stay narrow -------------------------------
    # An archive banner exempts rules 3 and 4 (a dated record may restate what
    # was believed, and may carry a count from the day it was written). It does
    # NOT exempt rule 1: a percentage still has to say what measured it. Pinned
    # here because the two guards differ — check_canonical.py exempts an
    # archived document outright — and a difference nobody has written down is
    # a difference that gets "simplified" away.
    (True, "archived: rule 3 excused, rule 1 still satisfied",
     "**Archived 2026-09-15 — superseded by the Gate 1 re-measurement.**\n\n"
     "toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens."),
    (False, "archived does NOT excuse an unqualified percentage",
     "**Archived 2026-09-15 — superseded by the Gate 1 re-measurement.**\n\n"
     "toke needs 52% fewer tokens than Python."),

    # -- 132.60: `near` is the structural unit, not +/-4 lines ---------------
    # Each REJECT here passed the line-count window (verified against the
    # pre-132.60 guard) and each ACCEPT was a false positive of it or must
    # stay accepted.
    (False, "132.60: a date in the NEXT list item excuses nothing",
     "- Spec v0.4 was cut on 2026-07-02.\n- toke has 12 keywords."),
    (False, "132.60: a historical count stated as live (competitive-matrix.md:46)",
     "toke's corpus holds 46,754 validated programs."),
    (True, "132.60: the same historical count, dated in its own sentence",
     "The v0.2-era corpus held 46,754 validated programs."),
    (True, "132.60: a list quoted under a lead-in that retires it",
     "The live card contains, quoted here as published text that is being retired:\n\n"
     "- \"toke has a 55-character alphabet\" -- wrong."),
    (True, "132.60: a retiring table names what it retires (canonical.md \u00a711)",
     "| Never | Write instead |\n|---|---|\n| \"13 keywords\" | \"14 keywords\" |"),
    (False, "132.60: the same row in an ordinary table is a live claim",
     "| Fact | Value |\n|---|---|\n| Keywords | toke has 13 keywords |"),
    (True, "132.60: a rendered withdrawal-table row is judged cell by cell",
     "<table><tr><td>\"42% reduction vs Python\" (or 31% / 48%)</td>"
     "<td>withdrawn outright: toke costs about 1.34x Python's cl100k_base tokens</td></tr></table>",
     ".html"),

    # -- 132.61: an abbreviated count noun ------------------------------------
    (False, "132.61: \"55-char syntax\" (toke-website templates/ooke.tkt)",
     "toke's 55-char syntax means an LLM generates a complete site in fewer tokens.",
     ".tkt"),

    # -- 132.48: a label is not a count; a wrong count still fails ------------
    (True, "132.48: \"Profile-1 character set\" is a label (toke-ooke public-api.md:35)",
     "toke's Profile-1 character set excludes `_`, so this is a language constraint."),
    (False, "132.48: a genuinely wrong character-set count still fails",
     "toke uses a 55-character set."),

    # -- 132.47: struck-through withdrawn text --------------------------------
    (True, "132.47: a struck, withdrawn figure (design handoff README)",
     "- **Sounds like toke:** ~~\"40-75% fewer tokens than Python.\"~~ (withdrawn)"),
    (True, "132.47: the same, rendered as <del>",
     "<p><del>40-75% fewer tokens than Python</del> (withdrawn) -- toke keeps the register.</p>",
     ".html"),
    (False, "132.47: an UNSTRUCK stale claim still fails, withdrawn or not",
     "toke is 40-75% fewer tokens than Python (withdrawn)."),
    (False, "132.47: a live figure beside a struck one is not excused by it",
     "Once ~~52% fewer tokens~~ (withdrawn); today toke is 40% fewer tokens than Python."),

    # -- 132.88: a date excuses only the count in its own sentence or row -----
    (False, "132.88: an unrelated date earlier in the block (toke-spec prior-art.md:25-28)",
     "> toke costs **1.34x** the tokens of equivalent Python on the 60 Gate-1 tasks\n"
     "> (N = 60, 2026-09-19) — more, not fewer. Project-scale counts (59 characters, 14 keywords,\n"
     "> 57 stdlib modules) come from the same file's \"Project facts\" table."),
    (True, "132.88: the date in the count's own (wrapped) sentence",
     "On 2026-05-25, the date of this analysis, the tree held\n"
     "57 stdlib modules."),
    (False, "132.88: \"(57 modules)\" after the standard library (prior-art.md:193)",
     "- The standard library (57 modules) covers the core functionality."),
    (True, "132.88: the same parenthesised form, dated in its sentence",
     "- The v0.3-era standard library (14 modules) covered the Gate 1 tasks."),
    (True, "132.88: a bare \"(12 modules)\" is not the stdlib",
     "The ooke framework (12 modules) renders the site."),
    (False, "132.88: \"(currently 228 cases\" of the conformance suite",
     "The conformance suite (currently 228 cases, all passing) gates every change."),
    (False, "132.88: in JSON a date two lines up is another field, not the sentence",
     "{\n  \"analysis_date\": \"2026-05-25\",\n  \"story\": \"102.11\",\n"
     "  \"note\": \"The conformance suite (currently 228 cases) gates every program.\"\n}",
     ".json"),
    (True, "132.88: a table row dates its own count",
     "| Snapshot | Count |\n|---|---|\n| 2026-05-25 | 57 stdlib modules |"),

    # -- 132.93: a date excuses only the count in its own cell ---------------
    (False, "132.93: a date in another cell's prose (toke-spec prior-art.md:80)",
     "| Language | Token Density | Ecosystem |\n|---|---|---|\n"
     "| **toke** | Costs 1.34x Python's tokens (N = 60, 2026-09-19) | "
     "57 stdlib modules; mature |"),
    (False, "132.93: the same, rendered as HTML cells",
     "<table><tr><td>Costs 1.34x Python's tokens (N = 60, 2026-09-19)</td>"
     "<td>57 stdlib modules</td></tr></table>", ".html"),
    (False, "132.93: \"Gate-1\" in another cell is not a stated target for this one",
     "| Language | Token Density | Ecosystem |\n|---|---|---|\n"
     "| **toke** | Costs 1.34x on the Gate-1 tasks | 57 stdlib modules |"),
    (True, "132.94: \"toke\" inside a link's URL is not the sentence's subject",
     "- [127.10](https://github.com/karwalski/toke/blob/main/docs/progress.md): "
     "form a is token-tied but 10.2% slower than c."),
    (False, "132.94: the same sentence with toke as its subject still fails",
     "- [127.10](https://github.com/karwalski/toke/blob/main/docs/progress.md): "
     "toke is 10.2% fewer tokens than Python."),
    (True, "132.93: the date in the count's own cell",
     "| Language | Ecosystem |\n|---|---|\n"
     "| **toke** | 57 stdlib modules (2026-05-25) |"),
    (True, "132.93: a date-only cell is the row's date column",
     "<table><tr><td>2026-05-25</td><td>57 stdlib modules</td></tr></table>",
     ".html"),

    # -- 132.96: a supersession or era cue excuses only its own cell --------
    (False, "132.96: \"withdrawn\" in another cell's prose",
     "| Language | Token Density | Ecosystem |\n|---|---|---|\n"
     "| **toke** | the 52% headline was withdrawn | 57 stdlib modules |"),
    (False, "132.96: the same in a one-line HTML table (near = the whole table)",
     "<table><tr><td>the 52% figure was previously quoted</td>"
     "<td>57 stdlib modules</td></tr></table>", ".html"),
    (False, "132.96: another column's \"Superseded by\" header is not a status",
     "| Ecosystem | Superseded by |\n|---|---|\n| 57 stdlib modules | nothing yet |"),
    (False, "132.96: \"v0.2\" in another cell does not date a historical count",
     "| Spec | Corpus |\n|---|---|\n| v0.2 grammar notes | 46,754 validated programs |"),
    (True, "132.96: the cue in the count's own cell",
     "| Language | Ecosystem |\n|---|---|\n"
     "| **toke** | 57 stdlib modules (superseded) |"),
    (True, "132.96: a status-only cell is the row's status column",
     "<table><tr><td>57 stdlib modules</td><td><b>Withdrawn</b></td></tr></table>",
     ".html"),
    (True, "132.96: the header of the count's own column",
     "| Historical ecosystem | Now |\n|---|---|\n| 57 stdlib modules | see facts |"),
    (True, "132.96: an era cue in the historical count's own cell",
     "| Spec | Corpus |\n|---|---|\n| grammar | v0.2-era: 46,754 validated programs |"),
    (True, "132.99: a note below a markdown table excuses a count in a MIDDLE row",
     "| Year | Ecosystem |\n|---|---|\n| 2026-05 | 57 stdlib modules |\n| 2026-06 | ok |\n\n"
     "**Superseded.** The module count above is withdrawn; see the fact sheet."),
    (False, "132.99: a plain paragraph below the table is not a note",
     "| Year | Ecosystem |\n|---|---|\n| 2026-05 | 57 stdlib modules |\n| 2026-06 | ok |\n\n"
     "The count was superseded, we think."),

    # -- 132.98: rule 3 gets the same unit -- a supersession cue excuses a
    # withdrawn headline only from its own cell, a status cell, its column's
    # header. Every case carries a tokenizer and an
    # N, so rule 1 is satisfied and a rejection can only be rule 3.
    (False, "132.98: \"withdrawn\" in another cell's prose does not excuse the headline",
     "| Figure | Commentary |\n|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens | "
     "an unrelated benchmark was withdrawn last year |"),
    (False, "132.98: the same in a one-line HTML table",
     "<table><tr><td>toke-16K vs cl100k on identical source, N = 500: 52% fewer "
     "tokens</td><td>the old page previously linked a different chart</td></tr></table>",
     ".html"),
    (True, "132.98: a status-only cell excuses the row's headline",
     "| Figure | Status |\n|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens | "
     "**Withdrawn** |"),
    (True, "132.98: the header of the headline's own column",
     "| Superseded figure | Where |\n|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens | v0.3 site |"),
    (True, "132.98: the cell under the table's own Status column (whitepaper §9)",
     "| Published claim | Status | Correct statement |\n|---|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens | "
     "**Withdrawn, not requalified** | see below |"),
    (False, "132.98: the same words under a column that is not Status",
     "| Published claim | Commentary | Correct statement |\n|---|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens | "
     "withdrawn elsewhere, not here | see below |"),
    (True, "132.98: the cue in the headline's own cell",
     "| Figure | Where |\n|---|---|\n"
     "| toke-16K vs cl100k on identical source, N = 500: 52% fewer tokens "
     "(withdrawn) | v0.3 site |"),
]


def selftest_structure():
    """132.61: exclusions and pending entries follow what a tree IS, not what
    its checkout is called. Builds throwaway trees under neutral names."""
    import tempfile
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        # a worktree of toke-spec, checked out as "wt_x": its .git is a file
        wt = os.path.join(tmp, "wt_x")
        os.makedirs(os.path.join(wt, "rfc"))
        with open(os.path.join(wt, ".git"), "w") as fh:
            fh.write("gitdir: /elsewhere/toke-spec/.git/worktrees/wt_x\n")
        rfc = os.path.join(wt, "rfc", "draft.md")
        if repo_key(rfc) != "toke-spec/rfc/draft.md" or \
                not pending_owner(os.path.relpath(rfc, ROOT), rfc):
            bad += 1
            print("SELFTEST FAIL [structure] a toke-spec worktree's rfc/ lost its "
                  "pending owner (repo_key=%r)" % repo_key(rfc))
        # ...and a repo that is NOT toke-spec, with the same rfc/ path, does not get it
        other = os.path.join(tmp, "other")
        os.makedirs(os.path.join(other, ".git"))
        os.makedirs(os.path.join(other, "rfc"))
        orfc = os.path.join(other, "rfc", "draft.md")
        if pending_owner(os.path.relpath(orfc, ROOT), orfc):
            bad += 1
            print("SELFTEST FAIL [structure] an unrelated repo's rfc/ was exempted")
        # a website checkout under a neutral name: its mirrors are mirrors
        site = os.path.join(tmp, "site_wt")
        for d in ("scripts", "build-docs", os.path.join("content", "docs")):
            os.makedirs(os.path.join(site, d))
        open(os.path.join(site, "scripts", "build_static.sh"), "w").close()
        for d in ("build-docs", os.path.join("content", "docs")):
            if not is_generated_mirror(os.path.join(site, d)):
                bad += 1
                print("SELFTEST FAIL [structure] %s beside build_static.sh is not "
                      "recognised as a mirror" % d)
        # ...and a directory merely NAMED like one, with no build script, is scanned
        fake = os.path.join(tmp, "toke-website", "content", "docs")
        os.makedirs(fake)
        if is_generated_mirror(fake):
            bad += 1
            print("SELFTEST FAIL [structure] a tree named toke-website/content/docs "
                  "with no build script was excluded by name")
    # 132.108: --self keeps this repository's own docs in a sweep of siblings
    sib = os.path.join(os.path.dirname(ROOT), "toke-mcp")
    own_docs = os.path.join(ROOT, "docs")
    if own_docs not in resolve_targets([sib], with_self=True):
        bad += 1
        print("SELFTEST FAIL [structure] --self dropped this repo's docs/ from a "
              "sweep of sibling paths")
    if own_docs in resolve_targets([sib]):
        bad += 1
        print("SELFTEST FAIL [structure] an explicit path pulled in this repo's "
              "docs/ without --self")
    return bad


def selftest():
    import tempfile
    bad = selftest_structure()
    for case in SELFTEST:
        accept, name, text = case[:3]
        suffix = case[3] if len(case) > 3 else ".md"
        with tempfile.NamedTemporaryFile("w", suffix=suffix, encoding="utf-8",
                                         delete=False) as fh:
            fh.write(text + "\n")
            tmp = fh.name
        try:
            found = check_file(tmp)
        finally:
            os.unlink(tmp)
        ok = (not found) if accept else bool(found)
        if not ok:
            bad += 1
            print("SELFTEST FAIL [%s] %s: %s"
                  % ("accept" if accept else "reject", name,
                     ("flagged: %s" % found[0][1]) if found else "not flagged"))
    if bad:
        print("\nselftest: %d of %d cases wrong — the metrics-claim gate is not "
              "behaving as documented. Do NOT trust a green run from it."
              % (bad, len(SELFTEST)))
        return 1
    print("selftest: %d cases OK — %d wordings accepted, %d real defects still "
          "rejected (every rule, the archive exemption, and the 132.47/48/60/61 "
          "windows and nouns)."
          % (len(SELFTEST), sum(1 for c in SELFTEST if c[0]),
             sum(1 for c in SELFTEST if not c[0])))
    return 0


def resolve_targets(argv, with_self=False):
    """The paths to scan. 132.79: an argument is the caller's path, so it
    resolves against the cwd; only the DEFAULTS are this repository's own.

    132.108: with_self (`--self`) adds the defaults to explicit paths. Until
    then `make check-claims-all` passed only the siblings, whose mirror of
    toke's docs (content/docs) is skipped as scanned at its source -- so a
    wrong count in toke docs/ (ecosystem.md "68 modules" against a fact sheet
    of 67) passed that sweep, and its synced copy on the site passed with it."""
    own = [os.path.join(ROOT, t) for t in DEFAULT_TARGETS]
    if not argv:
        return own
    targets = [os.path.abspath(t) for t in argv]
    if with_self:
        targets += [t for t in own if t not in targets]
    return targets


def main():
    if "--selftest" in sys.argv:
        return selftest()
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    strict = "--strict" in sys.argv
    # 132.79: an argument is the caller's path, so it resolves against the cwd
    # (`test`, typed in toke-mcp, is toke-mcp's test/). Only the DEFAULTS are
    # this repository's own, so only they are joined to ROOT.
    targets = resolve_targets(argv, "--self" in sys.argv)
    for t in targets:
        if not os.path.exists(t):
            print("ERROR: no such path: %s" % t)
            return 2

    failures, warnings, scanned = [], [], 0
    # 132.46: build/ is gitignored, so a checkout that has not run `make build`
    # has no deployed tree to scan. Say so by name rather than pass silently.
    for t in targets:
        tp = t
        if os.path.isfile(os.path.join(tp, DEPLOY_SCRIPT)) and \
                not os.path.isdir(os.path.join(tp, "build")):
            print("SKIP  deployed tree not built, so not scanned: %s/build "
                  "(run `make build` there)" % os.path.relpath(tp, ROOT))
    for path in md_files(targets):
        rel = os.path.relpath(path, ROOT)
        if rel.startswith(SKIP_PREFIXES):
            continue
        scanned += 1
        # 132.46: a rendered docs page whose SOURCE passes is reported, not
        # asserted. Measured on the first deployed-tree scan: every such
        # finding is a window artefact -- the source is exempted by text within
        # 4 (or 30) lines, and rendering moves that text out of the window. At
        # least one is a real claim the SOURCE scan is wrongly exempting, so
        # these are printed by name, never dropped.
        src = deployed_source(path)
        divergent = bool(src) and not check_file(src)
        for lineno, why, sent in check_file(path):
            owner = pending_owner(rel, path)
            if not owner and src:     # a rendered page inherits its source's owner
                owner = pending_owner(os.path.relpath(src, ROOT), src)
            # a claim the renderer INTRODUCED (not in the source) still fails
            if divergent and not owner and in_source(sent, src):
                owner = ("132.46 render divergence: source %s passes"
                         % os.path.relpath(src, ROOT))
            item = (rel, lineno, why, sent, owner)
            if owner and not strict:
                warnings.append(item)
            else:
                failures.append(item)

    if "--list" in sys.argv:
        print(f"scanned {scanned} markdown files")
    stale_fail = report_stale_trees(strict)

    for rel, lineno, why, sent, owner in warnings:
        print(f"WARN  {rel}:{lineno}: {why}\n      {sent[:200]}")
        print(f"      (owned by story {owner})")

    if stale_fail:
        print("\nERROR (--strict): %d deployed tree(s) stale, so unscanned." % stale_fail)
        return 1
    if failures:
        print("\nERROR: token-efficiency claims without TEMSpec §6.3 qualifiers, "
              "crossing tokenizer lanes, or counts that disagree with the tree:\n")
        for rel, lineno, why, sent, _owner in failures:
            print(f"  {rel}:{lineno}: {why}")
            print(f"    {sent[:240]}")
        print("\nFix: copy the approved long or short form from "
              "docs/metrics-baseline.md ('Canonical wording for token-efficiency "
              "claims'). Every published number states metric type, tokenizer(s), "
              "baseline and N; a toke-trained tokenizer is never applied to a "
              "non-toke baseline. See story 132.6 / 132.13, TEMSpec §6.3.\n"
              "For a counts-of-things failure (rule 4): re-derive with "
              "`python3 scripts/verify_project_facts.py`, correct the number, or — "
              "if it is a deliberate historical snapshot — mark it "
              "`facts-exempt: YYYY-MM-DD <why>`. Story 132.14.")
        return 1

    print(f"metrics claims OK: {scanned} files scanned, "
          f"{len(warnings)} pending-story warning(s), "
          f"{len(STALE_TREES)} stale deployed tree(s) not scanned.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
