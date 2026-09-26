#!/usr/bin/env bash
# C100_stdlib_sum_error_payload.sh — a stdlib SUM-typed error payload binds,
# and `mt e{$notfound:m …}` dispatches on it (story 127.209).
#
# THE DEFECT.  FileErr and JsonErr were declared in their .tki with a
# `"variants"` key (and five other sums with `"kind":"sum_type"` / `"sum"`),
# a spelling nothing in the compiler reads: register_tki_struct_types() only
# registers `"kind":"type"` records with `"fields"`, and never read `is_sum`.
# So the $err payload of file.size bound nil and
#
#     mt file.size(p){$ok:n …;$err:e mt e{$notfound:m …;$permission:m …;$io:m …}}
#
# failed E9005 "the sum type of the matched value is not established" at
# codegen -- after `--check` had passed it.  The variant names were also
# capitalised (`NotFound`), which the match-arm parser rejects, so the arm
# could not be written at all.
#
# THE FIX.  Every sum in stdlib/*.tki is now spelled the one way the loaders
# read -- `"kind":"type","is_sum":true,"fields":[{"name":"$notfound",…}]` --
# register_tki_struct_types() carries is_sum, and stdlib_err_payload_type()
# binds a sum payload for the wrappers whose glue stores a {tag, msg} box on
# every failure (json_raise, file_raise).  check-tki rejects any other kind
# spelling, so the next one cannot silently bind nil.
#
# WHAT IS ASSERTED.  The glue's own message text through each variant arm,
# direct and let-bound, for std.file and std.json; the ok path still ok.
#
# Story: 127.209

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c100_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C100: stdlib sum-typed error payloads (FileErr, JsonErr) match by variant"
echo "--------------------------------------"

printf 'twelve bytes' > present.txt

cat > p.tk <<'EOF'
m=p;
i=file:std.file;
i=json:std.json;
i=io:std.io;
i=str:std.str;
f=fsz(label:$str;p:$str):i64{
  mt file.size(p){
    $ok:n io.println(str.concat(label;str.concat(": ok ";str.fromint(n))));
    $err:e mt e{$notfound:m io.println(str.concat(label;str.concat(": notfound ";m)));$permission:m io.println(str.concat(label;": permission"));$io:m io.println(str.concat(label;": io"))}
  };
  <0
};
f=main():i64{
  fsz("size missing";"no/such/file");
  fsz("size present";"present.txt");
  let r=file.size("no/such/other");
  mt r{$ok:n io.println("size let: ok");$err:e mt e{$notfound:m io.println(str.concat("size let: notfound ";m));$permission:m io.println("size let: permission");$io:m io.println("size let: io")}};
  mt file.readrange("no/such/file";0;4){$ok:b io.println("readrange: ok");$err:e mt e{$notfound:m io.println("readrange: notfound");$permission:m io.println("readrange: permission");$io:m io.println("readrange: io")}};
  let d=mt json.dec("{\"a\":1}"){$ok:j j;$err:e json.dec("{}")};
  mt json.bool(d;"zz"){$ok:b io.println("json missing: ok");$err:e mt e{$parse:m io.println("json missing: parse");$type:m io.println("json missing: type");$missing:m io.println(str.concat("json missing: missing ";m))}};
  mt json.i64(d;"a"){$ok:v io.println(str.concat("json ok: ok ";str.fromint(v)));$err:e mt e{$parse:m io.println("json ok: parse");$type:m io.println("json ok: type");$missing:m io.println("json ok: missing")}};
  <0
};
EOF

EXPECTED='size missing: notfound file.size: No such file or directory: no/such/file
size present: ok 12
size let: notfound file.size: No such file or directory: no/such/other
readrange: notfound
json missing: missing zz
json ok: ok 1'

if ! "${TKC}" --check p.tk >check.log 2>&1; then
    echo "  FAIL: --check rejected the program"
    head -c 400 check.log | sed 's/^/      /'; echo
    FAIL=$((FAIL + 1))
fi
if ! "${TKC}" --allow-all --out p_bin p.tk >compile.log 2>&1; then
    echo "  FAIL: program did not compile"
    grep -o '"error_code":"[^"]*","severity":"[^"]*","stage":"[^"]*","message":"[^"]*"' compile.log | head -2 | sed 's/^/      /'
    FAIL=$((FAIL + 1))
else
    out="$(./p_bin 2>&1)"; rc=$?
    while IFS= read -r want; do
        key="${want%%: *}: "
        if printf '%s\n' "${out}" | grep -qxF "${want}"; then
            echo "  PASS: ${want}"
            PASS=$((PASS + 1))
        else
            got="$(printf '%s\n' "${out}" | awk -v p="${key}" 'index($0,p)==1' | head -1)"
            echo "  FAIL: expected [${want}], got [${got}]"
            FAIL=$((FAIL + 1))
        fi
    done <<< "${EXPECTED}"
    if [ "${rc}" -ne 0 ]; then
        echo "  FAIL: program exited ${rc}: $(printf '%s\n' "${out}" | tail -1)"
        FAIL=$((FAIL + 1))
    fi
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
