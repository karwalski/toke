#!/usr/bin/env bash
# C1940_module_let_global_types.sh — a module-level `let` global of every
# type reads back its value: in a function, in a string interpolation, in
# arithmetic, in another global's initialiser, through a call of a global
# function value, and in two modules that declare the same global names.
#
# Story: 127.469.
#
# THE DEFECT.  A module-level global is held in an `i64` cell, and every read
# was an `i64` load with no conversion back to the value's type, while the
# backend also typed the read as i64.  `let y=1.5;` then `"\(y)"` printed
# 4609434218613702656 (1.5's bit pattern), and `let z=y+0.0` was an integer
# add of those bits.  A global function value was never recorded as one by
# the checker (fnval_binding searched only the enclosing function), so a
# call `h(1)` lowered as its argument (`let h=fn(x:i64):i64{<x+1}` gave
# h(1) = 1) and a zero-argument one failed the build with E9003 '@g'.
#
# THE FIX.  llvm.c records the value's LLVM type on each global (the
# annotation, else the checker's type of the initialiser) and converts the
# loaded i64 to it, exactly as the initialiser was converted when stored;
# expr_llvm_type answers that type.  types.c fnval_binding falls back to a
# module-level `let` when no local or parameter is in force (also inside a
# module-level initialiser), and the function-value call path accepts a
# global.  fnval_bind_sig resolves a copy's source (`let q=ad;`) through
# fnval_binding too, so a copy of a global function value keeps its
# signature.  Outside any function (a module-level initialiser or a
# module-level closure) the binder search runs lexically from the module
# root, so a closure's own parameters and locals shadow a global as they
# do in a function.  Pointer-held values (str, arrays, maps, structs, sums)
# keep the i64 convention every consumer already handles; they are covered
# here so that stays true.
#
# Every case runs at -O0 and -O2 and compares the exact stdout.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_modlet_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C1940: module-level let globals of every type"
echo "----------------------------------------------"

# The story's repro: f64, bool, function values.  v3.2.0 printed
# y=4609434218613702656 z=4886401197650477056 and h=1, and the zero-argument
# g failed the build with E9003.
cat > repro.tk <<'EOF'
m=a;
i=io:std.io;
let y=1.5;
let z=y+0.0;
let b=false;
let h=fn(x:i64):i64{<x+1};
let g=fn():i64{<7};
f=main():i64{
  io.println("y=\(y) z=\(z) b=\(b)");
  io.println("h=\(h(1)) g=\(g())");
  <0
};
EOF

# Every scalar and non-scalar kind, read in interpolation, arithmetic,
# conditions and calls; `mut` globals assigned from another function.
cat > kinds.tk <<'EOF'
m=a;
i=io:std.io;
t=$pt{x:i64;y:f64};
t=$shape{$circle:i64;$sq:i64};
let gi=42;
let gf=1.5;
let gb=true;
let gn=false;
let gs="hi";
let gy:byte=200;
let gw:i32=7;
let ga=@(1;2;3);
let gfa=@(0.5;2.25);
let gm=@("a":1;"b":2);
let gp=$pt{x:3;y:2.5};
let gsum=$shape{$sq:5};
let gh=fn(x:i64):i64{<x+1};
let gz=fn():i64{<7};
let gff=fn(x:f64):f64{<x*2.0};
let gr=&inc;
let gc=gf*2.0;
let gk=gh(2);
let gbx=3>2;
let gcount=mut.0;
let gmf=mut.0.25;
let gmb=mut.false;
f=inc(x:i64):i64{<x+10};
f=side(s:$shape):i64{<mt s{$circle:r r; $sq:n n*n}};
f=bump():i64{gcount=gcount+1;gmf=gmf+1.0;gmb=!gmb;<gcount};
f=show(x:bool):str{<"\(x)"};
f=main():i64{
  io.println("gi=\(gi) gf=\(gf) gb=\(gb) gn=\(gn) gs=\(gs)");
  io.println("gy=\(gy) gw=\(gw+1) gc=\(gc) gk=\(gk) gbx=\(gbx)");
  io.println("ga=\(ga.len) a1=\(ga.get(1)) gfa1=\(gfa.get(1))");
  io.println("gm=\(gm.getor("b";0)) gp=\(gp.x) \(gp.y) side=\(side(gsum))");
  io.println("gh=\(gh(1)) gz=\(gz()) gff=\(gff(1.25)) gr=\(gr(1))");
  let s=gf+gc;
  io.println("s=\(s) show=\(show(gb))");
  if(gb){io.println("gb-if")};
  if(!gn){io.println("gn-if")};
  bump();bump();
  io.println("gcount=\(gcount) gmf=\(gmf) gmb=\(gmb)");
  let cl=fn():str{<"cl gf=\(gf) gb=\(gb)"};
  io.println(cl());
  let gh=fn(x:i64):i64{<x+100};
  io.println("shadow=\(gh(1))");
  <0
};
EOF

