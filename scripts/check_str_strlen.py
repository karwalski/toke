#!/usr/bin/env python3
"""127.170 gate 3 -- the reader ratchet for length-carrying strings.

docs/runtime-abi.md §3.1 (option S1): a toke string from a converted producer
carries its length in a header that tk_strlen() reads in O(1); every other
string falls back to strlen.  An unconverted reader is therefore CORRECT, only
slow, so this gate is a performance ratchet, not a safety check -- the safety
gate is TK_STR_VERIFY (gate 1), and linear scaling is test/perf/strlen (gate 2).

CHECK (default; `make check-str-strlen`, in `make lint`)
  Counts the lines calling bare `strlen(` in every C file under src/stdlib
  (tk_strlen and other *strlen names do not count) and compares each file with
  scripts/check_str_strlen_skiplist.txt:
    - NEW    a file has more strlen lines than its entry allows (or none) --
             use tk_strlen (tk_strhdr.h) or, if the string never reached toke,
             raise the entry in the same commit and say why
    - STALE  a file has FEWER than its entry: lower the entry (the list only
             shrinks), or delete the line at zero
  and that the CORE readers -- the per-call O(len) operations the design names
  (.len, charat, char_at, slice/substr, setat, endswith, trimsuffix, concat,
  interpolation, builder add) -- contain no bare strlen at all:
    - CORE   a core reader calls strlen
  plus one shape check: str.indexfrom's bound reads the header (tks_hdrlen).

CENSUS (`--census`)
  Prints what is left: strlen lines and files, converted readers (tk_strlen /
  tks_hdrlen), sealed producers (tks_alloc / tks_dup / tks_strdup), and the
  strdup/strndup producer lines still unconverted.

`--root DIR` points the check at another tree (negative controls).
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

STRLEN = re.compile(r'(?<![A-Za-z0-9_])strlen\s*\(')
SKIPLIST = 'scripts/check_str_strlen_skiplist.txt'

# file -> functions that must not call strlen (runtime-abi §3.1 consumer
# inventory, "core string readers").  Wrappers that only delegate are listed
# too, so a wrapper that grows its own strlen is caught.
CORE = {
    'src/stdlib/str.c': ['str_len', 'str_concat', 'str_slice', 'str_ends_with',
                         'str_trimsuffix'],
    'src/stdlib/str_glue.c': ['tk_str_len_w', 'tk_str_concat_w', 'tk_str_charat_w',
                              'tk_str_indexfrom_w', 'tk_str_slice_w', 'tk_str_substr_w',
                              'tk_str_setat_w', 'tk_str_endswith_w', 'tk_str_ends_w',
                              'tk_str_trimsuffix_w', 'tk_str_add_w'],
    'src/stdlib/tk_runtime.c': ['tk_str_concat', 'tk_str_join_n', 'tk_str_len',
                                'tk_str_char_at'],
}
# Core readers not yet converted.  Landing step 2 empties this; an entry that
# no longer calls strlen is STALE.
PENDING_CORE: set[tuple[str, str]] = set()

MUST_CALL = {('src/stdlib/str_glue.c', 'tk_str_indexfrom_w'): 'tks_hdrlen('}


def strip_comments(text: str) -> str:
    """Blank out /* */ and // comments and string literals, keeping newlines."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * (j - i - 2) + (c if j - i >= 2 else '')); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def strlen_lines(code: str) -> int:
    return sum(1 for line in code.splitlines() if STRLEN.search(line))


def function_body(code: str, name: str) -> str | None:
    """The text of `name`'s definition body (top-level, brace-matched)."""
    m = re.search(r'^[A-Za-z][^;{}]*?\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', code, re.M)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(code)):
        if code[j] == '{':
            depth += 1
        elif code[j] == '}':
            depth -= 1
            if depth == 0:
                return code[i:j + 1]
    return None


def load_skiplist(root: pathlib.Path) -> dict[str, int]:
    entries: dict[str, int] = {}
    for raw in (root / SKIPLIST).read_text().splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        path, count = line.split()
        entries[path] = int(count)
    return entries


