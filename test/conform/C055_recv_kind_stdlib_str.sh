#!/usr/bin/env bash
# C055_recv_kind_stdlib_str.sh — a receiver bound to a stdlib call whose
# interface declares `str` is judged by 127.151's member-kind check
# (story 127.176).
#
# THE DEFECT. At 39b6cfd, with st=std.str:
#
#     let t=st.trim(" ab "); let k=t.keys()   --check 0, ld: undefined _keys
#     let t=st.trim(" ab "); let r=t.pop()    --check 0, prints a pointer
#
# The checker does not type stdlib returns, so C049's check had no kind to
# judge. The emitter tags such a binding "$str" from the same `.tki`
# declaration (tki_declared_tag, 127.80) and dispatches on that tag, so the
# checker now reads the declaration too — for the receiver check only.
#
# WHAT IS ASSERTED:
#   1. Array/map-only members on such a receiver are E4036, no fix.
#   2. str members on it stay clean and run right.
#   3. Not judged: a `T!E` return, a collection return (st.split), a `mut`
#      binding, and a name re-bound to an array after the stdlib binding.
#
# Negative control: TKC built from 39b6cfd — case 1 is clean (red).
#
# Story: 127.176

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
prog() { printf 'm=t;\ni=io:std.io;\ni=st:std.str;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

# ── 1. wrong kind on a stdlib-str receiver ───────────────────────────────
n=0
for call in 'keys()' 'pop()' 'push(1)' 'getor("a";1)' 'map(0)'; do
    n=$((n + 1))
    prog "bad$n" "  let t=st.trim(\" ab \");
  let r=t.${call};"
    expect "1-trim-${call%%(*}-is-E4036" "$(codes "${WORK}/bad$n.tk")" "E4036 "
done
expect "1-no-fix" "$("${TKC}" --check "${WORK}/bad1.tk" 2>&1 | grep -c '"fix":""' || true)" "1"

# ── 2. str members: clean and right ──────────────────────────────────────
prog good '  let t=st.trim(" ab ");
  io.println("\(t.upper()) \(t.len()) \(t.contains("a"))");'
expect "2-str-members-clean" "$(codes "${WORK}/good.tk")" ""
"${TKC}" "${WORK}/good.tk" -o "${WORK}/good" >/dev/null 2>&1
# CHANGED 2026-09-25 (Batch Y1, 127.194): `\(t.contains("a"))` renders as
# `true` (owner decision 127.15); it printed `1` while receiver predicates
# were invisible to 127.94.  The value is unchanged.
expect "2-str-members-run" "$("${WORK}/good" 2>&1)" "AB 2 true"

# ── 3. not judged ────────────────────────────────────────────────────────
prog split '  let p=st.split("a,b";",");
  let q=p.pop();'
expect "3-collection-return-not-judged" "$(codes "${WORK}/split.tk")" ""
printf 'm=t;\ni=file:std.file;\nf=main():i64{\n  let t=file.read("x");\n  let r=t.keys();\n  <0\n};\n' > "${WORK}/eu.tk"
expect "3-error-union-return-not-judged" "$(codes "${WORK}/eu.tk")" ""
prog mutb '  let t=mut.st.trim(" ab ");
  let r=t.keys();'
expect "3-mut-binding-not-judged" "$(codes "${WORK}/mutb.tk")" ""
prog rebound '  let t=st.trim(" ab ");
  io.println(t);
  let t=mut.@(1;2);
  t=t.push(3);'
expect "3-rebound-to-array-not-judged-as-str" "$(codes "${WORK}/rebound.tk")" ""

echo "C055: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
