#!/usr/bin/env bash
# C900_lint_mut_drop_keeps_type.sh — the mutable-never-mutated fix deletes
# `mut.` only when the checker types the `mut`-less program identically
# (story 131.104).
#
# THE DEFECT, on toke 06f2b0b4 (and main 7758dcf3): the fix deleted `mut.` as
# plain text, but the checker types a binding by its mutability
# (types.c bind_init_type): an int/float/bool literal, a negated literal and a
# std call's adopted return type an IMMUTABLE let only (127.229, 127.230,
# 127.248, 138.3).  `let x=mut.5; g(x)` into a u64 parameter checks clean and
# prints 5; `--lint --fix` rewrote it to `let x=5;` and the result was
# E4031 expected 'u64', got 'i64' -- a compiling program turned into a
# non-compiling one.  AGENTS §6: a fix is populated only when it is correct
# in every case.
#
# NOW: --lint re-checks the program with the `mut.` deleted (main.c
# lint_recheck; with every deletion already accepted in the same pass) and
# the fix is offered only when that program type-checks cleanly, the two
# ASTs are typed node for node identically (tk_type_identical, or both
# unknown), the name is bound once in the function, and at least one AST
# reference shows the binding's type (an interpolation-only use does not).
#
# Negative control (measured 2026-11-09): with the check bypassed (the fix
# offered whenever `mut.` is found, as before 131.104) 1b 1c 1d 1e 2 3 4 5 6
# and "10 r" fail -- 9 passed / 10 failed.
#
# Story: 131.104, 127.558

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
# 127.558: a build's alarm is a hang guard only -- 30 s killed slow builds on a
# loaded host; a timeout is reported as such, not as "build-failed".
BUILD_SECS="${TK_CONFORM_BUILD_SECS:-600}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# fix_for FILE NAME -> "fix" / "<none>" (hint without fix) / "<no-hint>"
fix_for() {
    "${TKC}" --lint "$1" 2>/dev/null | awk -v n="$2" '
        index($0, "\"rule\":\"mutable-never-mutated\"") && index($0, "binding '\''" n "'\''") {
            if (index($0, "\"fix\":")) print "fix"; else print "<none>";
            found=1; exit }
        END { if (!found) print "<no-hint>" }'
}
# out FILE -> the program's output and exit status, or "build-failed"
out() {
    local rc=0
    "${RUN}" "${BUILD_SECS}" "${TKC}" "$1" -o "$1.bin" >/dev/null 2>&1 || rc=$?
    [ "${rc}" -eq 142 ] && { echo "build-timed-out-after-${BUILD_SECS}s (a hang)"; return; }
    [ "${rc}" -eq 0 ] || { echo "build-failed"; return; }
    "${RUN}" 20 "$1.bin" 2>&1; echo "rc=$?"
}

# ── 1. the 131.104 repro ────────────────────────────────────────────────────
cat > "${WORK}/r.tk" <<'EOF'
m=t;
i=io:std.io;
f=g(n:u64):u64{<n};
f=main():i64{let x=mut.5; let r=g(x); io.println("\(r)"); <0};
EOF
"${TKC}" --check "${WORK}/r.tk" >/dev/null 2>&1
expect "1a the repro checks clean" "$?" "0"
expect "1b let x=mut.5 into a u64 parameter: hint, no fix" "$(fix_for "${WORK}/r.tk" x)" "<none>"
cp "${WORK}/r.tk" "${WORK}/r_fx.tk"
"${TKC}" --lint --fix "${WORK}/r_fx.tk" >/dev/null 2>&1
expect "1c --fix keeps the mut" "$(grep -c 'let x=mut.5;' "${WORK}/r_fx.tk" | tr -d ' ')" "1"
"${TKC}" --check "${WORK}/r_fx.tk" >/dev/null 2>&1
expect "1d the --fix output still checks" "$?" "0"
expect "1e and still prints 5" "$(out "${WORK}/r_fx.tk")" "$(printf '5\nrc=0')"
# What the old fix produced, by hand: E4031.
sed 's/let x=mut.5;/let x=5;/' "${WORK}/r.tk" > "${WORK}/r_old.tk"
expect "1f the hand-applied old fix is E4031 (the defect)" \
       "$("${TKC}" --check "${WORK}/r_old.tk" 2>&1 | grep -c E4031 | tr -d ' ')" "1"

# ── 2-5. bindings typed differently by mutability: no fix ───────────────────
cat > "${WORK}/t.tk" <<'EOF'
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a=@(1;2;3);
  let k=mut.5;
  let d=a.len-k;
  let f=mut.2.5;
  let h=f*2.0;
  let b=mut.true;
  let u=mut.s.trim(" a ");
  io.println(u);
  io.println("\(d) \(h) \(b)");
  <0
};
EOF
"${TKC}" --check "${WORK}/t.tk" >/dev/null 2>&1
expect "2a the program checks clean" "$?" "0"
expect "2  int literal beside a u64 (a.len-k): no fix"  "$(fix_for "${WORK}/t.tk" k)" "<none>"
expect "3  float literal (untyped vs f64): no fix"      "$(fix_for "${WORK}/t.tk" f)" "<none>"
expect "4  bool literal used only in \\(..): no fix"    "$(fix_for "${WORK}/t.tk" b)" "<none>"
expect "5  std str call (untyped vs str): no fix"       "$(fix_for "${WORK}/t.tk" u)" "<none>"

# ── 6. a shadowed name: references may be another binding, no fix ──────────
cat > "${WORK}/sh.tk" <<'EOF'
m=t;
f=main():i64{
  let y=mut."a";
  let n=if(1<2){let y="bb";2}el{3};
  <n
};
EOF
expect "6  name bound twice in the function: no fix" "$(fix_for "${WORK}/sh.tk" y)" "<none>"

# ── 7-9. type-invariant bindings keep the fix ───────────────────────────────
cat > "${WORK}/ok.tk" <<'EOF'
m=t;
i=io:std.io;
f=sq(n:i64):i64{<n*n};
f=main():i64{
  let a=mut.sq(3);
  let b=mut.@(1;2;3);
  let c=mut."hi";
  io.println(c);
  io.println(c);
  io.println("\(a+b.get(1))");
  <0
};
EOF
expect "7  user call with a declared return keeps its fix" "$(fix_for "${WORK}/ok.tk" a)" "fix"
expect "8  array literal keeps its fix"                    "$(fix_for "${WORK}/ok.tk" b)" "fix"
expect "9  str literal keeps its fix"                      "$(fix_for "${WORK}/ok.tk" c)" "fix"

# ── 10. every fix this file applies preserves behaviour ────────────────────
for p in r t ok; do
    cp "${WORK}/${p}.tk" "${WORK}/${p}_b.tk"
    "${TKC}" --lint --fix "${WORK}/${p}_b.tk" >/dev/null 2>&1
    expect "10 ${p}: --fix output builds and prints the same" "$(out "${WORK}/${p}_b.tk")" "$(out "${WORK}/${p}.tk")"
done
expect "10b ok: all three mut. prefixes were deleted" "$(grep -c 'mut\.' "${WORK}/ok_b.tk" | tr -d ' ')" "0"

echo "C900: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