def scan(root: pathlib.Path) -> dict[str, str]:
    # tk_strhdr.h is excluded: its strlen IS the fallback every reader takes.
    files = sorted(p for p in (root / 'src/stdlib').rglob('*')
                   if p.suffix in ('.c', '.h') and p.is_file() and p.name != 'tk_strhdr.h')
    return {str(p.relative_to(root)): strip_comments(p.read_text(errors='replace'))
            for p in files}


def census(codes: dict[str, str]) -> None:
    def count(rx: str) -> tuple[int, int]:
        r = re.compile(rx)
        per = [sum(1 for l in c.splitlines() if r.search(l)) for c in codes.values()]
        return sum(per), sum(1 for x in per if x)
    rows = [
        ('bare strlen( lines (unconverted readers)', r'(?<![A-Za-z0-9_])strlen\s*\('),
        ('tk_strlen( / tks_hdrlen( lines (converted readers)', r'\b(tk_strlen|tks_hdrlen)\s*\('),
        ('tks_alloc( / tks_dup( / tks_strdup( lines (sealed producers)', r'\b(tks_alloc|tks_dup|tks_strdup)\s*\('),
        ('strdup( / strndup( lines (unconverted producers)', r'(?<![A-Za-z0-9_])(strdup|strndup)\s*\('),
    ]
    print(f'census over {len(codes)} C files in src/stdlib:')
    for label, rx in rows:
        lines, files = count(rx)
        print(f'  {lines:5d} in {files:3d} files  {label}')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    ap.add_argument('--root', default=str(pathlib.Path(__file__).resolve().parent.parent))
    ap.add_argument('--census', action='store_true')
    args = ap.parse_args()
    root = pathlib.Path(args.root)
    codes = scan(root)
    if args.census:
        census(codes)
        return 0

    problems: list[str] = []
    allowed = load_skiplist(root)
    seen = set()
    for path, code in codes.items():
        n = strlen_lines(code)
        want = allowed.get(path)
        if want is not None:
            seen.add(path)
        if n and want is None:
            problems.append(f'NEW    {path}: {n} strlen line(s), no skiplist entry -- use tk_strlen')
        elif want is not None and n > want:
            problems.append(f'NEW    {path}: {n} strlen lines, skiplist allows {want} -- use tk_strlen')
        elif want is not None and n < want:
            problems.append(f'STALE  {path}: {n} strlen lines, skiplist says {want} -- lower the entry'
                            + (' (delete it at 0)' if n == 0 else ''))
    for path in sorted(set(allowed) - seen):
        problems.append(f'STALE  {path}: listed but no such C file under src/stdlib')

    pending_seen = set()
    for path, names in CORE.items():
        code = codes.get(path)
        if code is None:
            problems.append(f'CORE   {path}: missing')
            continue
        for name in names:
            body = function_body(code, name)
            if body is None:
                problems.append(f'CORE   {path}: {name} not found (renamed? update CORE)')
                continue
            found = []
            bad = strlen_lines(body)
            if bad:
                found.append(f'CORE   {path}: {name} calls strlen ({bad} line(s)) -- a core reader must use tk_strlen')
            need = MUST_CALL.get((path, name))
            if need and need not in body:
                found.append(f'CORE   {path}: {name} must read the header ({need[:-1]})')
            if (path, name) in PENDING_CORE:
                pending_seen.add((path, name))
                if not found:
                    problems.append(f'STALE  {path}: {name} is converted -- remove it from PENDING_CORE')
                continue
            problems.extend(found)
    for path, name in sorted(PENDING_CORE - pending_seen):
        problems.append(f'STALE  PENDING_CORE names {path}:{name}, which is not a CORE reader')

    total = sum(strlen_lines(c) for c in codes.values())
    if problems:
        print('\n'.join(problems))
        print(f'check-str-strlen: FAIL ({len(problems)} problem(s); {total} strlen lines in src/stdlib)')
        return 1
    pend = f', {len(PENDING_CORE)} core reader(s) pending' if PENDING_CORE else ''
    print(f'check-str-strlen: OK ({total} strlen lines in {sum(1 for c in codes.values() if strlen_lines(c))} '
          f'files on the skiplist; {sum(len(v) for v in CORE.values()) - len(PENDING_CORE)} core readers clean{pend})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
