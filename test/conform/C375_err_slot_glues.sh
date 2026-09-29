#!/usr/bin/env bash
# C375_err_slot_glues.sh — every `T!E` stdlib export either fills the error
# slot in its glue, or is on the bare-glue exception list — exactly
# (127.238, Batch GB2).
#
# THE DEFECT.  llvm.c's is_stdlib_slot_wrapper was a hand list of the
# wrappers whose glue keeps @tk_current_error (runtime-abi.md §7), patched
# one module at a time (138.3, 138.6).  Every other module's `$err:e e.msg`
# bound nil (RT005): auth, csv, dataframe, infer, ml, template, ws, http's
# param/header, ...  The set is now DERIVED from the .tki (every `T!E`
# export with T not void) minus g_err_slot_bare[] in src/stdlib_tables.c:
# the exports whose glue returns a bare 0/null on failure, which must stay on
# the value sentinel until fixed — as a slot wrapper, their failure would
# read as a success.
#
# WHAT THIS GATE CHECKS, statically, over stdlib/*.tki and src/stdlib/*.c:
#   a wrapper FILLS THE SLOT when its body — or a function it calls, to any
#   depth — assigns tk_current_error a non-zero value (a raise) AND assigns
#   it 0 (the ok-path clear); `x ? 1 : 0` counts as both.  A wrapper that
#   has no ok path at all — a stub that always fails, like std.infer's and
#   std.mlx's (127.241) — fills it too when EVERY `return` in its body
#   returns the result of a raising function: there is no success to clear
#   for.  Then:
#     1. every `T!E` export that does not fill the slot is on the list
#        (a new bare glue fails here: fix the glue or list it, dated);
#     2. every list entry is a `T!E` export that does NOT fill the slot
#        (a fixed glue fails here: delete its entry — the list only shrinks);
#     3. the json get* accessors llvm.c keeps by name (their .tki says plain
#        i64) fill the slot.
#   It is a static lower bound: "assigns on some path", not "on every
#   path".  It cannot see a glue that raises on one failure path and returns
#   bare 0 on another; the per-module conform suites carry that.
#
# Negative controls (each shown red, then restored): deleting one list entry
# (tk_csv_parse_w) -> check 1 red naming it; adding a filling wrapper
# (tk_toml_str_w) -> check 2 red naming it.  127.241: an always-raising stub
# given one bare `return 0` path (tk_infer_embed_w) -> check 1 red naming
# it; without the always-raise rule the seven infer/mlx stubs were red.
#
# Story: 127.238, 127.241

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="${C375_ROOT:-${REPO_ROOT}}"

python3 - "${ROOT}" <<'PYEOF'
import glob, json, os, re, sys
root = sys.argv[1]
bodies = {}
fnre = re.compile(r'^[A-Za-z_][\w \*]*?\b(\w+)\s*\(([^;{)]*)\)\s*\{', re.M)
for p in sorted(glob.glob(os.path.join(root, 'src', 'stdlib', '*.c'))):
    s = open(p, errors='replace').read()
    for m in fnre.finditer(s):
        i, depth = m.end(), 1
        while i < len(s) and depth:
            depth += {'{': 1, '}': -1}.get(s[i], 0); i += 1
        bodies.setdefault(m.group(1), s[m.end():i])
ASG = re.compile(r'tk_current_error\s*=(?!=)\s*([^;]*);')
def kinds(b):
    r = c = False
    for m in ASG.finditer(b):
        v = m.group(1).strip()
        if re.fullmatch(r'\(?\s*0[A-Za-z]*\s*\)?', v): c = True
        elif '?' in v and re.search(r'[?:]\s*0\b', v): r = c = True
        else: r = True
    return r, c
calls = {n: set(re.findall(r'\b(\w+)\s*\(', b)) & bodies.keys() for n, b in bodies.items()}
def closure(seed):
    out = set(seed); grew = True
    while grew:
        grew = False
        for n, cs in calls.items():
            if n not in out and cs & out: out.add(n); grew = True
    return out
raises = closure({n for n, b in bodies.items() if kinds(b)[0]})
clears = closure({n for n, b in bodies.items() if kinds(b)[1]})
fills = raises & clears
# 127.241: an always-failing wrapper — every return is `return <raiser>(…)`.
RET = re.compile(r'\breturn\b\s*([^;]*);')
def always_raises(b):
    rets = RET.findall(b)
    if not rets: return False
    for r in rets:
        m = re.match(r'(\w+)\s*\(', r.strip())
        if not m or m.group(1) not in raises: return False
    return True
fills |= {n for n, b in bodies.items() if n in raises and always_raises(b)}

te = {}
for t in sorted(glob.glob(os.path.join(root, 'stdlib', '*.tki'))):
    fmod = os.path.basename(t)[:-4]
    for e in json.load(open(t)).get('exports', []):
        ret, name = e.get('return', ''), e.get('name', '')
        if '!' not in ret or '.' not in name or ret.split('!')[0] == 'void': continue
        mod, meth = name.split('.', 1)
        w = e['c_name'] if e.get('kind') == 'extern_c' and e.get('c_name') else f'tk_{mod}_{meth}_w'
        te[w] = (os.path.basename(t), ret)
        # the import spelling (tki_entry_for_symbol): std.dataframe's df.x
        # is also called as tk_dataframe_x_w; both glues must agree
        if mod != fmod and e.get('kind') != 'extern_c':
            alt = f'tk_{fmod}_{meth}_w'
            if alt in bodies: te[alt] = (os.path.basename(t), ret)

tab = open(os.path.join(root, 'src', 'stdlib_tables.c')).read()
m = re.search(r'BEGIN 127\.238 bare-glue exceptions \*/(.*?)/\* END 127\.238', tab, re.S)
if not m: print('FAIL: the bare-glue exception list markers are missing'); sys.exit(1)
listed = re.findall(r'"(\w+)"', re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S))

fail = 0
dup = sorted({w for w in listed if listed.count(w) > 1})
for w in dup: print(f'FAIL: {w} is listed twice'); fail += 1
# 1. every T!E export with a C body that does not fill the slot is listed
for w, (f, ret) in sorted(te.items()):
    if w in bodies and w not in fills and w not in listed:
        print(f'FAIL 1: {w} ({f}: {ret}) does not fill the error slot and is not on the list'); fail += 1
# 2. every entry is a non-filling T!E export
for w in listed:
    if w not in te:
        print(f'FAIL 2: {w} is listed but is not a T!E export of any .tki'); fail += 1
    elif w in fills:
        print(f'FAIL 2: {w} now fills the error slot: delete its entry'); fail += 1
# 3. the by-name json accessors fill the slot
for w in ['tk_json_getbool_w', 'tk_json_getint_w', 'tk_json_geti64_w',
          'tk_json_geti32_w', 'tk_json_getf32_w', 'tk_json_getfloat_w']:
    if w not in fills:
        print(f'FAIL 3: {w} is kept as a slot wrapper by name but does not fill the slot'); fail += 1
nofn = sorted(w for w in te if w not in bodies)
derived = sorted(w for w in te if w in fills)
print(f'C375: {len(te)} T!E exports; {len(derived)} fill the slot (derived slot wrappers); '
      f'{len(listed)} listed bare; {len(nofn)} resolve to another symbol (not checked: {", ".join(nofn)})')
print(f'C375: {"FAIL" if fail else "OK"} ({fail} problem(s))')
sys.exit(1 if fail else 0)
PYEOF
