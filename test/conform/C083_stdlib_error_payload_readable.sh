#!/usr/bin/env bash
# C083_stdlib_error_payload_readable.sh — a stdlib error's message reaches the
# toke caller (stories 135.5b and 127.182).
#
# THE DEFECT.  The $err arm's payload was resolved only for USER functions
# (through their FnSig); a stdlib call never had one, so the arm bound nil
# whatever the C glue knew:
#
#   toml   `e.msg` on a load error: E4034 at compile time (tomlerr was declared
#          "kind":"record" in toml.tki, which nothing registers), RT005 at run
#          time once past that; propagating through `!$T` dropped it as well.
#   image  every `T!str` failure stored the payload-less flag 1, so "not a
#          TIFF" and "unsupported compression" were indistinguishable.
#
# THE ABI (runtime-abi.md §7.3).  For a stdlib wrapper whose glue keeps the
# slot truthful (llvm.c is_stdlib_slot_wrapper): a `T!str` error stores the
# message pointer itself in the slot and the arm binds it as the str; a record
# error stores a box of the record's .tki layout.  `!` passes the payload
# through only into an error type of the same layout; anything else is
# re-raised payload-less (a {msg} box read as a sum's {tag, payload} would
# dispatch on a pointer, and a JsonErr {Missing, msg} box read as a user sum
# would select that sum's third variant).
#
# WHAT IS ASSERTED.  The parser's / decoder's OWN message text, not merely a
# non-empty string: direct match, let-bound match, `!` into a same-layout
# record, `!` into a sum (falls through to the last arm, no crash), and the
# success path of each module still taking $ok.
#
# Stories: 135.5b, 127.182

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

WORK="$(mktemp -d /tmp/tkc_c083_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C083: stdlib error payloads (toml record, image str) are readable from toke"
echo "--------------------------------------"

cat > p.tk <<'EOF'
m=p;
i=toml:std.toml;
i=json:std.json;
i=image:std.image;
i=io:std.io;
i=str:std.str;
t=$cerr{msg:$str};
t=$serr{$bad:$str;$worse:i64};
t=$s4{$a:i64;$b:i64;$c:$str;$d:i64};
f=jsum(o:i64):bool!$s4{
  let v=json.bool(o;"nope")!$s4;
  <v
};
f=viarec(s:$str):i64!$cerr{
  let v=toml.load(s)!$cerr;
  <1
};
f=viasum(s:$str):i64!$serr{
  let v=toml.load(s)!$serr;
  <1
};
f=main():i64{
  mt toml.load("x = = 1"){$ok:v io.println("toml direct: ok");$err:e io.println(str.concat("toml direct: ";e.msg))};
  let r=toml.load("= 3");
  mt r{$ok:v io.println("toml let: ok");$err:e io.println(str.concat("toml let: ";e.msg))};
  mt toml.load("a = 1"){$ok:v io.println("toml ok: ok");$err:e io.println(str.concat("toml ok: ";e.msg))};
  mt viarec("x = = 1"){$ok:v io.println("toml via record: ok");$err:e io.println(str.concat("toml via record: ";e.msg))};
  mt viasum("x = = 1"){$ok:v io.println("toml via sum: ok");$err:e mt e{$bad:m io.println("toml via sum: bad");$worse:n io.println("toml via sum: last arm")}};
  let jo=mt json.dec("{\"a\":1}"){$ok:v v;$err:e 0};
  mt jsum(jo){$ok:v io.println("json via foreign sum: ok");$err:e mt e{$a:x io.println("json via foreign sum: a");$b:x io.println("json via foreign sum: b");$c:m io.println("json via foreign sum: c (a JsonErr box read as this sum)");$d:x io.println("json via foreign sum: last arm")}};
  mt image.decode(str.bytes("not an image at all")){$ok:v io.println("image direct: ok");$err:e io.println(str.concat("image direct: ";e))};
  let im=image.decode(str.bytes("abc"));
  mt im{$ok:v io.println("image let: ok");$err:e io.println(str.concat("image let: ";e))};
  mt image.tiffpages(str.bytes("abcd")){$ok:n io.println("tiffpages: ok");$err:e io.println(str.concat("tiffpages: ";e))};
  <0
};
EOF

EXPECTED='toml direct: line 1: syntax error
toml let: line 1: syntax error
toml ok: ok
toml via record: line 1: syntax error
toml via sum: last arm
json via foreign sum: last arm
image direct: image_decode: not a PNG or TIFF file (bad signature)
image let: image_decode: buffer too short to be a valid image
tiffpages: image.tiffpages: not a TIFF file, or its IFD chain is malformed'

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
