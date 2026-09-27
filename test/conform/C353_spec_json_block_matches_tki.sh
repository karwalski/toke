#!/usr/bin/env bash
# C353_spec_json_block_matches_tki.sh — docs/spec/stdlib-signatures.md's
# std.json block says what stdlib/json.tki declares (story 135.27).
#
# THE DEFECT.  The spec block was written for a string-keyed API that no
# longer exists: f=dec(s:$str):i64, f=str(s:$str;key:$str):$str (json.tki:
# dec returns Json!JsonErr, the getters take a Json and return T!JsonErr),
# no json.u64, no $json/$jsonerr types, a $writer type json.tki calls
# JsonWriter, and parse/print with invented types where json.tki has i64 ABI
# placeholders.  Batch LS added only `at` and `getval`.  Nothing read the
# block, so nothing noticed.
#
# THE RULE (both directions):
#   1. every `f=` line names a typed (non-"gen":"abi") func export, and every
#      typed func export has an `f=` line; parameter types and return type
#      agree after spelling normalisation ($ dropped, @T / @(T) read as [T],
#      case folded, a "json." prefix dropped).
#   2. every `t=` line names a json.tki type with the same field names and
#      types, and every json.tki type has a `t=` line.
#   3. the ABI-helper list (`name/arity`) names exactly the "gen":"abi" func
#      exports, each with its arity.
#
# Negative control (run 2026-09-28): C353_SPEC=<main 8f63fc6's
# stdlib-signatures.md>: fails (dec/str/... return types, u64 missing,
# $writer vs JsonWriter, no ABI list); C353_TKI=<json.tki plus a typed
# json.newthing export>: fails "typed export json.newthing has no f= line".
#
# Story: 135.27
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SPEC="${C353_SPEC:-${REPO_ROOT}/docs/spec/stdlib-signatures.md}"
TKI="${C353_TKI:-${REPO_ROOT}/stdlib/json.tki}"
echo "C353: spec std.json block matches stdlib/json.tki (135.27)"
echo "------------------------------------------------------------------------"
python3 - "${SPEC}" "${TKI}" <<'PYEOF'
import json, re, sys
spec_path, tki_path = sys.argv[1], sys.argv[2]
text = open(spec_path).read()
m = re.search(r'^### std\.json\n(.*?)(?=^### )', text, re.S | re.M)
if not m:
    print("  FAIL: no '### std.json' section"); print("\nC353: 0 passed, 1 failed"); sys.exit(1)
sect = m.group(1)
blk = re.search(r'```text\n(.*?)```', sect, re.S)
body = blk.group(1) if blk else ""
body = re.sub(r'\(\*.*?\*\)', '', body, flags=re.S)       # drop (* comments *)
after = sect[blk.end():] if blk else sect

def norm(t):
    t = t.strip().replace(' ', '').replace('$', '').lower()
    t = re.sub(r'@\(([^()]*)\)', r'[\1]', t)
    while True:
        n = re.sub(r'@([a-z0-9_]+)', r'[\1]', t)
        if n == t: break
        t = n
    return t

tki = json.load(open(tki_path))["exports"]
typed = {e["name"].split(".", 1)[1]: e for e in tki if e.get("kind") == "func" and e.get("gen") != "abi"}
abi = {e["name"].split(".", 1)[1]: len(e["params"]) for e in tki if e.get("kind") == "func" and e.get("gen") == "abi"}
types = {e["name"].lower(): e for e in tki if e.get("kind") == "type"}

fails = []; passes = 0
seen_f = set(); seen_t = set()
for line in body.splitlines():
    line = line.strip()
    fm = re.match(r'^f=(?:json\.)?([a-z0-9_]+)\((.*)\):(.+)$', line)
    tm = re.match(r'^t=\$([a-z0-9_]+)\{(.*)\}$', line)
    if fm:
        name, ps, ret = fm.group(1), fm.group(2), fm.group(3)
        seen_f.add(name)
        e = typed.get(name)
        if not e:
            fails.append("spec f=%s is not a typed json.tki export" % name); continue
        sp = [norm(p.split(':', 1)[1]) for p in ps.split(';') if p.strip()]
        tp = [norm(p) for p in e["params"]]
        if sp != tp: fails.append("%s params: spec %s, tki %s" % (name, sp, tp)); continue
        if norm(ret) != norm(e["return"]):
            fails.append("%s return: spec %s, tki %s" % (name, norm(ret), norm(e["return"]))); continue
        passes += 1
    elif tm:
        name, fs = tm.group(1), tm.group(2)
        seen_t.add(name)
        e = types.get(name)
        if not e: fails.append("spec t=$%s is not a json.tki type" % name); continue
        sf = [(norm(f.split(':', 1)[0]), norm(f.split(':', 1)[1])) for f in fs.split(';') if f.strip()]
        tf = [(norm(f["name"]), norm(f["type"])) for f in e.get("fields", [])]
        if sf != tf: fails.append("type %s fields: spec %s, tki %s" % (name, sf, tf)); continue
        passes += 1
for name in sorted(set(typed) - seen_f): fails.append("typed export json.%s has no f= line" % name)
for name in sorted(set(types) - seen_t): fails.append("json.tki type %s has no t= line" % name)
listed = dict((n, int(a)) for n, a in re.findall(r'`([a-z0-9_]+)/([0-9]+)`', after))
if listed != abi:
    miss = sorted(set(abi) - set(listed)); extra = sorted(set(listed) - set(abi))
    bad = sorted(n for n in set(abi) & set(listed) if abi[n] != listed[n])
    fails.append("ABI-helper list: missing %s, not exports %s, wrong arity %s" % (miss, extra, bad))
else:
    passes += 1
for f in fails: print("  FAIL: " + f)
print("  PASS: %d signatures / types / lists agree" % passes)
print("\nC353: %d passed, %d failed" % (passes, len(fails)))
sys.exit(1 if fails else 0)
PYEOF
