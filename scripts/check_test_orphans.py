#!/usr/bin/env python3
"""136.56 — every tracked test source is built by something.

The Makefile has no pattern rule for tests: every test target is hand-listed.
That is how eighteen test sources, 6,691 lines, came to have no target and no
caller -- eight of them `*_integration.c` files that
docs/spec/spec-implementation-delta.md cited as evidence of implemented
behaviour while nothing compiled them.  A hand-maintained list needs a check
that notices what it forgot; this is that check.

A test source counts as REACHED when its repo-relative path appears on a
non-comment line of the Makefile, a .github workflow, a scripts/ file, or a
test/ shell script.  A path that appears only in a comment is not reached:
test_keychain.c was "referenced" by keychain_binding.sh for months, in a
comment.

EXEMPT names each file this check deliberately does not require, with the
story that owns it.  An exemption for a file that no longer exists, or that is
now reached, is stale and fails -- the list only shrinks.

Exit 1 on any orphan or stale exemption.  `--root DIR` points the check at
another tree (negative controls).
"""
import argparse
import pathlib
import subprocess
import sys

# Owned elsewhere, named one by one so a NEW harness is not silently exempt.
# 123.10 wired the eight 120.22 fuzz harnesses and removed their exemptions,
# so today nothing is exempt; the mechanism stays for the next owned file.
EXEMPT = {}


def live_lines(path):
    """Text of `path` without comment-only lines (#, //, /* and * leaders)."""
    out = []
    for line in path.read_text(errors='replace').splitlines():
        s = line.lstrip()
        if s.startswith(('#', '//', '/*', '*')):
            continue
        out.append(line)
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent)
    root = ap.parse_args().root.resolve()

    ls = subprocess.run(['git', 'ls-files', '-z', '--', 'test'], cwd=root,
                        capture_output=True, text=True, check=True).stdout
    tracked = sorted(p for p in ls.split('\0') if p.endswith('.c'))

    callers = [root / 'Makefile']
    callers += sorted((root / '.github').rglob('*.yml'))
    callers += sorted(p for p in (root / 'scripts').rglob('*')
                      if p.is_file() and p.suffix in ('.sh', '.py', '.mk', ''))
    callers += sorted((root / 'test').rglob('*.sh'))
    me = pathlib.Path(__file__).resolve()
    text = '\n'.join(live_lines(p) for p in callers
                     if p.is_file() and p.resolve() != me)

    orphans = [t for t in tracked if t not in EXEMPT and t not in text]
    stale = sorted(e for e in EXEMPT if e not in tracked or e in text)

    print(f'check-test-orphans: {len(tracked)} tracked test/**/*.c, '
          f'{len(EXEMPT)} exempt by name, {len(orphans)} orphaned, '
          f'{len(stale)} stale exemption(s)')
    for t in orphans:
        print(f'  ORPHAN  {t}  -- no target, no caller: add a Makefile target '
              'and put it in a TEST_CI_* list')
    for e in stale:
        print(f'  STALE   {e}  -- exemption no longer needed: delete it')
    return 1 if orphans or stale else 0


if __name__ == '__main__':
    sys.exit(main())
