#!/usr/bin/env bash
# C532_user_scalar_error_payloads.sh — a user function's non-struct error
# payload (`T!str`, `T!i64`, `T!bool`, `T!f64`) reaches its `$err` arm
# (story 127.249 item 1).
#
# THE DEFECT.  classify_fallible_call() typed a user error payload only when
# the error type was a struct, so the `$err:e` arm of `mt g() {..}` over a
# `T!str` or `T!i64` function bound e = 0 whatever `<$err(x)` raised.  And a
# scalar payload crossed @tk_current_error as a bare word, which is
# tri-valued (0 ok, 1 payload-less failure, else a payload): `<$err(0)` and
# `<$err(1)` were indistinguishable from each other.
#
# NOW: a str payload binds as the message pointer; a scalar one is raised in
# an 8-byte error box (tk_err_box) and the arm loads it, so 0 and 1 survive,
# through a let binding (the box is kept per binding, 127.103), through `!`
# into a function of the same error type, through a returned match (127.245)
# and across a module boundary.
#
# Negative control: TKC=<main d3a083b8's toke> -- cases 1 and 3 fail (every
# payload prints 0: "err:0 0 0 0 0 0 err:0 err:0 0 0 40 0 103 5" and
# "0 0 e:0").  Case 2 (ok values) is green on both.
#
# Story: 127.249
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
cd "${WORK}" || exit 1
# run SRC [more sources]: build at -O0 and -O2; both must agree.
run() {
    local o out prev=""
    for o in -O0 -O2; do
        rm -f b
        "${TKC}" --allow-all "${o}" -I . --out b "$@" >/dev/null 2>&1 || { echo "build-failed${o}"; return; }
        out="$(./b 2>&1 | tr '\n' '|'; echo "rc=${PIPESTATUS[0]}")"
        if [ -n "${prev}" ] && [ "${prev}" != "${out}" ]; then echo "O-levels differ: ${prev} / ${out}"; return; fi
        prev="${out}"
    done
    echo "${prev}"
}

cat > s1.tk <<'EOF2'
m=s1;
i=io:std.io;
f=gs(n:i64):i64!str{ if(n<0){ <$err("boom\(n)") }; <n };
f=gi(n:i64):i64!i64{ if(n<0){ <$err(n+1) }; <n };
f=gb(n:i64):str!bool{ if(n<0){ <$err(n==0-1) }; <"ok" };
f=gf(n:i64):i64!f64{ if(n<0){ <$err(1.5) }; <n };
f=gp(n:i64):i64!i64{ let v=gi(n)!$err; <v*10 };
f=gm(n:i64):i64!i64{ <mt gi(n) { $ok:v v+100; $err:e $err(e*2) } };
f=main():i64{
  let a=mt gs(0-7) { $ok:v "ok"; $err:e "err:\(e)" };
  let b=mt gi(0-1) { $ok:v 0-99; $err:e e };
  let c=mt gi(0-2) { $err:e e; $ok:v 0-99 };
  let d=mt gi(0-43) { $ok:v 0-99; $err:e e };
  let r1=gi(0-1);
  let r2=gi(0-5);
  let x1=mt r1 { $ok:v 0-99; $err:e e };
  let x2=mt r2 { $ok:v 0-99; $err:e e };
  let g1=mt gb(0-1) { $ok:v "ok"; $err:e "err:\(e)" };
  let g2=mt gb(0-2) { $ok:v "ok"; $err:e "err:\(e)" };
  let f1=mt gf(0-1) { $ok:v 0.0; $err:e e };
  let p1=mt gp(0-3) { $ok:v 0-99; $err:e e };
  let p2=mt gp(4) { $ok:v v; $err:e 0-99 };
  let m1=mt gm(0-3) { $ok:v v; $err:e e };
  let m2=mt gm(3) { $ok:v v; $err:e 0-99 };
  let o=mt gi(5) { $ok:v v; $err:e 0-99 };
  io.println("\(a) \(b) \(c) \(d) \(x1) \(x2) \(g1) \(g2) \(f1) \(p1) \(p2) \(m1) \(m2) \(o)");
  <0
};
EOF2
# $err(0) is 0, $err(-1) is -1 -- neither is the flag; the two let-bound
# payloads are kept apart though both were raised in the thread's one box.
expect "1 str/i64/bool/f64 payloads, direct, let-bound, ! and returned mt" \
    "$(run s1.tk)" "err:boom-7 0 -1 -42 0 -4 err:true err:false 1.5 -2 40 -4 103 5|rc=0"

cat > s2.tk <<'EOF2'
m=s2;
i=io:std.io;
f=gi(n:i64):i64!i64{ if(n<0){ <$err(n) }; <n };
f=gs(n:i64):str!str{ if(n<0){ <$err("x") }; <"v\(n)" };
f=main():i64{
  let a=mt gi(0) { $ok:v v; $err:e 0-99 };
  let b=mt gi(1) { $ok:v v; $err:e 0-99 };
  let c=mt gs(2) { $ok:v v; $err:e "err" };
  io.println("\(a) \(b) \(c)");
  <0
};
EOF2
expect "2 control: ok values of 0 and 1" "$(run s2.tk)" "0 1 v2|rc=0"

cat > lib.tk <<'EOF2'
m=lib;
f=gi(n:i64):i64!i64{ if(n<0){ <$err(n+1) }; <n };
f=gs(n:i64):i64!str{ if(n<0){ <$err("neg") }; <n };
EOF2
"${TKC}" --emit-interface --check -I . lib.tk >/dev/null 2>&1
cat > x.tk <<'EOF2'
m=main;
i=io:std.io;
i=lib:lib;
f=main():i64{
  let a=mt lib.gi(0-1) { $ok:v 0-99; $err:e e };
  let b=mt lib.gi(0-6) { $ok:v 0-99; $err:e e };
  let c=mt lib.gs(0-6) { $ok:v "ok"; $err:e "e:\(e)" };
  io.println("\(a) \(b) \(c)");
  <0
};
EOF2
expect "3 across a module boundary" "$(run x.tk lib.tk)" "0 -5 e:neg|rc=0"

echo "C532: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
