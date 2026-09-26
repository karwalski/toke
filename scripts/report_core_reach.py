#!/usr/bin/env python3
"""136.50a — finished C capabilities that no interface mentions at all.

WHY THIS EXISTS.  Three gates already look at this area and none of them can see
this class:

  * `check-tki` checks that every `.tki` export resolves to a defined symbol.
    It is honest and its quarantine tracks the known backlog -- canvas alone is
    13 of them.  But it starts from the .tki, so a capability the .tki never
    mentions is invisible to it by construction.
  * `check-glue-core` (136.50) catches a `*_glue.c` that never includes its own
    core's header.  It passes a glue that includes the header and wraps a third
    of it.
  * 136.44's stub sweep compared declared exports against the glue.  136.44a --
    `tls_gen_self_signed_alg`, complete and linked since July, ML-DSA-65
    certificate generation unreachable from toke -- was declared on NEITHER
    side, so there was nothing to disagree with.

So the detector runs the other way round: enumerate each module's own public
header and subtract everything the C stdlib actually calls and everything the
module's `.tki` exports.  What remains is finished work with no way in.

WHAT IS ASSERTED AND WHAT IS ONLY REPORTED.  The raw output mixes things a
regex cannot tell apart, so every finding carries a hand-made classification in
scripts/core_reach_triage.txt:

  gap       a real capability with no surface (`db_begin`, `str_split_lines`).
            REPORTED, NOT ASSERTED: each is a backlog item needing a surface
            decision (wrap + export it, or delete it), not a CI failure.
  internal  a building block never meant to cross the ABI (`str_buf_new` and
            friends are a string-builder API; `ws_build_upgrade_response` is
            server plumbing).  Accepted.
  dup       a second copy of something that IS surfaced under another name
            (`file_join` next to `path.join`).  Reported as a deletion
            candidate; deleting C needs owner sign-off.

What IS asserted (`--check`, which `make check-core-reach` runs in `ci`):

  1. every finding is classified -- a NEW public C function with no surface
     fails until someone decides which of the two it is.  This is the gate the
     three above cannot be: it is what would have caught 136.44a;
  2. no classification is stale -- a triage line whose function is now
     surfaced (or gone) fails until the line is deleted, so the list only
     ever shrinks and cannot silently over-cover.

Callers outside src/stdlib (the compiler proper, test/) are counted and shown,
but a function reached only by its own unit test still has no surface for a
toke program, so test callers do not remove a finding.

Usage:
  python3 scripts/report_core_reach.py            # report, exit 0
  python3 scripts/report_core_reach.py --check    # also assert 1 and 2 above
  python3 scripts/report_core_reach.py --src DIR  # read headers/C from DIR
                                                  # (negative controls)
"""
import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
TRIAGE = REPO / 'scripts' / 'core_reach_triage.txt'
QUARANTINE = REPO / 'scripts' / 'check_tki_skiplist.txt'
TKI_DIR = REPO / 'stdlib'
TEST_DIR = REPO / 'test'

DECL = re.compile(r'\b([a-z][a-z0-9_]{2,})\s*\([^;{]*\)\s*;', re.S)
# lifecycle names that are not capabilities
NOISE = re.compile(r'(_free|_init|_destroy|_cleanup|_release|_reset)$')
# C header stem -> .tki stem, where they differ.  Without this the whole of
# tk_test.h was reported as unsurfaced, including `test.assert`, which is
# exported from stdlib/test.tki.
# 135.3b renamed math.h -> tkmath.h (so it cannot shadow <math.h>); without
# the alias the gate lost sight of every math core and called their triage
# lines stale.
TKI_ALIAS = {'tk_test': 'test', 'llm_tool': 'llmtool', 'tkmath': 'math'}
CLASSES = ('gap', 'internal', 'dup')


