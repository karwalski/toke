#!/usr/bin/env bash
# C089_block_shadow_and_wide_struct_check.sh — two false checker diagnostics
# on programs codegen handles (stories 127.206, 127.205).
#
# 127.206. `let s="abc"; if(1<2){let s=@(1;2);..}; io.println("outer \(s)")`
# drew E4032 (and `io.println(s)` E4031): the checker took the nearest
# preceding binding anywhere in the function, including one inside a block
# that had closed.  Codegen restores the outer binding on block exit.
#
# 127.205. A struct with more than 64 fields failed --check with
# E4025 "struct 'big' has no field 'f64'": the checker collected fields into a
# 64-slot array (and the .tki type loader had the same cap), while codegen
# handles any width since 127.190.
#
# Each program must --check clean AND run to the output that proves codegen
# agrees.  Case 3 is the positive guard: a binding in force is still used.
#
# Negative control: TKC built from 84e1f4d — cases 1a, 1b and 2 are rejected
# (6 of 7 red: every check and every run of 1a/1b/2).
#
# Stories: 127.206, 127.205

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

cat > "${WORK}/p1a.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{ let s="abc"; if(1<2){let s=@(1;2); io.println("\(s.len)")}; io.println("outer \(s)"); <0 };
EOF
expect "1a-interp-after-block-checks" "$(errs "${WORK}/p1a.tk")" ""
expect "1a-interp-after-block-runs"   "$(runs "${WORK}/p1a.tk")" "2 outer abc "

cat > "${WORK}/p1b.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{ let s="abc"; if(1<2){let s=@(1;2); io.println("\(s.len)")}; io.println(s); <0 };
EOF
expect "1b-arg-after-block-checks" "$(errs "${WORK}/p1b.tk")" ""
expect "1b-arg-after-block-runs"   "$(runs "${WORK}/p1b.tk")" "2 abc "

python3 - "${WORK}/p2.tk" <<'EOF'
import sys
n = 70
fields = ';'.join(f'f{i}:i64' for i in range(n))
inits = ';'.join(f'f{i}:{i}' for i in range(n))
open(sys.argv[1], 'w').write(
    f"m=t;\ni=io:std.io;\nt=$big{{{fields}}};\n"
    f"f=main():i64{{ let b=$big{{{inits}}}; io.println(\"\\(b.f64) \\(b.f69)\"); <0 }};\n")
EOF
expect "2-seventy-field-struct-checks" "$(errs "${WORK}/p2.tk")" ""
expect "2-seventy-field-struct-runs"   "$(runs "${WORK}/p2.tk")" "64 69 "

cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{ let s="abc"; if(1<2){let s=@(1;2); io.println("\(s)")}; <0 };
EOF
expect "3-inner-binding-still-in-force" "$(errs "${WORK}/p3.tk")" "E4032 "

echo "C089: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
