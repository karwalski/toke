#!/usr/bin/env bash
# C1905_forged_handle_refused.sh — an INTEGER other than the literal 0 where
# an opaque std handle is declared (process's `handle`, vechandle, strbuf,
# ...), or where the runtime reads a C string, is E4031 at --check with NO
# `fix` (story 138.35, owner answer T-67 "checker and runtime").
#
# THE DEFECT (loke handoff 2026-11-15, T-67).  `process.wait(5)` passed
# --check, built, and died with SIGSEGV: 138.21 refused a str, float or array
# in a handle slot but let any integer through (136.36: the ABI value IS the
# i64).  A helper `f=waitfor(h:i64)` passed any integer on the same way, and
# `str.toupper(42)` -- a `gen:"abi"` member whose glue reads a C string --
# did the same.  Now an integer reaching such a slot is refused unless it is
# the literal `0`, the null handle (`$err:e 0`), which the runtime answers
# -1 / `$err`.  The check covers std calls and user parameters declared
# `$handle` / `process.$handle` (same module and imported).  A forged value
# the checker cannot see (`n as $handle`, a value through C) is the runtime
# half, 138.36.
#
# Negative control: TKC built from toke main b518b41c, and from v3.2.0
# (ea7ddf22) -- cases 1-12 and 21 red (--check rc 0), 13-20 and 22 green.
#
# Story: 138.35

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
H='m=fh;i=io:std.io;i=s:std.str;i=process:std.process;i=vec:std.vec;
f=waith(h:$handle):i64{<process.wait(h) as i64};
f=waitq(h:process.$handle):i64{<process.wait(h) as i64};
f=asint(x:i64):i64{<x};'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <expected> <got> fix=<count of fix fields>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/fh.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/fh.tk" 2>&1)"; rc=$?
    local code exp got nfix
    code="$(printf '%s' "$out" | grep -o '"error_code":"[^"]*"' | head -1 | cut -d'"' -f4)"
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    nfix="$(printf '%s' "$out" | grep -o '"fix"' | wc -l | tr -d ' ')"
    echo "rc=${rc} ${code} ${exp} ${got} fix=${nfix}"
}
# run <body> — build and run with --allow-run; output on one line.
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/fh.tk"
    "${TKC}" -o "${WORK}/b" "${WORK}/fh.tk" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" --allow-run 2>/dev/null | tr '\n' ' '
}
SPAWN='mt process.spawn(@("/bin/echo";"hi")) {$ok:v v;$err:e 0}'

# 1 — the filed repro (T-67 repro.tk): an integer literal into wait
expect "1-int-literal-into-wait" \
  "$(chk 'let w=process.wait(5);')" "rc=1 E4031 handle i64 fix=0"
# 2 — process.stdout(7), the second filed form
expect "2-int-literal-into-stdout" \
  "$(chk 'let o=mt process.stdout(7) {$ok:v v;$err:e ""};')" "rc=1 E4031 handle i64 fix=0"
# 3 — the shape loke writes (T-67 laundered.tk): a helper `h:i64` passes
#     its parameter on; refused inside the helper, where it reaches wait
H0="${H}"; H="${H}
f=waitfor(h:i64):i64{<process.wait(h) as i64};"
expect "3-helper-i64-param" \
  "$(chk 'let w=waitfor(5);')" "rc=1 E4031 handle i64 fix=0"
H="${H0}"
# 4 — arithmetic: a handle laundered through an integer
expect "4-arithmetic" \
  "$(chk "let h=${SPAWN}; let n=h as i64; let w=process.wait(n+1);")" "rc=1 E4031 handle i64 fix=0"
# 5 — `as i64` written in the argument
expect "5-as-i64" \
  "$(chk "let h=${SPAWN}; let w=process.wait(h as i64);")" "rc=1 E4031 handle i64 fix=0"
