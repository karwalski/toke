#!/usr/bin/env python3
"""check_doc_site_links.py — no repo-relative `.md` links in docs the website publishes.

Story 132.111. toke-website copies the doc sections below into content/docs/
(scripts/sync_docs_content.sh there) and renders each file at
/docs/<section>/<slug>/. A link such as `[x](../reference/errors.md)` resolves
on GitHub but is dead on the site, so in those docs a cross-link must be either

  * a site path — `/docs/<section>/<slug>/` — when the target is published, or
  * a GitHub URL — `https://github.com/karwalski/toke/blob/main/<path>` —
    when it is not.

Scanned: every *.md under the synced sections, plus EXTRA_FILES (outward-facing
documents that are read away from the repository tree). Fenced code blocks and
inline code spans are ignored. Each finding names the replacement to use.

When the website checkout is present ($TOKE_WORKSPACE/toke-website, default the
parent of this repo), its SECTIONS table is read and compared with SITE_SECTIONS
below, so the two cannot drift silently. Without it (a CI runner) that
comparison is skipped and the scan still runs.

Usage:  python3 scripts/check_doc_site_links.py [--self-test] [paths...]
Exit:   0 clean, 1 findings (or section drift), 2 usage/self-test failure.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GITHUB = "https://github.com/karwalski/toke/blob/main/"

# <url section>: <dir under docs/> — mirrors SECTIONS in
# toke-website/scripts/sync_docs_content.sh (checked below when available).
SITE_SECTIONS = {
    "about": "about",
    "compiler": "compiler",
    "cookbook": "cookbook",
    "decisions": "decisions",
    "learn": "guide",
    "reference": "reference",
    "spec": "spec",
    "stdlib": "stdlib",
    "tutorials": "tutorials",
}

# Not synced to the site, but read outside the repo tree (132.111).
EXTRA_FILES = (
    "docs/whitepaper/toke-research-language.md",
    "docs/security/cve-process.md",
    "docs/consortium-proposal.md",
)

FENCE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE = re.compile(r"`+[^`]*`+")
INLINE_LINK = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?([^\s>]+)>?")
SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


def is_repo_relative_md(target: str) -> bool:
    if SCHEME.match(target) or target.startswith(("/", "#")):
        return False
    path = re.split(r"[#?]", target, maxsplit=1)[0]
    return path.lower().endswith(".md")


def links(text: str) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    in_fence = False
    for n, line in enumerate(text.splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        line = INLINE_CODE.sub("", line)
        for m in INLINE_LINK.finditer(line):
            out.append((n, m.group(1)))
        m = REF_DEF.match(line)
        if m:
            out.append((n, m.group(1)))
    return out


def slug_of(path: Path) -> str:
    try:
        head = path.read_text(encoding="utf-8").splitlines()[:30]
    except OSError:
        head = []
    if head and head[0].strip() == "---":
        for line in head[1:]:
            if line.strip() == "---":
                break
            m = re.match(r"slug:\s*['\"]?([^'\"\s]+)", line)
            if m:
                return m.group(1)
    return path.stem


def suggest(src: Path, target: str) -> str:
    cut = re.search(r"[#?]", target)
    path, tail = (target[:cut.start()], target[cut.start():]) if cut else (target, "")
    resolved = (src.parent / path).resolve()
    try:
        rel = resolved.relative_to(REPO)
    except ValueError:
        return "(target is outside the repository — link its GitHub URL)"
    if not resolved.is_file():
        return f"(target {rel} does not exist — fix or drop the link)"
    parts = rel.parts
    if len(parts) >= 3 and parts[0] == "docs":
        for url, d in SITE_SECTIONS.items():
            if parts[1] == d:
                return f"/docs/{url}/{slug_of(resolved)}/{tail}"
    return f"{GITHUB}{rel.as_posix()}{tail}"


def default_paths() -> list[Path]:
    paths: list[Path] = []
    for d in SITE_SECTIONS.values():
        paths.extend(sorted((REPO / "docs" / d).rglob("*.md")))
    paths.extend(REPO / f for f in EXTRA_FILES)
    return paths


def scan(paths: list[Path]) -> list[str]:
    findings = []
    for p in paths:
        if not p.is_file():
            findings.append(f"{p}: listed but missing")
            continue
        for n, target in links(p.read_text(encoding="utf-8")):
            if is_repo_relative_md(target):
                shown = p.relative_to(REPO) if p.is_relative_to(REPO) else p
                findings.append(f"{shown}:{n}: repo-relative link '{target}' — use {suggest(p, target)}")
    return findings


def section_drift() -> list[str]:
    ws = Path(os.environ.get("TOKE_WORKSPACE", str(REPO.parent)))
    sync = ws / "toke-website" / "scripts" / "sync_docs_content.sh"
    if not sync.is_file():
        print(f"note: {sync} not found — section table not cross-checked")
        return []
    m = re.search(r"^SECTIONS=\((.*?)^\)", sync.read_text(encoding="utf-8"), re.DOTALL | re.MULTILINE)
    if not m:
        return [f"{sync}: could not find its SECTIONS=( ... ) table"]
    theirs = dict(e.split(":", 1) for e in re.findall(r'"([^"]+)"', m.group(1)))
    if theirs != SITE_SECTIONS:
        return [f"SITE_SECTIONS differs from {sync}: website {theirs} vs here {SITE_SECTIONS} — update this script"]
    return []


def self_test() -> int:
    text = "\n".join([
        "[a](../reference/errors.md#e4075)",       # bad
        "[b](governance.md)",                       # bad
        "[c]: ./x.md",                              # bad (reference definition)
        "[d](/docs/stdlib/str/)",                   # ok: site path
        "[e](https://github.com/karwalski/toke/blob/main/docs/x.md)",  # ok: URL
        "[f](#local-anchor)",                       # ok
        "[g](../img/diagram.png)",                  # ok: not markdown
        "`[h](inline-code.md)`",                    # ok: code span
        "```",
        "[i](in-fence.md)",                         # ok: fenced
        "```",
    ])
    got = [t for _, t in links(text) if is_repo_relative_md(t)]
    want = ["../reference/errors.md#e4075", "governance.md", "./x.md"]
    if got != want:
        print(f"self-test FAILED: flagged {got}, expected {want}", file=sys.stderr)
        return 2
    s = suggest(REPO / "docs" / "stdlib" / "str.md", "../reference/errors.md#e4075")
    if s != "/docs/reference/errors/#e4075":
        print(f"self-test FAILED: suggestion {s!r}", file=sys.stderr)
        return 2
    print("self-test: 3/3 repo-relative links flagged, 8 legitimate forms passed, site-path suggestion ok")
    return 0


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--self-test":
        rc = self_test()
        if rc:
            return rc
        argv = argv[1:]
    paths = [Path(a).resolve() for a in argv] if argv else default_paths()
    problems = section_drift() + scan(paths)
    for f in problems:
        print(f, file=sys.stderr)
    if problems:
        print(f"check-doc-site-links: FAIL — {len(problems)} finding(s) in {len(paths)} file(s)", file=sys.stderr)
        return 1
    print(f"check-doc-site-links: OK — {len(paths)} file(s), no repo-relative .md links")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
