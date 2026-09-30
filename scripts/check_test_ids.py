#!/usr/bin/env python3
"""131.82 -- numbered test cases keep unique ids, and new ids come from a registry.

On one day three agents each created a conformance test numbered C009, because
each picked "the next free number" against the tree as it stood when it
started.  They were renumbered on merge, by hand.  This script is both halves
of the fix.

CHECK (default; `make check-test-ids`, in `make ci`)
  A numbered test case is test/<dir>/<S><NNN>.yaml or test/<dir>/<S><NNN>_<subject>.sh,
  where <S> is the series letter (L/G/N/T/C/D/P/... per AGENTS.md section 6).  The
  id is series-wide: test/lexical/L001.yaml and test/conform/L001_*.sh are the
  same id.  Fails on
    - DUP       one id used by two or more test files (unless GRANDFATHERED)
    - ID        a .yaml case whose `id:` field is missing or differs from its filename
    - SUBJECT   a .sh case without a `_<subject>` suffix -- the number is only a sort
                key; the subject is what names the test
    - STALE     a GRANDFATHERED entry that no longer describes a duplicate (the
                list only shrinks)
  `--root DIR` points the check at another tree (negative controls).

ALLOCATE (the registry)
  scripts/check_test_ids.py --next C             print the next free C id
  scripts/check_test_ids.py --claim C my_subject reserve it, print the file name
  The next id is one above the highest id of that series seen in ANY of:
    - this working tree,
    - every other worktree of this repository (uncommitted files included),
    - every local branch not merged into HEAD (files added there),
    - the claims registry: <git-common-dir>/test-id-claims.jsonl, shared by every
      worktree of the repo, appended under an exclusive lock.
  So two agents working in parallel on this machine cannot be handed the same
  number, whether the other agent has committed, only created the file, or only
  claimed it.  Agents on another machine are caught by CHECK at merge.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import json
import os
import pathlib
import re
import subprocess
import sys

CASE = re.compile(r'^(?P<series>[A-Z])(?P<num>\d{3,4})(?:_(?P<subject>[a-z0-9_]+))?\.(?P<ext>yaml|sh)$')
YAML_ID = re.compile(r'^id:\s*"?([A-Za-z0-9_]+)"?\s*$', re.MULTILINE)

# Ids already shared by two files when this check landed (2026-10-01), named
# file by file so a NEW duplicate of the same id still fails.  Renumbering one
# side is a separate change (it moves a test's identity); until then these are
# known, not silent.  An entry that stops matching the tree fails as STALE.
GRANDFATHERED = {
    'L001': {'test/lexical/L001.yaml', 'test/conform/L001_keyword_ident_boundary.sh'},
    'T001': {'test/types/T001.yaml', 'test/conform/T001_tki_field_check.sh'},
    'T002': {'test/types/T002.yaml', 'test/conform/T002_tki_call_check.sh'},
    'T003': {'test/types/T003.yaml', 'test/conform/T003_struct_layout.sh'},
    'T004': {'test/types/T004.yaml', 'test/conform/T004_stdlib_link_set.sh'},
    'T005': {'test/types/T005.yaml', 'test/conform/T005_stdlib_arity_zero_and_no_tki.sh'},
    'T006': {'test/types/T006.yaml', 'test/conform/T006_builtin_table_matches_the_c.sh'},
    'T007': {'test/types/T007.yaml', 'test/conform/T007_every_module_declares_its_surface.sh'},
}


def git(root, *args):
    r = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True, check=False)
    return r.stdout if r.returncode == 0 else ''


def case_files(root):
    """Repo-relative paths of every numbered test case directly under test/<dir>/."""
    out = []
    test = root / 'test'
    if not test.is_dir():
        return out
    for d in sorted(p for p in test.iterdir() if p.is_dir()):
        for f in sorted(d.iterdir()):
            if f.is_file() and CASE.match(f.name):
                out.append(f'test/{d.name}/{f.name}')
    return out


def case_id(relpath):
    m = CASE.match(pathlib.PurePosixPath(relpath).name)
    return (f"{m['series']}{m['num']}", m) if m else (None, None)


def check(root):
    files = case_files(root)
    by_id = {}
    problems = []
    for rel in files:
        cid, m = case_id(rel)
        by_id.setdefault(cid, set()).add(rel)
        if m['ext'] == 'yaml':
            found = YAML_ID.search((root / rel).read_text(errors='replace'))
            if not found:
                problems.append(('ID', rel, 'no `id:` field'))
            elif found.group(1) != cid:
                problems.append(('ID', rel, f'`id: {found.group(1)}` but the file is {cid}'))
        elif not m['subject']:
            problems.append(('SUBJECT', rel, f'name the test by its subject: {cid}_<subject>.sh'))
    for cid, paths in sorted(by_id.items()):
        if len(paths) > 1 and GRANDFATHERED.get(cid) != paths:
            problems.append(('DUP', cid, ', '.join(sorted(paths))
                             + ' -- allocate a fresh id: scripts/check_test_ids.py --claim '
                             + cid[0] + ' <subject>'))
    for cid, paths in sorted(GRANDFATHERED.items()):
        if by_id.get(cid) != paths:
            problems.append(('STALE', cid, 'grandfathered duplicate no longer matches the tree: delete the entry'))
    return files, by_id, problems


# ── registry ────────────────────────────────────────────────────────────────
def claims_path(root):
    common = git(root, 'rev-parse', '--git-common-dir').strip()
    if not common:
        return None
    p = pathlib.Path(common)
    return (p if p.is_absolute() else (root / p)).resolve() / 'test-id-claims.jsonl'


def ids_in_use(root, series, claims_text=''):
    """Every id number of `series` visible from this repo, with where it was seen."""
    seen = {}

    def add(rel, where):
        _, m = case_id(rel)
        if m and m['series'] == series:
            seen.setdefault(int(m['num']), set()).add(where)

    for rel in case_files(root):
        add(rel, 'tree')
    for block in git(root, 'worktree', 'list', '--porcelain').split('\n\n'):
        line = block.split('\n', 1)[0]
        if line.startswith('worktree '):
            wt = pathlib.Path(line[len('worktree '):])
            if wt.resolve() != root.resolve():
                for rel in case_files(wt):
                    add(rel, f'worktree {wt}')
    for rel in git(root, 'log', '--branches', '--not', 'HEAD', '--diff-filter=A',
                   '--name-only', '--format=', '--', 'test').split():
        if rel.count('/') == 2:
            add(rel, 'unmerged branch')
    for line in claims_text.splitlines():
        try:
            c = json.loads(line)
        except ValueError:
            continue
        if c.get('series') == series:
            seen.setdefault(int(c['num']), set()).add(f"claim {c.get('subject')}")
    return seen


def next_id(root, series, claims_text=''):
    seen = ids_in_use(root, series, claims_text)
    return (max(seen) + 1) if seen else 1


def claim(root, series, subject):
    reg = claims_path(root)
    if reg is None:
        sys.exit('check-test-ids: --claim needs a git repository (the registry lives in its common dir)')
    with open(reg, 'a+', encoding='utf-8') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.seek(0)
        n = next_id(root, series, f.read())
        branch = git(root, 'rev-parse', '--abbrev-ref', 'HEAD').strip()
        f.write(json.dumps({'series': series, 'num': n, 'subject': subject, 'branch': branch,
                            'pid': os.getpid(),
                            'at': dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')}) + '\n')
        f.flush()
        fcntl.flock(f, fcntl.LOCK_UN)
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description='131.82: unique test ids + an id registry')
    ap.add_argument('--root', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--next', metavar='SERIES', help='print the next free id of SERIES')
    g.add_argument('--claim', nargs=2, metavar=('SERIES', 'SUBJECT'),
                   help='reserve the next id of SERIES in the registry and print the file name')
    a = ap.parse_args(argv)
    root = a.root.resolve()
    series = (a.next or (a.claim[0] if a.claim else '')).upper()
    if (a.next or a.claim) and not re.fullmatch(r'[A-Z]', series):
        ap.error('SERIES is one capital letter, e.g. C')
    if a.next:
        reg = claims_path(root)
        text = reg.read_text(encoding='utf-8') if reg and reg.exists() else ''
        print(f'{series}{next_id(root, series, text):03d}')
        return 0
    if a.claim:
        subject = a.claim[1]
        if not re.fullmatch(r'[a-z0-9_]+', subject):
            ap.error('SUBJECT is lowercase words joined by _, e.g. map_value_semantics')
        n = claim(root, series, subject)
        print(f'{series}{n:03d}_{subject}.sh')
        return 0

    files, by_id, problems = check(root)
    dups = sum(1 for _, p in by_id.items() if len(p) > 1)
    print(f'check-test-ids: {len(files)} numbered test cases, {len(by_id)} ids, '
          f'{dups} shared ({len(GRANDFATHERED)} grandfathered), {len(problems)} problem(s)')
    for kind, what, why in problems:
        print(f'  {kind:<7} {what}  -- {why}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
