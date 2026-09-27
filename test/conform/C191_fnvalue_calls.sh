#!/usr/bin/env bash
# C191_fnvalue_calls.sh — a call through a function value computes the
# value's result wherever the value came from (story 124.0c).
#
# THE DEFECT, on main at 7c16729: only a local bound DIRECTLY to a closure
# literal (`let g=fn..; g(x)`), a closure applied on the spot, and a local
# bound to `&name` were called indirectly.  Every other call through a
# function value fell through to the plain-call path, found no function of
# that name, and lowered `g(x)` as `x` — a silent wrong value at exit 0:
#     let h=g; h(1)              -> 1        (want 5)
#     f=ap(g:(i64):i64;x){<g(x)} -> x        (a parameter, any caller)
#     let m=mk(4); m(1)           -> 1        (a returned closure)
#     let q=a.get(1); q(3)        -> 3        (a container element)
#     ap(&inc;5)                  -> 5        (a reference through a param)
# The repository recorded one of these as expected output: C163 case 9
# pinned "11" where the program computes 15.
#
# NOW: such a call dispatches on the value — a closure (env+1, low bit set)
# calls its lifted function with the environment, a plain function address
# is called directly — using the signature the checker records on the
# callee.  Every expected value below is derived by hand.  Each program
# builds and runs at -O0 and -O2 and prints `bad=0` when all of its own
# checks hold, and the line naming the first failure otherwise.
#
# Negative control: TKC built from main at 7c16729 — all 6 red: program 1
# exits 139 with no output at -O0 and -O2; program 2 prints six FAIL lines
# (struct-field, closure-fn-param, closure-returns-closure, rep, if-expr,
# mut-assign) and bad=6 at exit 0; program 3 does not build (E9003, clang
# rejects "'%t24' defined with type 'i64'" — the shadowed name was called
# through the literal's signature).
#
# Story: 124.0c

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
runs() {   # FILE OPT -> stdout (one line) + rc
    if "${TKC}" "$2" "$1" -o "${WORK}/b" >"${WORK}/build.log" 2>&1; then
        "${WORK}/b" 2>/dev/null | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"
    else echo "BUILD-FAILED $(awk '/"error_code"/' "${WORK}/build.log" | head -1 | cut -c1-200)"; fi
}

cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=inc(x:i64):i64{<x+1};
f=ap(g:(i64):i64;x:i64):i64{<g(x)};
f=aps(g:(i64):str;x:i64):str{<g(x)};
f=apf(g:(f64):f64;x:f64):f64{<g(x)};
f=apb(g:(i64):bool;x:i64):bool{<g(x)};
f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=keep(g:(i64):i64):(i64):i64{<g};
f=compose(f1:(i64):i64;f2:(i64):i64):(i64):i64{<fn(x:i64):i64{<f2(f1(x))}};
f=ck(name:str;got:i64;want:i64):i64{
  if(got==want){<0};
  io.println("FAIL \(name) got=\(got) want=\(want)");
  <1
};
f=main():i64{
  let bad=mut.0;
  let n=4;
  let g=fn(x:i64):i64{<x+n};
  let h=g;
  bad=bad+ck("copy";h(1);5);
  bad=bad+ck("pass";ap(g;1);5);
  bad=bad+ck("pass-literal";ap(fn(x:i64):i64{<x*3};5);15);
  let m=mk(4);
  bad=bad+ck("return";m(1);5);
  bad=bad+ck("pass-returned";ap(mk(10);1);11);
  let k=fn(x:i64):i64{<g(x)*2};
  bad=bad+ck("capture";k(1);10);
  let kp=keep(g);
  bad=bad+ck("returned-param";kp(2);6);
  let kk=10;
  let a=@(mk(1);fn(x:i64):i64{<x*kk});
  let q=a.get(1);
  bad=bad+ck("array-elem";q(3);30);
  bad=bad+ck("fref-param";ap(&inc;5);6);
  bad=bad+ck("bare-name-param";ap(inc;5);6);
  let r=&inc;
  let r2=r;
  bad=bad+ck("fref-copy";r2(1);2);
  let c=compose(g;mk(2));
  bad=bad+ck("compose";c(1);7);
  let c2=compose(&inc;c);
  bad=bad+ck("compose-fref";c2(1);8);
  let sf=fn(x:i64):str{<"v\(x)"};
  let sv=aps(sf;3);
  if(sv!="v3"){ io.println("FAIL str-param \(sv)"); bad=bad+1 };
  let kf=2.5;
  let ff=fn(x:f64):f64{<x*kf};
  let fv=apf(ff;2.0);
  if(fv!=5.0){ io.println("FAIL f64-param"); bad=bad+1 };
  let bf=fn(x:i64):bool{<x>n};
  if(!apb(bf;5)){ io.println("FAIL bool-param"); bad=bad+1 };
  if(apb(bf;3)){ io.println("FAIL bool-param-2"); bad=bad+1 };
  let s=mut.0;
  lp(let i=0;i<1000;i=i+1){
    let t=mk(i);
    s=s+t(1)+ap(t;0)+ap(fn(x:i64):i64{<x+i};0)+ap(mk(i);0);
  };
  bad=bad+ck("loop";s;4*499500+1000);
  io.println("bad=\(bad)");
  <0
};
EOF
for o in -O0 -O2; do expect "1-flows ${o}" "$(runs "${WORK}/p1.tk" "${o}")" "bad=0 rc=0"; done

cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
t=$h{f:(i64):i64;n:i64};
f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=rep(g:(i64):i64;n:i64;x:i64):i64{ if(n==0){<x}; <rep(g;n-1;g(x)) };
f=main():i64{
  let bad=mut.0;
  let s=$h{f:mk(3);n:1};
  let sf=s.f;
  if(sf(1)!=4){ io.println("FAIL struct-field"); bad=bad+1 };
  let g=mk(10);
  let k=fn(q:(i64):i64):i64{<q(1)};
  if(k(g)!=11){ io.println("FAIL closure-fn-param"); bad=bad+1 };
  let adder=fn(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
  let a5=adder(5);
  if(a5(1)!=6){ io.println("FAIL closure-returns-closure"); bad=bad+1 };
  if(rep(mk(2);100;0)!=200){ io.println("FAIL rep"); bad=bad+1 };
  let c=true;
  let ie=if(c){mk(1)}el{mk(2)};
  if(ie(1)!=2){ io.println("FAIL if-expr"); bad=bad+1 };
  let v=mut.mk(0);
  let tot=mut.0;
  lp(let i=0;i<100;i=i+1){ v=mk(i); tot=tot+v(0) };
  if(tot!=4950){ io.println("FAIL mut-assign \(tot)"); bad=bad+1 };
  io.println("bad=\(bad)");
  <0
};
EOF
for o in -O0 -O2; do expect "2-more-flows ${o}" "$(runs "${WORK}/p2.tk" "${o}")" "bad=0 rc=0"; done

# 3. a later binding of the same name shadows a closure-bound local: the
#    call must use the NEW value (a returned closure), not the literal's
#    lifted function nor its signature.
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=main():i64{
  let g=fn(x:i64):i64{<x*100};
  let a=g(1);
  let g=mk(7);
  io.println("\(a) \(g(1))");
  <0
};
EOF
for o in -O0 -O2; do expect "3-shadowed-closure-local ${o}" "$(runs "${WORK}/p3.tk" "${o}")" "100 8 rc=0"; done

echo "C191: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
