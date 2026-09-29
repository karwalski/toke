#!/usr/bin/env bash
# C410_err_accessors_retired.sh — csv, file and zip report a failure through
# the $err arm's own payload, and the six last-error accessors are gone
# (127.102a, 127.102b, 127.102c; owner decision on 127.102, one release).
#
# THE DEFECT.  These modules' glue signalled failure with a value sentinel and
# never built an error box, so the $err arm bound nothing: `e.msg` was RT005,
# and a match on the variants of FileErr / $ziperr failed to build (E9005,
# "the sum type of the matched value is not established").  The reason lived
# in process-wide accessors — csv.lasterr/lasterrkind/lasterrline,
# file.lasterr/lasterrkind, zip.lasterr — and file.lasterrkind() reported
# `ok` after a failed file.read, because file.read never set it (127.102b).
# The glue now raises a real box through tk_err_box (127.109) and clears the
# slot on success; csverr gained `kind` (line was declared, and always 0);
# FileErr grew from three variants to the ten kinds file.c distinguishes.
#
# Cases:
#   1 zip    — $err binds the $ziperr variant and its reason
#   2 file   — file.read of a missing path: $notfound with call and path
#   3 file   — file.readbytes of a directory: $isdir, a variant FileErr did
#              not have before 127.102b
#   4 csv    — e.kind / e.line / e.msg of a refused row (quote, line 2)
#   5 csv    — e.kind of reading past the end is "eof", not a refusal
#   6 kept   — a let-bound file error keeps its own message across a later
#              failure (the message is copied out of file.c's one static
#              buffer, tk_err_msg)
#   7 cleared — per module, a success after a failure takes the $ok arm
#   8 retired — each of the six accessors is now a compile-time E4027
#
# Negative control: TKC + stdlib from main at 8efba9ea (C410_STDLIB) — 1 passed,
# 12 failed: cases 1, 2, 3, 6 build-failed (E9005: the sum type of the $err
# payload is not established), 4 and 5 build-failed (E4025: csverr has no
# field 'kind'), all six case-8 probes red ("compiled").  Case 7 is green on
# both (the value sentinel already saw these successes).  Case 6 alone: with
# tk_err_msg's ring cut to one slot it reads the SECOND file's message.
#
# Story: 127.102a, 127.102b, 127.102c

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C410_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/adir"
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" >"${WORK}/build.log" 2>&1 || { echo build-failed; return; }
        (cd "${WORK}" && ./b --allow-all 2>&1) | tr '\n' '|'; }

# The ten FileErr variants, as "variant|message".  FileErr cannot be named in
# a parameter (an upper-case .tki type), so the match is spelled where the
# $err arm binds it.
FK='mt e {$notfound:m s.concat("notfound|";m);$permission:m s.concat("permission|";m);$io:m s.concat("io|";m);$invalid:m s.concat("invalid|";m);$isdir:m s.concat("isdir|";m);$notregular:m s.concat("notregular|";m);$symlink:m s.concat("symlink|";m);$toolarge:m s.concat("toolarge|";m);$nomem:m s.concat("nomem|";m);$badarg:m s.concat("badarg|";m)}'

expect "1-zip-variant-and-reason" "$(run 'm=main;i=io:std.io;i=zip:std.zip;i=s:std.str;
f=main():i64{
  io.println(mt zip.open(s.bytes("not an archive")) {$ok:z "ok";$err:e mt e {$badarchive:m s.concat("badarchive|";m);$badentry:m m;$notfound:m m;$toolarge:m m;$unsupported:m m;$io:m m}});
  <0
};')" "badarchive|zip: not a zip archive, or its central directory is corrupt|"

