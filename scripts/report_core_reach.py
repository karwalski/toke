#!/usr/bin/env python3
"""136.50a — report finished C capabilities that no interface mentions at all.

WHY THIS EXISTS.  Three gates already look at this area and none of them can see
this class:

  * `check-tki` checks that every `.tki` export resolves to a defined symbol.
    It is honest and its 48-entry quarantine tracks the known backlog -- canvas
    alone is 13 of them.  But it starts from the .tki, so a capability the .tki
    never mentions is invisible to it by construction.
  * `check-glue-core` (136.50) catches a `*_glue.c` that never includes its own
    core's header.  It passes a glue that includes the header and wraps a third
    of it.
  * 136.44's stub sweep compared declared exports against the glue.  136.44a --
    `tls_gen_self_signed_alg`, complete and linked since July, ML-DSA-65
    certificate generation unreachable from toke -- was declared on NEITHER
    side, so there was nothing to disagree with.

So the detector has to run the other way round: enumerate the module's own
public header and subtract everything the C actually calls and everything any
interface mentions.  What remains is finished work with no way in.

WHAT IT DELIBERATELY DOES NOT DO.  It is a REPORT, not a gate, and it must not
be wired into `ci` until its output has been triaged.  Its output mixes three
things that look identical to a regex and are not the same finding:

  1. a real capability with no surface (`db_begin`, `str_split_lines`),
  2. an internal building block that was never meant to cross the ABI
     (`str_buf_new` and friends are a string-builder API),
  3. a function reached only from outside `src/stdlib/` -- tests, or another
     subsystem -- which this script cannot see.

Lifecycle names (`*_free`, `*_init`, ...) are filtered out; nothing else is.
Triage before acting, and before turning any part of this into a gate.
"""
import re, json, pathlib
root = pathlib.Path('/Users/matthew.watt/tk/toke/src/stdlib')
sl   = pathlib.Path('/Users/matthew.watt/tk/toke/scripts/check_tki_skiplist.txt')
tkid = pathlib.Path('/Users/matthew.watt/tk/toke/stdlib')
DECL = re.compile(r'\b([a-z][a-z0-9_]{2,})\s*\([^;{]*\)\s*;', re.S)

quarantined = set()
for line in sl.read_text().splitlines():
    line = line.split('#')[0].strip()
    if '::' in line:
        quarantined.add(line.split('::')[1].strip())

# every call site anywhere in the C stdlib
allc = ''.join(p.read_text(errors='replace') for p in root.glob('*.c'))
called = collections = {}
def uses(n):
    return len(re.findall(r'\b'+re.escape(n)+r'\s*\(', allc))

# lifecycle names that are not capabilities
NOISE = re.compile(r'(_free|_init|_destroy|_cleanup|_release|_reset)$')

rows=[]; total=0
for h in sorted(root.glob('*.h')):
    mod=h.stem
    if not (root/f'{mod}.c').exists(): continue
    tki = tkid/f'{mod}.tki'
    exports=set(); 
    if tki.exists():
        d=json.loads(tki.read_text())
        exports={e['name'] for e in d.get('exports',[]) or []}
    declared={n for n in DECL.findall(h.read_text(errors='replace')) if n.startswith(mod+'_')}
    out=[]
    for n in sorted(declared):
        if NOISE.search(n): continue
        if uses(n) > 1: continue          # defined AND called somewhere in the C
        tail = n[len(mod)+1:].replace('_','')
        name = f'{mod}.{tail}'
        if name in exports or name in quarantined: continue
        # also allow the .tki to spell it with the underscores kept
        if f'{mod}.{n[len(mod)+1:]}' in exports: continue
        out.append(name + f'   (C: {n})')
    if out:
        rows.append((mod,out)); total+=len(out)

for mod,out in sorted(rows):
    print(f"== {mod}")
    for o in out: print("    ", o)
print(f"\n{total} finished C functions across {len(rows)} modules that NO .tki mentions and nothing in the C calls.")