def load_triage():
    triage = {}
    for ln, line in enumerate(TRIAGE.read_text().splitlines(), 1):
        body = line.split('#', 1)[0].strip()
        if not body:
            continue
        parts = body.split()
        if len(parts) != 2 or parts[1] not in CLASSES:
            sys.exit(f'{TRIAGE.name}:{ln}: expected "<c_symbol> <{"|".join(CLASSES)}>  # reason", got: {line!r}')
        if '#' not in line or not line.split('#', 1)[1].strip():
            sys.exit(f'{TRIAGE.name}:{ln}: every classification needs a reason after #')
        triage[parts[0]] = parts[1]
    return triage


def findings(src):
    quarantined = set()
    for line in QUARANTINE.read_text().splitlines():
        line = line.split('#')[0].strip()
        if '::' in line:
            quarantined.add(line.split('::')[1].strip())

    allc = ''.join(p.read_text(errors='replace') for p in sorted(src.glob('*.c')))
    outside = ''.join(p.read_text(errors='replace')
                      for p in sorted(src.parent.glob('*.c')))
    tests = ''.join(p.read_text(errors='replace')
                    for p in sorted(TEST_DIR.rglob('*.c')))

    def count(text, n):
        return len(re.findall(r'\b' + re.escape(n) + r'\s*\(', text))

    out = []
    for h in sorted(src.glob('*.h')):
        mod = h.stem
        if not (src / f'{mod}.c').exists():
            continue
        tmod = TKI_ALIAS.get(mod, mod)
        tki = TKI_DIR / f'{tmod}.tki'
        exports = set()
        if tki.exists():
            d = json.loads(tki.read_text())
            exports = {e['name'] for e in d.get('exports', []) or []}
        declared = {n for n in DECL.findall(h.read_text(errors='replace'))
                    if n.startswith(mod + '_')}
        for n in sorted(declared):
            if NOISE.search(n):
                continue
            if count(allc, n) > 1:       # defined AND called somewhere in the C
                continue
            if count(outside, n) > 0:    # called by the compiler proper
                continue
            rest = n[len(mod) + 1:]
            name = f'{tmod}.{rest.replace("_", "")}'
            if name in exports or name in quarantined:
                continue
            if f'{tmod}.{rest}' in exports:
                continue
            out.append((n, name, count(tests, n)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--src', type=pathlib.Path, default=REPO / 'src' / 'stdlib')
    a = ap.parse_args()

    found = findings(a.src)
    triage = load_triage()
    names = {n for n, _, _ in found}

    by_class = {c: [] for c in CLASSES}
    untriaged = []
    for n, name, t in found:
        cls = triage.get(n)
        tag = f'{name:34} (C: {n}{"; unit-tested only" if t else ""})'
        (by_class[cls] if cls else untriaged).append(tag)
    stale = sorted(set(triage) - names)

    print(f'core reach: {len(found)} public C functions in src/stdlib with no '
          f'.tki export and no C caller')
    print(f'  gap       {len(by_class["gap"]):4}  real capability, no surface '
          f'-- REPORTED, NOT ASSERTED (backlog: 136.50a)')
    print(f'  internal  {len(by_class["internal"]):4}  not meant to cross the ABI -- accepted')
    print(f'  dup       {len(by_class["dup"]):4}  second copy of a surfaced capability -- deletion candidate, reported')
    print(f'  untriaged {len(untriaged):4}')
    print(f'  stale     {len(stale):4}  triage lines whose function is now surfaced or gone')
    print('\nReported, not asserted -- the gap list:')
    for t in by_class['gap']:
        print('   ', t)
    for t in untriaged:
        print('UNTRIAGED ', t)
    for s in stale:
        print('STALE     ', s, '-- delete its line from scripts/core_reach_triage.txt')

    if a.check and (untriaged or stale):
        print(f'\nFAIL: {len(untriaged)} untriaged, {len(stale)} stale. '
              'Classify each new finding in scripts/core_reach_triage.txt '
              '(gap, internal or dup, with a reason), or give it a surface.')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