# 6 — an i64 local bound to 0 is not the literal 0
expect "6-int-local-zero" \
  "$(chk 'let z=0; let w=process.wait(z);')" "rc=1 E4031 handle i64 fix=0"
# 7 — another opaque std record: vechandle
expect "7-int-into-vechandle" \
  "$(chk 'let n=vec.len(3);')" "rc=1 E4031 vechandle i64 fix=0"
# 8 — another opaque std record: strbuf
expect "8-int-into-strbuf" \
  "$(chk 'let d=s.add(5;"x");')" "rc=1 E4031 strbuf i64 fix=0"
# 9 — a user parameter declared $handle
expect "9-user-param-handle" \
  "$(chk 'let w=waith(5);')" "rc=1 E4031 handle i64 fix=0"
# 10 — a user parameter declared process.$handle
expect "10-user-param-qualified" \
  "$(chk 'let w=waitq(asint(3));')" "rc=1 E4031 handle i64 fix=0"
# 11 — the C-string slot (T-67 abistr.tk): str.toupper(42)
expect "11-int-into-cstring" \
  "$(chk 'let a=s.toupper(42);')" "rc=1 E4031 str i64 fix=0"
# 12 — the C-string slot through a local
expect "12-int-local-into-cstring" \
  "$(chk 'let n=12345; let a=s.reverse(n);')" "rc=1 E4031 str i64 fix=0"
# 13 — control: the literal 0 (the null handle) in every slot above
expect "13-control-literal-zero" \
  "$(chk 'let w=process.wait(0); let n=vec.len(0); let a=waith(0); let b=waitq(0); let r=s.reverse(0);')" \
  "rc=0    fix=0"
# 14 — control: wait(0) still answers -1 (T-67 zero.tk)
expect "14-control-zero-runs" \
  "$(run 'io.println("w0=\(process.wait(0))");')" "w0=-1 "
# 15 — control: a real handle, to std and through $handle parameters
expect "15-control-real-handle-check" \
  "$(chk "let h=${SPAWN}; let w=process.wait(h); let a=waith(h); let b=waitq(h);")" \
  "rc=0    fix=0"
expect "16-control-real-handle-runs" \
  "$(run "let h=${SPAWN}; io.println(\"w=\\(waith(h))\");")" "w=0 "
# 17 — control: a handle where an integer is declared stays clean (136.36)
expect "17-control-handle-into-i64" \
  "$(chk "let h=${SPAWN}; let n=asint(h);")" "rc=0    fix=0"
# 18 — control: real vec handle and strbuf
expect "18-control-real-vec-strbuf" \
  "$(chk 'let v=vec.new(); let n=vec.len(v); let b=s.buf(); s.add(b;"x");')" "rc=0    fix=0"
# 19 — control: a str in the C-string slot
expect "19-control-str-into-cstring" \
  "$(chk 'let a=s.toupper("x");')" "rc=0    fix=0"
# 20 — control: 138.21's refusals unchanged (a str into a $handle parameter)
expect "20-control-str-into-handle-param" \
  "$(chk 'let w=waith("abc");')" "rc=1 E4031 handle str fix=0"

# 21/22 — an imported user module's $handle parameter (127.378 path)
cat > "${WORK}/a_lib.tk" <<'EOF'
m=c1905.lib;
i=process:std.process;
f=waitfor(h:$handle):i64{<process.wait(h) as i64};
EOF
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
imp() {
    printf 'm=c1905.main;\ni=l:c1905.lib;\ni=process:std.process;\nf=main():i64{\n%s\n<0};\n' "$1" > "${WORK}/imp.tk"
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check imp.tk 2>&1)"; rc=$?
    local exp got
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${exp} ${got}"
}
expect "21-imported-handle-param" "$(imp 'let w=l.waitfor(9);')" "rc=1 handle i64"
expect "22-control-imported-zero-and-real" \
  "$(imp "let a=l.waitfor(0); let h=${SPAWN}; let b=l.waitfor(h);")" "rc=0  "

echo "C1905: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
