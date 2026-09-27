#!/usr/bin/env bash
# C245_no_zero_arg_exemption_for_stubs.sh — a zero-argument call into a
# one-parameter stdlib function is E4026 even when its glue is a stub that
# ignores the parameter (story 127.220 item 6).
#
# check_tki_call exempted `actual == 0 && abi == 1` for the symbols in
# stdlib_dummyarg_gen.h (127.61).  Since 136.14 rewrote every genuinely
# zero-argument wrapper to `(void)`, that list holds only stubs of real
# one-parameter functions -- tk_mlx_load_w(model_path),
# tk_mdns_stopbrowse_w(handle), tk_str_emptyof_w(type), ... -- so `m.load()`
# passed --check with its argument missing.
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail (rc=0).
#
# Story: 127.220 (item 6)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

expect "1 mlx.load() -- model path missing" \
    "$(diags 'm=q;i=m:std.mlx;f=main():i64{let x=m.load();<0};')" "E4026|1|0"
expect "2 mdns.stopbrowse() -- handle missing" \
    "$(diags 'm=q;i=d:std.mdns;f=main():i64{let x=d.stopbrowse();<0};')" "E4026|1|0"
expect "3 str.emptyof() -- type missing" \
    "$(diags 'm=q;i=s:std.str;f=main():i64{let x=s.emptyof();<0};')" "E4026|1|0"
expect "4 test.report() -- suite missing" \
    "$(diags 'm=q;i=t:std.test;f=main():i64{let x=t.report();<0};')" "E4026|1|0"
expect "5 the one-argument call stays clean" \
    "$(diags 'm=q;i=d:std.mdns;f=main():i64{let x=d.stopbrowse("_http._tcp");<0};')" "rc=0"
expect "6 a (void) zero-argument function stays clean" \
    "$(diags 'm=q;i=t:std.time;f=main():i64{let x=t.nowms();<0};')" "rc=0"

echo "C245: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
