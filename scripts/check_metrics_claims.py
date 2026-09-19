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

Rule 2 — LANE CROSSING (story 132.13). A toke-trained tokenizer (Toke-16K,
tokenizer_v03, proxy8k, the toke SentencePiece models) must never be named in a
comparison alongside a non-toke baseline language. Sentences that *forbid* the
practice are recognised and allowed.

Usage:  python3 scripts/check_metrics_claims.py [paths...] [--strict] [--list]
Exit:   0 clean (pending-story files warn only), 1 on a violation.
        --strict also fails on the pending-story files.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTEXT_LINES = 30

# Default scan set: the published doc tree plus the repo's front-page files.
DEFAULT_TARGETS = ["docs", "README.md", "PROJECT_STATUS.md"]

# Files owned by another story: reported as warnings, never fail the build,
# until that story lands and the entry is deleted. Keep this list SHORT and
# always keyed to a story number.
PENDING = {
    "docs/whitepaper/toke-research-language.md": "132.8 — whitepaper v2 + RFC alignment",
    "docs/about/positioning-2026-09.md": "132.7 — repositioning brief (in flight)",
}

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
    r"byte-level BPE|8K? ?(vocab|BPE|SentencePiece)|16K? ?(vocab|BPE))\b", re.I)

# Sample size: an explicit N, or a count followed by a unit noun.
N_FIELD = re.compile(
    r"(\bN\s?=\s?[\d,]+)|"
    r"\b[\d,]{1,9}\s+(?:[a-z-]+\s+){0,3}(benchmark(s| programs)?|programs?|tasks?|"
    r"records?|pairs?|examples?|samples?|solutions?|files?|modules?)\b", re.I)

# Rule 2: tokenizers trained on toke text — valid on the toke side only.
TOKE_TRAINED = re.compile(
    r"\b(toke-?16k|toke-?bpe|tokenizer_v03|proxy8k|toke's own (BPE|tokenizer)|"
    r"toke BPE|purpose-built (16K )?(BPE|tokenizer)|toke tokenizer)\b", re.I)
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
]
# a third party's own published figure is theirs to state; Rule 3 polices *our*
# withdrawn headlines, so an attributed number is exempt from it (Rules 1 and 2
# still apply).
ATTRIBUTED = re.compile(
    r"\b(KERN|KARN|NERD|Sigil|Vyxal|third[- ]party|they publish|their (own )?"
    r"(claim|figure|number|report))\b", re.I)

SUPERSEDED = re.compile(
    r"\b(superseded|withdraw(n|s)?|no longer|historical|does not reconcile|"
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


def sentences(line):
    """Split a line into claim-sized units. Table cells split on '|' too."""
    for cell in line.split("|"):
        for s in SENTENCE_SPLIT.split(cell):
            s = s.strip()
            if s:
                yield s


def claims_token_percentage(s):
    """True when the sentence states a measured percentage about toke's tokens.

    Exempt: third-party results that carry a citation, and stated
    targets/thresholds — TEMSpec §6.3 governs published *results*.
    """
    if not TOKE_SUBJECT.search(s):
        return False
    if CITATION.search(s) and not re.search(r"\btoke\b", s, re.I):
        return False
    if UNMEASURED.search(s) and not MEASURED.search(s):
        return False
    for m in PCT.finditer(s):
        before = s[max(0, m.start() - 24):m.start()]
        if THRESHOLD_PREFIX.search(before):
            continue  # ">= 10%" is a gate threshold, not a published result
        lo, hi = max(0, m.start() - WINDOW), min(len(s), m.end() + WINDOW)
        w = s[lo:hi]
        if TOKEN_WORD.search(w) and COMPARATIVE.search(w):
            return True
    return False


def md_files(targets):
    out = []
    for t in targets:
        # accept a path relative to the repo root, or any path that exists as
        # given (so sibling repos can be scanned: `... ../toke-tokenizer`)
        p = t if os.path.exists(t) and os.path.isabs(t) else os.path.join(ROOT, t)
        if not os.path.exists(p) and os.path.exists(t):
            p = os.path.abspath(t)
        if os.path.isfile(p):
            out.append(p)
        else:
            for dirpath, _dirs, files in os.walk(p):
                for f in sorted(files):
                    if f.endswith(".md"):
                        out.append(os.path.join(dirpath, f))
    return sorted(set(out))


def check_file(path):
    rel = os.path.relpath(path, ROOT)
    if rel.startswith(SKIP_PREFIXES):
        return []
    lines = open(path, encoding="utf-8").read().splitlines()
    findings = []
    for i, line in enumerate(lines):
        ctx_start = max(0, i - CONTEXT_LINES)
        context = "\n".join(lines[ctx_start:i + 1])
        # a supersession note may sit just after the sentence it qualifies, but it
        # has to be adjacent to count — a marker 20 lines away qualifies nothing.
        near = "\n".join(lines[max(0, i - 4):i + 5])
        for s in sentences(line):
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
            if TOKE_SUBJECT.search(s) and TOKEN_WORD.search(s):
                for pat, label in WITHDRAWN:
                    if ATTRIBUTED.search(s):
                        break
                    if pat.search(s) and not (SUPERSEDED.search(s)
                                              or SUPERSEDED.search(near)):
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
    return findings


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    strict = "--strict" in sys.argv
    targets = argv or DEFAULT_TARGETS

    failures, warnings, scanned = [], [], 0
    for path in md_files(targets):
        rel = os.path.relpath(path, ROOT)
        if rel.startswith(SKIP_PREFIXES):
            continue
        scanned += 1
        for lineno, why, sent in check_file(path):
            item = (rel, lineno, why, sent)
            if rel in PENDING and not strict:
                warnings.append(item)
            else:
                failures.append(item)

    if "--list" in sys.argv:
        print(f"scanned {scanned} markdown files")

    for rel, lineno, why, sent in warnings:
        print(f"WARN  {rel}:{lineno}: {why}\n      {sent[:200]}")
        print(f"      (owned by story {PENDING[rel]})")

    if failures:
        print("\nERROR: token-efficiency claims without TEMSpec §6.3 qualifiers, "
              "or crossing tokenizer lanes:\n")
        for rel, lineno, why, sent in failures:
            print(f"  {rel}:{lineno}: {why}")
            print(f"    {sent[:240]}")
        print("\nFix: copy the approved long or short form from "
              "docs/metrics-baseline.md ('Canonical wording for token-efficiency "
              "claims'). Every published number states metric type, tokenizer(s), "
              "baseline and N; a toke-trained tokenizer is never applied to a "
              "non-toke baseline. See story 132.6 / 132.13, TEMSpec §6.3.")
        return 1

    print(f"metrics claims OK: {scanned} files scanned, "
          f"{len(warnings)} pending-story warning(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
