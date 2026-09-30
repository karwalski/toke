#!/usr/bin/env bash
# C550_ml_stubs_published_arity_no_false_success.sh — std.infer, std.mlx and
# std.mdns stubs take their published arguments and never report a success
# they did not achieve (story 137.11; 121.53 item 5; loke T-11).
#
# THE DEFECT, measured on main (cde89021):
#   - infer.generate and mlx.generate took 2 arguments where the .tki (and the
#     C cores tk_infer_generate / mlx_generate) take 3: the published call
#     `generate(h;prompt;maxtokens)` was E4026 "the implementation takes 2".
#   - mdns.browse took the service type alone (the .tki publishes a callback
#     too) and returned an empty ARRAY into its bool slot — a non-null
#     pointer, so `mdns.browse("_http._tcp")` printed `true`: "browsing
#     started" when no DNS-SD call was ever made.
#   - mdns.isavailable and mlx.unload had no wrapper at all (E4027).
#
# NOW: every call compiles as published; every bool stub is false and every
# T!err stub takes the $err arm with a message saying the module is a stub.
# The arity gate itself is `make check-tki`, whose three arity-skiplist lines
# (and two quarantine lines) were deleted in the same change.
#
# Negative control: TKC=<tkc> picks another compiler; main's fails 8 of 8
# (the three E4026s, then the build: E4027 for the two missing wrappers).
# The `true` from a one-argument mdns.browse was measured on main by hand.
#
# Story: 137.11
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c550_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C550: infer/mlx/mdns stubs: published arity, no false success"
echo "--------------------------------------------------------------"

cat > a.tk <<'EOF'
m=a;
i=infer:std.infer;
i=mlx:std.mlx;
i=mdns:std.mdns;
i=io:std.io;
f=onfound(d:$discovered):void{
  io.println("found")
};
f=main():i64{
  io.println("mlxavail=\(mlx.isavailable())");
  io.println("mdnsavail=\(mdns.isavailable())");
  let mm=$mlxmodel{id:"x";path:"/nonexistent"};
  io.println("mlxunload=\(mlx.unload(mm))");
  let g=mt mlx.generate(mm;"hi";8) { $ok:s "ok:\(s)"; $err:e "err:\(e.msg)" };
  io.println("mlxgen=\(g)");
  let hm=$modelhandle{id:"x"};
  let gi=mt infer.generate(hm;"hi";8) { $ok:s "ok:\(s)"; $err:e "err:\(e.msg)" };
  io.println("infergen=\(gi)");
  io.println("browse=\(mdns.browse("_http._tcp";onfound))");
  <0
};
EOF
"${TKC}" --check a.tk > check.out 2>&1
for call in infer.generate mlx.generate mdns.browse; do
    if /usr/bin/grep -a -q "E402[67][^}]*'\(std\.\)\{0,1\}${call}'" check.out; then
        FAIL=$((FAIL + 1)); echo "  FAIL: ${call}: $(/usr/bin/grep -a -o "wrong number of arguments for 'std\\.${call}'[^\"]*" check.out | head -1)"
    else
        PASS=$((PASS + 1)); echo "  PASS: ${call} with its published arguments passes --check"
    fi
done
if "${TKC}" a.tk -o a > build.out 2>&1; then
    ./a > run.out 2>&1
    line() { /usr/bin/grep -a "^$1=" run.out | head -1 | cut -d= -f2-; }
    chk() { # name got want-regex
        if printf '%s' "$2" | /usr/bin/grep -E -q "$3"; then PASS=$((PASS + 1)); echo "  PASS: $1 = $2"
        else FAIL=$((FAIL + 1)); echo "  FAIL: $1 = '$2' (want /$3/)"; fi; }
    chk mlx.isavailable  "$(line mlxavail)"  '^false$'
    chk mdns.isavailable "$(line mdnsavail)" '^false$'
    chk mlx.unload       "$(line mlxunload)" '^false$'
    chk mdns.browse      "$(line browse)"    '^false$'
    chk mlx.generate+infer.generate "$(line mlxgen) | $(line infergen)" '^err:mlx\.generate: .*stub.* \| err:infer\.generate: .*stub'
else
    FAIL=$((FAIL + 5)); echo "  FAIL: build: $(head -c 300 build.out)"
fi
echo ""
echo "C550: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
