#!/usr/bin/env bash
# C720_lasterr_accessors_retired_ocr_pdf_xlsx.sh — ocr, pdf and xlsx report a
# failure through the $err arm's own payload only; the five last-error
# accessors that duplicated it are gone (127.246 (3); owner decision
# 2026-10-01 §3.3, matching 127.102's ruling for csv/file/zip — see C410).
#
# THE DUPLICATE.  127.241 made every T!$ocrerr / T!$pdferr / T!$xlsxerr
# wrapper raise a {tag, msg} box, so the $err arm binds the variant and its
# message.  ocr.lasterr/lasterrkind, pdf.lasterr/lasterrkind and xlsx.lasterr
# stayed exported beside it, carrying the same two facts through process-wide
# state — two channels that a later change could let disagree.
#
# Cases:
#   1-5 retired  — each of the five accessors is a compile-time E4027
#   6   pdf      — $err binds $baddoc and the reader's message
#   7   xlsx     — $err binds $badworkbook and a message
#   8   ocr      — $err binds a variant: $badimage where a recogniser exists,
#                  $noengine where none does — never "ok"
#
# Negative control: TKC + stdlib from main at c2aa3a7a (C720_STDLIB) — cases
# 1-5 red ("compiled"); 6-8 green on both (the payload predates this story).
#
# Story: 127.246

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C720_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" >"${WORK}/build.log" 2>&1 || { echo build-failed; return; }
        (cd "${WORK}" && ./b --allow-all 2>&1) | tr '\n' '|'; }

# 1-5. The accessors are retired: each is now "no member" at compile time.
n=0
for acc in "ocr.lasterr" "ocr.lasterrkind" "pdf.lasterr" "pdf.lasterrkind" "xlsx.lasterr"; do
    n=$((n + 1))
    mod="${acc%%.*}"
    printf 'm=main;i=io:std.io;i=%s:std.%s;\nf=main():i64{\n  let x=%s();\n  <0\n};\n' "$mod" "$mod" "$acc" > "${WORK}/r.tk"
    if "${TKC}" --allow-all "${WORK}/r.tk" -o "${WORK}/r" > "${WORK}/r.log" 2>&1; then got="compiled"
    elif grep -q '"error_code":"E4027"' "${WORK}/r.log"; then got="E4027"
    else got="other: $(head -c 160 "${WORK}/r.log")"; fi
    expect "${n}-retired-${acc}" "${got}" "E4027"
done

expect "6-pdf-variant-and-message" "$(run 'm=main;i=io:std.io;i=p:std.pdf;i=s:std.str;
f=main():i64{
  io.println(mt p.open(s.bytes("not a pdf")) {$ok:d "ok";$err:e mt e {$baddoc:m s.concat("baddoc|";if(s.len(m)>0){"msg"}el{"empty"});$encrypted:m "encrypted";$nopage:m "nopage";$toolarge:m "toolarge";$unsupported:m "unsupported";$io:m "io"}});
  <0
};')" "baddoc|msg|"

expect "7-xlsx-variant-and-message" "$(run 'm=main;i=io:std.io;i=x:std.xlsx;i=s:std.str;
f=main():i64{
  io.println(mt x.open(s.bytes("not a workbook")) {$ok:b "ok";$err:e mt e {$badworkbook:m s.concat("badworkbook|";if(s.len(m)>0){"msg"}el{"empty"});$badpart:m "badpart";$nosheet:m "nosheet";$toolarge:m "toolarge";$io:m "io"}});
  <0
};')" "badworkbook|msg|"

got8="$(run 'm=main;i=io:std.io;i=o:std.ocr;i=s:std.str;
f=main():i64{
  io.println(mt o.text(s.bytes("not an image")) {$ok:t "ok";$err:e mt e {$noengine:m "noengine";$badimage:m "badimage";$toosmall:m "toosmall";$nolanguage:m "nolanguage";$failed:m "failed";$io:m "io"}});
  io.println(if(o.isavailable()){"engine"}el{"none"});
  <0
};')"
case "${got8}" in
    "badimage|engine|"|"noengine|none|") expect "8-ocr-variant" "ok" "ok" ;;
    *) expect "8-ocr-variant" "${got8}" "badimage|engine| or noengine|none|" ;;
esac

echo "C720: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