expect "2-file-read-notfound" "$(run "m=main;i=io:std.io;i=file:std.file;i=s:std.str;
f=main():i64{
  io.println(mt file.read(\"nosuch.txt\") {\$ok:t \"ok\";\$err:e ${FK}});
  <0
};")" "notfound|file.read: file not found: nosuch.txt|"

expect "3-file-readbytes-isdir" "$(run "m=main;i=io:std.io;i=file:std.file;i=s:std.str;
f=main():i64{
  io.println(mt file.readbytes(\"adir\") {\$ok:t \"ok\";\$err:e ${FK}} );
  <0
};" | cut -d'|' -f1)" "isdir"

printf 'a,b\n"unterminated,2\n' > "${WORK}/bad.csv"
expect "4-csv-kind-line-msg" "$(run 'm=main;i=io:std.io;i=csv:std.csv;i=file:std.file;i=s:std.str;
f=main():i64{
  let data=mt file.readbytes("bad.csv") {$ok:b b;$err:e s.bytes("")};
  let o=csv.opts(",";"\"";"utf-8";"strip";"error";true);
  let rd=mt csv.readeropts(data;o) {$ok:r r;$err:e <1};
  let hdr=csv.header(rd);
  io.println(mt csv.next(rd) {$ok:row "ok";$err:e s.concat(s.concat(s.concat(e.kind;"|");s.fromint(e.line as i64));s.concat("|";if(s.len(e.msg)>0){"msg"}el{"empty"}))});
  <0
};')" "quote|2|msg|"

expect "5-csv-eof-kind" "$(run 'm=main;i=io:std.io;i=csv:std.csv;i=s:std.str;
f=main():i64{
  let rd=csv.reader(s.bytes("x\n");44 as u8);
  let a=csv.next(rd);
  io.println(mt csv.next(rd) {$ok:row "ok";$err:e e.kind});
  <0
};')" "eof|"

expect "6-kept-message-survives" "$(run "m=main;i=io:std.io;i=file:std.file;i=s:std.str;
f=main():i64{
  let a=file.read(\"first-missing.txt\");
  let b=file.read(\"second-missing.txt\");
  io.println(mt a {\$ok:t \"ok\";\$err:e ${FK}});
  <0
};")" "notfound|file.read: file not found: first-missing.txt|"

printf 'hello' > "${WORK}/ok.txt"
expect "7-cleared-after-failure" "$(run 'm=main;i=io:std.io;i=file:std.file;i=zip:std.zip;i=csv:std.csv;i=s:std.str;
f=main():i64{
  let f1=mt file.read("nosuch.txt") {$ok:t "ok";$err:e "err"};
  let f2=mt file.read("ok.txt") {$ok:t t;$err:e "err"};
  let z1=mt zip.open(s.bytes("x")) {$ok:z "ok";$err:e "err"};
  let c1=mt csv.parse(s.bytes("")) {$ok:r "ok";$err:e "err"};
  let c2=mt csv.parse(s.bytes("a,b\n")) {$ok:r "ok";$err:e "err"};
  let f3=mt file.size("ok.txt") {$ok:n s.fromint(n);$err:e "err"};
  io.println(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(f1;" ");f2);" ");z1);" ");c1);" ");c2);" ");f3));
  <0
};')" "err hello err err ok 5|"

# 8. The accessors are retired: each is now "no member" at compile time.
for acc in "csv.lasterr" "csv.lasterrkind" "csv.lasterrline" "file.lasterr" "file.lasterrkind" "zip.lasterr"; do
    mod="${acc%%.*}"
    printf 'm=main;i=io:std.io;i=%s:std.%s;\nf=main():i64{\n  let x=%s();\n  <0\n};\n' "$mod" "$mod" "$acc" > "${WORK}/r.tk"
    if "${TKC}" --allow-all "${WORK}/r.tk" -o "${WORK}/r" > "${WORK}/r.log" 2>&1; then got="compiled"
    elif grep -q '"error_code":"E4027"' "${WORK}/r.log"; then got="E4027"
    else got="other: $(head -c 160 "${WORK}/r.log")"; fi
    expect "8-retired-${acc}" "${got}" "E4027"
done

echo "C410: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
