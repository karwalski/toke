#!/usr/bin/env bash
# C2041_unimported_record_field_read.sh — --check refuses every field read
# that codegen refuses on a record of a module the file does not import
# (story 138.58, extending 138.40 / loke T-39).
#
# HISTORY.  138.40 made --check answer E4034 for a BARE record a user-module
# call returns (`let r=srv.handle(x); r.text`) when the record's module is
# not imported.  The same record reached through another value still passed
# --check, and the compile refused it with E4034: an element of an array or
# map the call returns (`rs.get(0).text`, in place or bound, in a loop), the
# `$ok` payload of a `T!E`, a sum type's variant payload bound by an `mt`
# arm, and an imported record's field whose type is such a record
# (`wr.r.text`).  With another struct in scope declaring a field of the same
# name, codegen's 136.28 fallback took THAT struct's offset instead: the read
# built and printed an empty string (iserror's slot read as a str).
# Consequence for lint: the 138.39 import-drop re-check could not see an
# import only codegen needs, so unused-import reported `i=p:proto;` and
# --lint --fix deleted it into an E4034 compile.
#
# WHAT IS ASSERTED (proto declares $res{iserror:bool;text:str} and $perr;
# server imports proto and returns $res in each shape)
#   1. each shape, without `i=p:proto;`: --check E4034, the compile E4034,
#      and no `fix` on the --check diagnostic
#   2. each shape WITH `i=p:proto;`: --check clean, builds, prints "a"
#   3. a file that also declares its own struct with a field `text` (at the
#      offset $res has, so the codegen fallback would have built it): E4034
#   4. --lint on each importing file reports no unused-import
#
# Negative control: TKC=<base tkc> TKC_STDLIB_DIR=<base>/src/stdlib fails 1
# for every shape except the bare one (138.40), 3 and 4.
#
# Story: 138.58
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2041_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/src" "${WORK}/ifc" "${WORK}/out"
cd "${WORK}" || exit 1

cat > src/proto.tk <<'TK'
m=proto;
t=$res{iserror:bool;text:str};
t=$perr{msg:str};
TK

cat > src/server.tk <<'TK'
m=server;
i=p:proto;
t=$wrap{r:$res;n:i64};
t=$choice{$one:$res;$none:i64};
f=handle(x:str):$res{<$res{iserror:false;text:x}};
f=many(x:str):@($res){<@($res{iserror:false;text:x})};
f=mp(x:str):@($str:$res){<@(x:$res{iserror:false;text:x})};
f=tried(x:str):$res!$perr{<$res{iserror:false;text:x}};
f=pick(x:str):$choice{<$one($res{iserror:false;text:x})};
f=mkwrap(x:str):$wrap{<$wrap{r:$res{iserror:false;text:x};n:1}};
TK

"${TKC}" -I ifc --emit-interface --emit-llvm --out ifc src/proto.tk >/dev/null 2>&1
"${TKC}" -I ifc --emit-interface --emit-llvm --out ifc src/server.tk >/dev/null 2>&1
if [ ! -f ifc/server.tki ]; then echo "FAIL: server interface did not build"; exit 1; fi

# shape name | body of main
SHAPES='bare|  let r=srv.handle("a");@  io.println(r.text);
arrlet|  let rs=srv.many("a");@  io.println(rs.get(0).text);
arrinplace|  io.println(srv.many("a").get(0).text);
map|  let m=srv.mp("a");@  io.println(m.get("a").text);
errunion|  let v=mt srv.tried("a"){$ok:r r.text;$err:e "e"};@  io.println(v);
sum|  let v=mt srv.pick("a"){$one:r r.text;$none:n "n"};@  io.println(v);
fieldlet|  let wr=srv.mkwrap("a");@  io.println(wr.r.text);
fieldinplace|  io.println(srv.mkwrap("a").r.text);
loop|  let rs=srv.many("a");@  lp(let i=0;i<rs.len;i=i+1){io.println(rs.get(i).text)};'

mk() {   # mk <module> <extra decl lines> <body>
    printf 'm=%s;\ni=io:std.io;\ni=srv:server;\n%s\nf=main():i64{\n%s\n  <0\n};\n' \
        "$1" "$2" "$(printf '%s' "$3" | tr '@' '\n')" > "src/$1.tk"
}
codes() { grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' '; }

while IFS='|' read -r name body; do
    [ -n "${name}" ] || continue
    mk "u${name}" "" "${body}"
    out="$("${TKC}" -I ifc --check "src/u${name}.tk" 2>&1)"
    expect "1-${name}-check" "$(printf '%s' "${out}" | codes)" "E4034 "
    expect "1-${name}-nofix" "$(printf '%s' "${out}" | grep -c '"fix"')" "0"
    expect "1-${name}-compile" "$("${TKC}" -I ifc --emit-llvm --out out "src/u${name}.tk" 2>&1 | codes)" "E4034 "

    mk "v${name}" "i=p:proto;" "${body}"
    expect "2-${name}-check" "$("${TKC}" -I ifc --check "src/v${name}.tk" 2>&1 | codes)" ""
    if "${TKC}" -I ifc --out "out/v${name}" src/proto.tk src/server.tk "src/v${name}.tk" >"out/v${name}.log" 2>&1; then
        expect "2-${name}-run" "$("out/v${name}" 2>&1)" "a"
    else
        expect "2-${name}-run" "build failed: $(codes <"out/v${name}.log")" "a"
    fi
    expect "4-${name}-lint" "$("${TKC}" -I ifc --lint "src/v${name}.tk" 2>&1 | grep -c '"rule":"unused-import"')" "0"
done <<EOF
${SHAPES}
EOF

# 3. another struct in scope declares `text` at the same offset
mk "xsame" 't=$loc{n:i64;text:str};' '  let rs=srv.many("a");@  io.println(rs.get(0).text);'
expect "3-other-struct-check" "$("${TKC}" -I ifc --check src/xsame.tk 2>&1 | codes)" "E4034 "

echo "C2041: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