# Two modules declare the same global names with different values: each
# reads its own (the cells are module-prefixed), f64 and bool included.
cat > lib.tk <<'EOF'
m=lib;
let gf=2.5;
let gb=true;
let gn=7;
let gh=fn(x:i64):i64{<x*3};
f=get():f64{<gf*2.0};
f=flag():bool{<gb};
f=call(x:i64):i64{<gh(x)+gn};
EOF
cat > app.tk <<'EOF'
m=app;
i=io:std.io;
i=l:lib;
let gf=0.5;
let gb=false;
let gn=1;
f=main():i64{io.println("app \(gf) \(gb) \(gn) lib \(l.get()) \(l.flag()) \(l.call(2))");<0};
EOF

# A global read as the exit code: an f64 global that read as bits could not
# be truncated to 3.
cat > rc.tk <<'EOF'
m=a;
let gf=3.75;
f=main():i64{<gf as i64};
EOF

# A copy of a global function value, at module level and inside a function,
# calls the function (127.469 round 2).  The copy's source was looked up only
# in the enclosing function, so `let q=ad;` got no signature and `q(2)`
# lowered as its argument (printed 2, not 7).
cat > copy.tk <<'EOF'
m=a;
i=io:std.io;
let ad=fn(x:i64):i64{<x+5};
let q=ad;
let q2=q;
let gr=&inc;
let qr=gr;
f=inc(x:i64):i64{<x+10};
f=main():i64{
  let w=ad;
  let w2=q;
  io.println("q=\(q(2)) q2=\(q2(3)) w=\(w(4)) w2=\(w2(1)) qr=\(qr(1))");
  <0
};
EOF

# runs <name> <expected-stdout> <expected-exit> <opt> <sources...>
runs() {
    local name="$1" expected="$2" want_rc="$3" opt="$4"; shift 4
    rm -f bin
    if [ "$#" -eq 1 ] && ! "${TKC}" --check "$@" > check.log 2>&1; then
        echo "  FAIL: ${name} ${opt}: --check rejected it"
        head -c 300 check.log | sed 's/^/      /'; echo
        FAIL=$((FAIL + 1)); return
    fi
    if ! "${TKC}" "${opt}" "$@" -o bin > build.log 2>&1 || [ ! -x bin ]; then
        echo "  FAIL: ${name} ${opt}: build failed"
        grep -o '"message":"[^"]*"' build.log | head -1 | sed 's/^/      /'
        FAIL=$((FAIL + 1)); return
    fi
    local out rc
    out="$(./bin 2>&1)"; rc=$?
    if [ "${out}" = "${expected}" ] && [ "${rc}" -eq "${want_rc}" ]; then
        echo "  PASS: ${name} ${opt}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name} ${opt}: expected '${expected}' rc ${want_rc}, got '${out}' rc ${rc}"
        FAIL=$((FAIL + 1))
    fi
}

