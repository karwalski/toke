#!/usr/bin/env bash
# C2055_emit_deps_several_files_merged.sh -- `tkc --emit-deps` given several
# files prints ONE merged set in the single-file layout (story 138.54).
#
# THE BUG.  `tkc --emit-deps h.tk a.tk` (h imports std.http) printed h's
# sources, `---`, h's flags, then a.tk's sources with no separator, then `---`
# and a.tk's flags.  Every consumer that splits on `---` (ooke's
# scripts/emit_deps.sh, the docs/compiler/build-deps.md recipe) read a.tk's
# sources as link flags, and nothing was deduplicated.
#
# THE CHANGE.  With more than one file the output is the union, in the layout
# of one file: C sources, then (if any) `---` and link flags, then (if any)
# `---` and user modules as <path>.ll; each entry once, first-seen order; a
# `-framework X` pair stays together.  One file's output is unchanged.  A file
# that fails prints nothing on stdout for the whole run (no partial list).
#
# WHAT IS ASSERTED
#   1. h.tk a.tk: exactly one flags separator before the first flag, every
#      line before it is a .c path, every line after it is a flag
#   2. the merged sources are the union of the two single-file source lists
#      (as sets) and have no duplicates; the flags likewise
#   3. single-file output is byte-identical to the per-file section of a
#      run over that file alone (it is printed the same way as before)
#   4. a user module: mn.tk (imports b) with h.tk gives sources, `---`,
#      flags, `---`, b.ll -- three sections, b.ll once
#   5. (macOS) -framework pairs stay adjacent after deduplication
#   6. a file that does not type-check: non-zero exit, nothing on stdout
#
# NEGATIVE CONTROL: TKC=<a tkc before 138.54> -- 1, 2, 4, 5 and 6 FAIL
# (back-to-back sections; the first file's list printed before the failure);
# 3 passes, which is the point of it.
#
# Story: 138.54
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2055_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2055: --emit-deps over several files prints one merged set (138.54)"
echo "------------------------------------------------------------------------"

printf 'm=h;\ni=http:std.http;\nf=main():i64{<0};\n' >h.tk
printf 'm=a;\ni=json:std.json;\ni=io:std.io;\nf=main():i64{io.println("x");<0};\n' >a.tk
printf 'm=b;\ni=json:std.json;\nf=two():i64{<2};\n' >b.tk
printf 'm=mn;\ni=b:b;\ni=io:std.io;\nf=main():i64{io.println("x");<0};\n' >mn.tk
printf 'm=k;\ni=kc:std.keychain;\nf=main():i64{<0};\n' >k.tk
printf 'm=o;\ni=ocr:std.ocr;\ni=kc:std.keychain;\nf=main():i64{<0};\n' >o.tk
printf 'm=bad;\nf=main():i64{<zz};\n' >bad.tk
"${TKC}" --emit-interface --check b.tk >/dev/null 2>&1

# sec N FILE -- the lines of section N (0 = sources) of an --emit-deps output
sec() { awk -v n="$1" '$0=="---"{s++;next} s==n' "$2"; }

"${TKC}" --emit-deps h.tk >h.out 2>/dev/null || fail "emit-deps h.tk failed"
"${TKC}" --emit-deps a.tk >a.out 2>/dev/null || fail "emit-deps a.tk failed"
rc=0; "${TKC}" --emit-deps h.tk a.tk >ha.out 2>ha.err || rc=$?

# 1
nsep="$(grep -c -x -- '---' ha.out)"
nonc="$(sec 0 ha.out | grep -v '\.c$' | head -1)"
cflag="$(sec 1 ha.out | grep '\.c$' | head -1)"
if [ "${rc}" -eq 0 ] && [ "${nsep}" = 1 ] && [ -z "${nonc}" ] && [ -z "${cflag}" ] \
   && [ -n "$(sec 1 ha.out)" ]; then
    pass "1 h.tk a.tk: sources, one '---', flags -- no source after the separator"
else
    fail "1 h.tk a.tk: rc=${rc}, separators=${nsep}, non-.c source '${nonc}', .c among flags '${cflag}'"
fi

# 2
u_src="$( { sec 0 h.out; sec 0 a.out; } | sort -u)"
u_flg="$( { sec 1 h.out; sec 1 a.out; } | sort -u)"
m_src="$(sec 0 ha.out | sort)"; m_flg="$(sec 1 ha.out | grep -v -x -- '-framework' | sort)"
if [ "${m_src}" = "${u_src}" ] && [ "$(sec 0 ha.out | sort | uniq -d)" = "" ] \
   && [ "$(sec 1 ha.out | sort -u)" = "${u_flg}" ] && [ "$(sec 1 ha.out | sort | uniq -d)" = "" ]; then
    pass "2 the merge is the union of the per-file lists, each entry once"
else
    fail "2 merged set differs from the union"
    diff <(printf '%s\n' "${u_src}") <(printf '%s\n' "${m_src}") | head -6 | sed 's/^/      | /'
fi

# 3: one file is printed exactly as the resolver lists it -- h.tk's output is
#    the sources, `---`, flags, nothing else
want3="$(sec 0 h.out; echo ---; sec 1 h.out)"
if [ "$(cat h.out)" = "${want3}" ] && [ "$(sec 0 h.out | sort | uniq -d)" = "" ]; then
    pass "3 one file: sources, '---', flags (layout unchanged)"
else fail "3 single-file layout changed"; sed 's/^/      | /' h.out | tail -4; fi

# 4
rc=0; "${TKC}" --emit-deps -I . mn.tk h.tk >mh.out 2>mh.err || rc=$?
if [ "${rc}" -eq 0 ] && [ "$(grep -c -x -- '---' mh.out)" = 2 ] && [ "$(sec 2 mh.out)" = "b.ll" ] \
   && [ -z "$(sec 1 mh.out | grep '\.ll$')" ]; then
    pass "4 user module: sources, '---', flags, '---', b.ll (once)"
else
    fail "4 user module (rc=${rc})"; sed -n '/---/,$p' mh.out | sed 's/^/      | /' | head -10
fi

# 5
if [ "$(uname -s)" = Darwin ]; then
    "${TKC}" --emit-deps k.tk o.tk >ko.out 2>/dev/null
    bad5="$(sec 1 ko.out | awk 'p=="-framework" && ($0 ~ /^-/){print "split"} {p=$0} END{if(p=="-framework")print "dangling"}')"
    nfw="$(sec 1 ko.out | grep -c -x -- '-framework')"
    nsec="$(sec 1 ko.out | grep -c -x Security)"
    if [ -z "${bad5}" ] && [ "${nsec}" = 1 ] && [ "${nfw}" -ge 5 ]; then
        pass "5 -framework pairs kept together, Security once (${nfw} frameworks)"
    else fail "5 frameworks: ${bad5} Security=${nsec} frameworks=${nfw}"; sec 1 ko.out | sed 's/^/      | /'; fi
else
    pass "5 (not macOS: no -framework flags to check)"
fi

# 6
rc=0; "${TKC}" --emit-deps h.tk bad.tk >hb.out 2>hb.err || rc=$?
if [ "${rc}" -ne 0 ] && [ ! -s hb.out ]; then
    pass "6 a failing file: exit ${rc}, nothing on stdout"
else fail "6 a failing file: exit ${rc}, $(wc -l <hb.out | tr -d ' ') lines on stdout"; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
