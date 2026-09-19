#!/usr/bin/env python3
"""
github_repo_descriptions.py — print the `gh repo edit` commands for story 132.4.

A GitHub repository description is a web field: it is not a file in any repo, so
no release carries it and no CI job can fix it. Story 132.4 prepares the text;
the owner applies it. This script is the bridge — it **prints** one
`gh repo edit` line per repository and never calls the API itself, so a review
step always sits between the prepared text and the live account.

    python3 scripts/about/github_repo_descriptions.py           # review
    python3 scripts/about/github_repo_descriptions.py | sh      # apply (owner)
    python3 scripts/about/github_repo_descriptions.py --check   # compare with live

Every description below is `<what this repo is>` + the canonical one-liner,
reproduced word for word from docs/about/canonical.json, + the link home. The
prose rationale, and the live text each one replaces, are in
docs/about/registry-descriptions.md.
"""
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CANON = os.path.join(ROOT, "docs", "about", "canonical.json")
HOME = "https://tokelang.dev"
GH_LIMIT = 350                      # GitHub's description limit

with open(CANON, encoding="utf-8") as fh:
    ONE_LINER = json.load(fh)["blocks"]["one_liner"]["value"]

BACK = "github.com/karwalski/toke"

# repo -> (what it is, topics). The one-liner is appended verbatim; `BACK` is
# appended to every repo but the main one, whose own URL that is.
REPOS = [
    ("toke",
     "Reference compiler (tkc), language specification and standard library. Apache-2.0.",
     ["programming-language", "llm", "code-generation", "token-efficiency", "compiler"],
     False),
    ("toke-spec",
     "The toke language specification: the normative v0.4 text, the EBNF and GBNF "
     "grammars, and the RFC draft.",
     ["toke", "programming-language", "compiler", "specification"], True),
    ("toke-corpus",
     "Training corpus for toke: generation, audit and execution-verification pipeline.",
     ["toke", "llm", "code-generation", "dataset"], True),
    ("toke-eval",
     "toke-eval: the held-out benchmark and evaluation harness for toke.",
     ["toke", "llm", "code-generation", "benchmark"], True),
    ("toke-mcp",
     "MCP server, language server and VS Code extension for toke.",
     ["toke", "llm", "code-generation", "mcp", "language-server"], True),
    ("toke-models",
     "Fine-tuning, evaluation and packaging for toke code-generation models. The "
     "published model is the Gate 2 research artefact and writes v0.3 syntax.",
     ["toke", "llm", "code-generation", "qlora"], True),
    ("toke-tokenizer",
     "The v0.3 BPE tokenizer for toke (16,384 vocab) and its training/evaluation "
     "pipeline; the v0.4 retrain has not shipped.",
     ["toke", "tokenizer", "token-efficiency", "bpe"], True),
    ("toke-test-programs",
     "Executable toke programs used as regression and conformance material for the "
     "compiler.",
     ["toke", "programming-language", "compiler", "testing"], True),
    ("ooke",
     "ooke, toke's web framework and static site generator, written in toke; it builds "
     "and serves tokelang.dev.",
     ["toke", "web-framework", "static-site-generator"], True),
    ("loke",
     "loke, toke's local intelligence layer, written in toke.",
     ["toke", "llm"], True),
    ("toke-web",
     "Source for tokelang.dev, built and served by ooke.",
     ["toke", "website"], True),
    ("toke-benchmark",
     "Archived (superseded by karwalski/toke-eval). Early benchmark material for toke, "
     "kept for provenance.",
     [], False),
    ("toke-stdlib",
     "Archived (the standard library now lives in karwalski/toke). Kept for provenance.",
     [], False),
    ("toke-cloud",
     "Private: billing, authentication and deployment infrastructure for toke services.",
     [], False),
    ("toke-console",
     "Private: the toke console (accounts, billing, API keys).",
     [], False),
]

# Repos whose purpose the owner has not settled; story 132.4 will not invent one.
UNDECIDED = {
    "tkc": "public, undescribed and absent from docs/about/repos.md — describe it or "
           "archive it (owner decision, story 132.4)",
}


def description(what, back):
    text = "%s %s" % (what, ONE_LINER)
    if back:
        text += " " + BACK
    return text


def command(repo, what, topics, back):
    desc = description(what, back)
    parts = ["gh", "repo", "edit", "karwalski/%s" % repo,
             "--description", desc]
    if repo not in ("toke-cloud", "toke-console"):
        parts += ["--homepage", HOME]
    for t in topics:
        parts += ["--add-topic", t]
    return " ".join(shlex.quote(p) for p in parts), desc


def live_descriptions():
    """Read the live descriptions (read-only) so --check can diff against them."""
    try:
        out = subprocess.run(["gh", "api", "user/repos", "--paginate",
                              "-q", ".[] | [.name, .description] | @tsv"],
                             capture_output=True, text=True, timeout=60, check=True)
    except (OSError, subprocess.SubprocessError) as exc:
        print("could not read live descriptions: %s" % exc, file=sys.stderr)
        return {}
    live = {}
    for row in out.stdout.splitlines():
        name, _, desc = row.partition("\t")
        live[name] = desc
    return live


def main():
    check = "--check" in sys.argv
    live = live_descriptions() if check else {}
    bad = 0
    for repo, what, topics, back in REPOS:
        cmd, desc = command(repo, what, topics, back)
        if len(desc) > GH_LIMIT:
            print("# TOO LONG (%d > %d): %s" % (len(desc), GH_LIMIT, repo))
            bad += 1
            continue
        if check:
            current = live.get(repo)
            state = ("MISSING" if current in (None, "", "null")
                     else ("OK" if current == desc else "DIFFERS"))
            print("%-9s %-20s %s" % (state, repo, current or ""))
            if state != "OK":
                bad += 1
        else:
            print("%s   # %d chars" % (cmd, len(desc)))
    for repo, why in UNDECIDED.items():
        print("# karwalski/%s: %s" % (repo, why))
    return 1 if (check and bad) else 0


if __name__ == "__main__":
    sys.exit(main())