# Round 3: inside a module-level closure the closure's own parameters and
# locals are in scope, exactly as inside a function.  The global fallback
# used to be a flat scan of the module's lets, so a module-level closure
# calling its function-typed PARAMETER (`kp(&dbl)`, body `p(4)`) lowered the
# call as its argument (printed 4, not 8), and a parameter or local that
# shadows the global `ad` was judged against ad's signature (false E4026
# "takes 1, got 0" / "got 2", false E4031 on an (f64):f64 or (i64):str
# parameter).  A non-function parameter, local or loop variable named `ad`
# or `gs` was typed as the global (false E4031 "the left side is
# '(i64):i64'"); for a closure parameter that held inside functions too
# (`kin`).
cat > clos.tk <<'EOF'
m=a;
i=io:std.io;
let ad=fn(x:i64):i64{<x+5};
let gs=3;
let kp=fn(p:(i64):i64):i64{<p(4)};
let kz=fn(ad:():i64):i64{<ad()};
let kk=fn(y:i64):i64{let ad=fn(x:i64;z:i64):i64{<x*z};<ad(y;3)};
let kf=fn(ad:(f64):f64):f64{<ad(1.5)};
let ks=fn(ad:(i64):str):str{<ad(3)};
let kn=fn(ad:i64):i64{<ad*2};
let ki=fn(ad:i64):str{<"v\(ad*2)"};
let kg=fn(gs:f64):f64{<gs*2.0};
let kl=fn():f64{let gs=1.25;<gs*2.0};
let lpk=fn():i64{let s=mut.0;lp(let ad=0;ad<3;ad=ad+1){s=s+ad};<s};
let k2=fn(y:i64):i64{<ad(y)};
let outer=fn(p:(i64):i64):i64{let inner=fn(z:i64):i64{<p(z)};<inner(3)};
let k9=fn():i64{<9};
let half=fn(x:f64):f64{<x/2.0};
let sh=fn(x:i64):str{<"n\(x)"};
f=dbl(x:i64):i64{<x*2};
f=main():i64{
  let kin=fn(ad:i64):i64{<ad*3};
  io.println("kp=\(kp(&dbl)) kz=\(kz(k9)) kk=\(kk(4)) kf=\(kf(half)) ks=\(ks(sh))");
  io.println("kn=\(kn(4)) ki=\(ki(4)) kg=\(kg(1.5)) kl=\(kl()) lpk=\(lpk())");
  io.println("k2=\(k2(1)) outer=\(outer(&dbl)) kin=\(kin(2)) ad=\(ad(0)) gs=\(gs)");
  <0
};
EOF

REPRO_OUT="y=1.5 z=1.5 b=false
h=2 g=7"

KINDS_OUT="gi=42 gf=1.5 gb=true gn=false gs=hi
gy=200 gw=8 gc=3 gk=3 gbx=true
ga=3 a1=2 gfa1=2.25
gm=2 gp=3 2.5 side=25
gh=2 gz=7 gff=2.5 gr=11
s=4.5 show=true
gb-if
gn-if
gcount=2 gmf=2.25 gmb=false
cl gf=1.5 gb=true
shadow=101"

CLOS_OUT="kp=8 kz=9 kk=12 kf=0.75 ks=n3
kn=8 ki=v8 kg=3 kl=2.5 lpk=3
k2=6 outer=6 kin=6 ad=5 gs=3"

for opt in -O0 -O2; do
    runs "story repro (f64, bool, function values)" "${REPRO_OUT}" 0 "${opt}" repro.tk
    runs "every global kind" "${KINDS_OUT}" 0 "${opt}" kinds.tk
    runs "same global names in two modules" "app 0.5 false 1 lib 5 true 13" 0 "${opt}" lib.tk app.tk
    runs "f64 global as the exit code" "" 3 "${opt}" rc.tk
    runs "copies of a global function value" "q=7 q2=8 w=9 w2=6 qr=11" 0 "${opt}" copy.tk
    runs "module-level closure parameters and locals shadow globals" "${CLOS_OUT}" 0 "${opt}" clos.tk
done

# A call of a global function value is judged against its signature, as a
# local's is: wrong arity is E4026, a mistyped argument E4031.  Before the
# fix neither was reported and the call lowered as its argument.
cat > arity.tk <<'EOF'
m=a;
let h=fn(x:i64):i64{<x+1};
f=main():i64{<h(1;2)};
EOF
cat > argty.tk <<'EOF'
m=a;
let h=fn(x:i64):i64{<x+1};
f=main():i64{<h("s")};
EOF
cat > copyarity.tk <<'EOF'
m=a;
let ad=fn(x:i64):i64{<x+5};
let q=ad;
f=main():i64{<q(1;2)};
EOF
cat > asgclos.tk <<'EOF'
m=a;
let g=5;
let k=fn():i64{g=7;<g};
f=main():i64{<k()};
EOF
checks() {
    local name="$1" code="$2" file="$3"
    if "${TKC}" --check "${file}" > chk.log 2>&1; then
        echo "  FAIL: ${name}: --check accepted it"; FAIL=$((FAIL + 1)); return
    fi
    if grep -q "\"error_code\":\"${code}\"" chk.log; then
        echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected ${code}"; head -c 300 chk.log | sed 's/^/      /'; echo
        FAIL=$((FAIL + 1))
    fi
}
checks "global function value: wrong arity is E4026" E4026 arity.tk
checks "global function value: mistyped argument is E4031" E4031 argty.tk
checks "copy of a global function value: wrong arity is E4026" E4026 copyarity.tk
checks "module-level closure assigning an immutable global is E4070" E4070 asgclos.tk

echo ""
echo "C1940: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
