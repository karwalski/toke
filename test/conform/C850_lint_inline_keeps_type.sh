#!/usr/bin/env bash
# C850_lint_inline_keeps_type.sh — the single-use-let fix inlines only when
# the inlined expression is the same typed value the binding was (story 131.59).
#
# THE DEFECT, on main at 7758dcf3: the fix was AST-only, so it inlined any
# pure initialiser.  A `let` is not type-transparent in toke:
#   * `let count=segs.len` binds an UNTYPED (i64) local -- bind_init_type
#     adopts no scalar field type -- while `segs.len` is u64.  On corpus
#     record D-WEB-0006v87 the fix turned `right>=count` into
#     `right>=segs.len`, E4031 before 127.53's widening (131.14 run);
#   * on arithmetic it is a different program, not a compile error:
#     `let n=a.len; n-4` is -1, `a.len-4` is RT002 (unsigned overflow);
#   * a numeric literal is typed by its slot: `let k=5; a.len-k` is -2,
#     `a.len-5` is RT002 (the literal narrows to u64, binop_is_unsigned);
#   * `let c:i64=a.len` states a type the expression does not have.
# AGENTS §6: a fix is populated only when it is correct in every case.
#
# NOW: --lint type-checks the file silently first (src/main.c lint_typecheck)
# and the fix is offered only when the checker typed the USE and the
# INITIALISER identically (tk_type_identical, TY_UNKNOWN never identical),
# the let is unannotated (a str/bool literal, typed by its token, also where
# the checker infers no use type, e.g. a method receiver), and a numeric
# literal lands only beside an
# i64 (f64) operand or as `<` of a function declared exactly i64 (f64).  A
# file that does not type-check gets the hint without a fix.
#
# Negative control: TKC=<main's toke at 7758dcf3> -- 1b 1c 2d 3 5 6 fail (a
# fix is present; measured 10 passed / 6 failed).  Case 4 passes there too:
# the old initialiser scan stopped at the annotation's `:` by accident, and
# the case keeps that from regressing now that it is a stated rule.
#
# Load (127.558): the four builds (2b/2c, 10) ran under a 20 s alarm, so on
# a host at load 60-250 a compile that was merely slow was killed, its binary
# was missing and the case failed as "rc=127" -- 4/17 runs on one tree.  A
# build now has BUILD_SECS (default 600, TK_CONFORM_BUILD_SECS overrides), which
# only a hang reaches, and a build that does not produce its binary fails by
# name -- "timed out after N s" (SIGALRM, rc 142) or "failed rc=N" -- rather
# than as a wrong result further down.
#
# Story: 131.59, 127.558

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
BUILD_SECS="${TK_CONFORM_BUILD_SECS:-600}"

# build LABEL SRC OUT -- compile SRC to OUT under the hang guard; a build that
# times out or fails is a named FAIL here, not a missing binary later.
build() {
    "${RUN}" "${BUILD_SECS}" "${TKC}" "$2" -o "$3" >"${WORK}/build.err" 2>&1
    local rc=$?
    if [ "${rc}" -eq 142 ]; then
        echo "FAIL $1: build of $(basename "$2") timed out after ${BUILD_SECS} s (a hang, not load)"
        FAIL=$((FAIL + 1)); return 1
    elif [ "${rc}" -ne 0 ] || [ ! -x "$3" ]; then
        echo "FAIL $1: build of $(basename "$2") failed rc=${rc}: $(head -c 300 "${WORK}/build.err")"
        FAIL=$((FAIL + 1)); return 1
    fi
}

