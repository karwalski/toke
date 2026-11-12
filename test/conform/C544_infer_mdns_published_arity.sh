#!/usr/bin/env bash
# C544_infer_mdns_published_arity.sh — infer.load, infer.loadstreaming and
# mdns.advertise can be called the way their .tki publishes them (story
# 135.31 item 1).
#
# THE DEFECT, measured on main (d3a083b8):
#   tk_infer_load_w / tk_infer_loadstreaming_w took the path alone, so the
#   published `infer.load(path;opts)` was E4026 "the implementation takes 1
#   argument" -- the options record could never be passed; and
#   tk_mdns_advertise_w took three scalars where the .tki takes one
#   $servicerecord, so `mdns.advertise(rec)` was E4026 "takes 3".  All three
#   sat on scripts/check_tki_arity_skiplist.txt.
#
# NOW: the glue takes what the .tki publishes (the modules are still stubs:
# no inference or DNS-SD is linked, and they still return 0).  This compiles
# and runs a program making the three published calls.  The arity gate
# itself is `make check-tki` (scripts/check_tki_coverage.py), whose skiplist
# lines for the three were deleted: restoring the old glue fails it with
# three disagreements.
#
# Negative control: TKC=<tkc> picks another compiler; main's rejects all three.
#
# Story: 135.31
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c544_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C544: infer.load/loadstreaming and mdns.advertise take their published arguments"
echo "--------------------------------------------------------------------------------"

cat > a.tk <<'EOF'
m=a;
i=infer:std.infer;
i=mdns:std.mdns;
i=io:std.io;
f=main():i64{
  let o=$inferopts{ngpulayers:0;nthreads:1;seed:1};
  let h=infer.load("/nonexistent";o);
  let s=$streamopts{ramceilinggb:1.0 as f32;prefetchlayers:1;requiresnvme:false};
  let h2=infer.loadstreaming("/nonexistent";s);
  let r=$servicerecord{name:"svc";type:"_http._tcp";port:8080;txt:@("a=b")};
  let ok=mdns.advertise(r);
  io.println("ran");
  <0
};
EOF
"${TKC}" --check a.tk > check.out 2>&1
for call in infer.load infer.loadstreaming mdns.advertise; do
    if /usr/bin/grep -a -q "E4026.*'std\\.${call}'" check.out; then
        FAIL=$((FAIL + 1)); echo "  FAIL: ${call}: $(/usr/bin/grep -a -o "wrong number of arguments for 'std\\.${call}'[^\"]*" check.out | head -1)"
    else
        PASS=$((PASS + 1)); echo "  PASS: ${call} with its published arguments passes --check"
    fi
done
if "${TKC}" a.tk -o a > build.out 2>&1 && [ "$(./a 2>&1)" = "ran" ]; then
    PASS=$((PASS + 1)); echo "  PASS: the program builds and runs"
else
    FAIL=$((FAIL + 1)); echo "  FAIL: build/run: $(head -c 300 build.out)"
fi
echo ""
echo "C544: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
