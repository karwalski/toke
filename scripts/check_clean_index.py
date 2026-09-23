#!/usr/bin/env python3
"""136.43 — fail when a repository's index holds staged content nobody can see.

WHAT THIS CATCHES, and why an ordinary `git status` does not.

On 2026-09-22 a sweep found THREE sibling repositories each holding a staged
revert of the same 2026-09-19 correction -- the Gate 1 Pass@1 figure, the single
most consequential fact in the project, which took 48 files across 7 repos to
correct.  In every one the worktree was IDENTICAL to HEAD: the file on disk looked
untouched, `git diff HEAD` was empty for it, and the staged blob appeared only
under `git diff --cached`.  What is invisible is not that something is staged
-- `git status` says so -- but that the file on disk is NOT what a bare commit
would record.  They
would have fired on the next bare `git commit` in those repos, by anyone, for
any reason, and the result would have read as a deliberate retraction.

Three more of exactly this shape DID fire on 2026-09-21 (00347fc, 5334d4f,
3771868), so this is not hypothetical and it is not a same-session race.

TWO CHECKS, because they are different failures.

  ARMED   staged, and the worktree file is byte-identical to HEAD.  This is the
          dangerous one: the file on disk looks untouched, `git diff HEAD` is
          empty for it, and the staged content is a change nobody is holding in
          mind.  Always a finding.

  STAGED  staged with a differing worktree -- the ordinary mid-commit state for
          a person at a keyboard.  Reported, and only fatal under --strict
          (which is what CI should use, where the index is always clean).

AGENTS.md §12.4's pathspec-commit rule protects the paths you name, because a
pathspec commit takes worktree content and ignores the index.  It does not
protect against a bare `git commit` at all.  That is the hole this closes.

Exit 0 clean, 1 on a finding, 2 if it cannot run (not a git repo, git absent) --
a check that cannot run must fail, not pass vacuously.
"""
import argparse
import pathlib
import subprocess
import sys


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args],
                          capture_output=True, text=True)


def inspect(repo):
    """Return (armed, staged) path lists, or None if repo is not usable."""
    probe = git(repo, "rev-parse", "--git-dir")
    if probe.returncode != 0:
        return None
    cached = git(repo, "diff", "--cached", "--name-only")
    if cached.returncode != 0:
        return None
    names = [n for n in cached.stdout.splitlines() if n.strip()]
    armed, staged = [], []
    for n in names:
        # worktree vs HEAD for this one path: empty means the file on disk is
        # what HEAD says, so the staged blob is invisible to `git diff`.
        wt = git(repo, "diff", "HEAD", "--name-only", "--", n)
        if wt.returncode == 0 and not wt.stdout.strip():
            armed.append(n)
        else:
            staged.append(n)
    return armed, staged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repos", nargs="*",
                    help="repositories to check (default: the current one)")
    ap.add_argument("--sweep", metavar="DIR",
                    help="check every git repository directly under DIR")
    ap.add_argument("--strict", action="store_true",
                    help="also fail on ordinary staged-with-differing-worktree "
                         "content; use this in CI, where the index is clean")
    a = ap.parse_args()

    targets = [pathlib.Path(r) for r in a.repos]
    if a.sweep:
        base = pathlib.Path(a.sweep).expanduser()
        if not base.is_dir():
            print(f"FAIL anchor: {base} is not a directory", file=sys.stderr)
            return 2
        targets += sorted(p for p in base.iterdir() if (p / ".git").exists())
    if not targets:
        targets = [pathlib.Path.cwd()]

    checked = 0
    armed_total = staged_total = 0
    for repo in targets:
        r = inspect(repo)
        if r is None:
            if a.sweep:
                continue          # not every directory under a sweep is a repo
            print(f"FAIL anchor: {repo} is not a usable git repository",
                  file=sys.stderr)
            return 2
        checked += 1
        armed, staged = r
        for n in armed:
            print(f"ARMED  {repo}: {n}\n"
                  f"       staged, and the worktree file matches HEAD -- the "
                  f"file on disk looks untouched, `git diff HEAD` is empty for "
                  f"it, and the next bare `git commit` would land the staged "
                  f"blob instead (136.43)", file=sys.stderr)
        armed_total += len(armed)
        for n in staged:
            lvl = "STAGED" if not a.strict else "FAIL  "
            print(f"{lvl} {repo}: {n}  (staged; worktree differs)",
                  file=sys.stderr)
        staged_total += len(staged)

    if not checked:
        print("FAIL anchor: no git repository was checked", file=sys.stderr)
        return 2

    if armed_total or (a.strict and staged_total):
        print(f"\n{armed_total} armed and {staged_total} ordinary staged path(s) "
              f"across {checked} repo(s). Defuse with `git restore --staged "
              f"<path>` -- that leaves the worktree untouched.", file=sys.stderr)
        return 1

    note = f" ({staged_total} ordinary staged path(s), not fatal)" if staged_total else ""
    print(f"index clean: no armed staged content in {checked} repo(s){note}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