# fix_for FILE NAME -> the replacement of the single-use-let hint on NAME,
# "<none>" when the hint has no fix, "<no-hint>" when there is no hint.
fix_for() {
    "${TKC}" --lint "$1" 2>/dev/null | awk -v n="$2" '
        index($0, "\"rule\":\"single-use-let\"") && index($0, "binding '\''" n "'\''") {
            if (match($0, /"replacement":"[^"]*"/)) print substr($0, RSTART+15, RLENGTH-16);
            else print "<none>";
            found=1; exit }
        END { if (!found) print "<no-hint>" }'
}

# ── 1. the repro, corpus record D-WEB-0006v87 verbatim ──────────────────────
cat > "${WORK}/web.tk" <<'EOF'
m=pathsegment;
i=io:std.io;
i=s:std.str;
f=segments(left:str):@str{
  let parts=s.split(left;"/");
  let out=mut.@();
  lp(let i=0;i<parts.len;i=i+1){
    if(s.len(parts.get(i))>0){
      out=out.append(parts.get(i));
    };
  };
  <out
};
f=segmentat(left:str;right:i64):str{
  let segs=segments(left);
  let count=segs.len;
  <if(right<0||right>=count){""}el{segs.get(right)}
};
f=main():i64{
  io.println(segmentat("/api/users/42";2));
  io.println(segmentat("/home";0));
  io.println(segmentat("/a/b/c";1));
  <0
};
EOF
"${TKC}" --check "${WORK}/web.tk" >/dev/null 2>&1
expect "1a D-WEB-0006v87 checks clean" "$?" "0"
expect "1b let count=segs.len: hint, no fix" "$(fix_for "${WORK}/web.tk" count)" "<none>"
cp "${WORK}/web.tk" "${WORK}/web_fx.tk"
"${TKC}" --lint --fix "${WORK}/web_fx.tk" >/dev/null 2>&1
expect "1c --fix keeps the binding" "$(grep -c 'let count=segs.len;' "${WORK}/web_fx.tk" | tr -d ' ')" "1"

# ── 2-5. type-changing inlines carry no fix ─────────────────────────────────
cat > "${WORK}/t.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let a=@(1;2;3);
  io.println("x");
  let k=5;
  let e=a.len-k;
  let c:i64=a.len;
  let g=c-4;
  let one=1;
  let arr=@(one;a.len);
  <e+g+arr.len
};
EOF
"${TKC}" --check "${WORK}/t.tk" >/dev/null 2>&1
expect "2a the program checks clean" "$?" "0"
# The arithmetic shape, where the inline is a different program at run time.
cat > "${WORK}/n.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let a=@(1;2;3);
  let m2=a.len;
  let d=m2-4;
  io.println("\(d)");
  <0
};
EOF
sed 's/^  let m2=a.len;$//; s/let d=m2-4;/let d=a.len-4;/' "${WORK}/n.tk" > "${WORK}/n_inl.tk"
build "2b" "${WORK}/n.tk"     "${WORK}/n_bin"
build "2c" "${WORK}/n_inl.tk" "${WORK}/n_inl"
expect "2b with the binding: prints -1"            "$("${WORK}/n_bin" 2>&1; echo "rc=$?")" "$(printf -- '-1\nrc=0')"
expect "2c hand-inlined a.len-4: RT002, a different program" \
       "$("${WORK}/n_inl" 2>&1 | grep -c RT002 | tr -d ' ')" "1"
expect "2d let m2=a.len into m2-4 (untyped local vs u64): no fix" "$(fix_for "${WORK}/n.tk" m2)" "<none>"
expect "3  let k=5 beside a u64 (a.len-k): no fix"      "$(fix_for "${WORK}/t.tk" k)"   "<none>"
expect "4  annotated let c:i64=a.len: no fix"           "$(fix_for "${WORK}/t.tk" c)"   "<none>"
expect "5  literal into an array-literal element: no fix" "$(fix_for "${WORK}/t.tk" one)" "<none>"

# ── 6. a file that does not type-check: no type answer, so no fix ──────────
cat > "${WORK}/bad.tk" <<'EOF'
m=t;
f=main():i64{
  let x=3;
  let y=x+1;
  <nosuch(y)
};
EOF
expect "6  untypeable file: hint without fix" "$(fix_for "${WORK}/bad.tk" y)" "<none>"

# ── 7-9. controls: identically typed inlines keep their fix ────────────────
cat > "${WORK}/ok.tk" <<'EOF'
m=t;
i=io:std.io;
f=last(a:@i64):i64{
  let w=a.len-1;
  <a.get(w)
};
f=two():i64{
  let r=2;
  <r
};
f=has(c:str):bool{
  let pool="abc";
  <pool.contains(c)
};
f=main():i64{
  let a=@(1;2;3);
  let x=10;
  let j=3;
  let y=x+j;
  io.println("\(y)");
  let hit=has("b");
  io.println("\(hit)");
  <last(a)+two()
};
EOF
expect "7  u64 binding of a u64 expression keeps its fix" "$(fix_for "${WORK}/ok.tk" w)" "  <a.get(a.len-1"
expect "8  literal beside an i64 keeps its fix"           "$(fix_for "${WORK}/ok.tk" j)" "  let y=x+3"
expect "9  literal as <of an i64 function keeps its fix"  "$(fix_for "${WORK}/ok.tk" r)" "  <2"
# (fix_for's extraction stops at the first escaped quote of the JSON string)
expect "9b str literal as a method receiver keeps its fix" "$(fix_for "${WORK}/ok.tk" pool)" '  <\'

# ── 10. every fix this file applies preserves behaviour ────────────────────
for p in web ok; do
    cp "${WORK}/${p}.tk" "${WORK}/${p}_b.tk"
    "${TKC}" --lint --fix "${WORK}/${p}_b.tk" >/dev/null 2>&1
    build "10 ${p}" "${WORK}/${p}.tk"   "${WORK}/${p}_o1"
    build "10 ${p}" "${WORK}/${p}_b.tk" "${WORK}/${p}_o2"
    a="$("${WORK}/${p}_o1" 2>&1; echo "rc=$?")"; b="$("${WORK}/${p}_o2" 2>&1; echo "rc=$?")"
    expect "10 ${p}: --fix output builds and prints the same" "${b}" "${a}"
done

echo "C850: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
