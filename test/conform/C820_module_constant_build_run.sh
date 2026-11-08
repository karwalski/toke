#!/usr/bin/env bash
# C820_module_constant_build_run.sh — a module constant `name=literal:Type;`
# (grammar.ebnf ConstDecl) builds, and the binary reads its value.
#
# Story: 127.310.
#
# THE DEFECT.  `m=a; lim=5:i64; f=main():i64{<lim-5};` passed `tkc --check`
# and then failed the build with E9003 "use of undefined value '%lim'": a use
# of the constant lowered as a load of a LOCAL named %lim.  The global that
# emit_toplevel did write was also wrong -- it read the literal from
# children[2], the type node, so every constant was `constant i64 0`.
#
# THE FIX.  A constant is folded at each use: the use lowers as its literal,
# converted to the declared type as an annotated `let` initialiser is.  No
# global is emitted, so two modules may declare the same constant name.
#
# WHY VALUES.  A build that links proves only that the IR was valid; the
# binaries print what they read, so a constant folded to 0 (the old global)
# or a shadowing binding read through to the constant fails here.  Every case
# runs at -O0 and at -O2.

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

WORK="$(mktemp -d /tmp/tkc_modconst_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C820: module constants build and run"
echo "------------------------------------"

# The story's repro, verbatim: exit code 0 only if lim reads 5.
cat > repro.tk <<'EOF'
m=a;
lim=5:i64;
f=main():i64{<lim-5};
EOF

cat > kinds.tk <<'EOF'
m=a;
i=io:std.io;
i=s:std.str;
lim=42:i64;
nm="hello":$str;
pi=3.5:f64;
dbg=true:bool;
w=7:i32;
b=200:u8;
f=h(x:i64):i64{<x+1};
f=main():i64{
  let x=lim*2;
  io.println("lim=\(lim) x=\(x) h=\(h(lim))");
  io.println(nm);
  io.println("len=\(s.len(nm))");
  if(nm=="hello"){io.println("eq")};
  let r=pi*2.0;
  io.println("r=\(r)");
  if(dbg){io.println("dbg")};
  let y:i32=w+1;
  io.println("y=\(y)");
  let z:u8=b;
  io.println("z=\(z)");
  <0
};
EOF

# A parameter or a `let` of the same name shadows the constant where it is in
# force, and only there: after the block the name is the constant again.
cat > shadow.tk <<'EOF'
m=a;
i=io:std.io;
lim=5:i64;
f=g(lim:i64):i64{<lim};
f=main():i64{
  let a=g(9);
  if(a>0){let lim=100;io.println("in \(lim)")};
  io.println("out \(lim) \(a)");
  <0
};
EOF

# Two modules declare the same constant name with different values: no global
# is emitted, so nothing collides at link, and each module reads its own.
cat > lib.tk <<'EOF'
m=lib;
lim=5:i64;
f=get():i64{<lim+2};
EOF
cat > app.tk <<'EOF'
m=app;
i=io:std.io;
i=l:lib;
lim=30:i64;
f=main():i64{io.println("app \(lim) lib \(l.get())");<0};
EOF

# runs <name> <expected-stdout> <expected-exit> <opt> <sources...>
runs() {
    local name="$1" expected="$2" want_rc="$3" opt="$4"; shift 4
    rm -f bin
    # --check takes one source; a multi-module build is judged by the build.
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

KINDS_OUT="lim=42 x=84 h=43
hello
len=5
eq
r=7
dbg
y=8
z=200"

for opt in -O0 -O2; do
    runs "story repro (lim-5 exits 0)" "" 0 "${opt}" repro.tk
    runs "i64/str/f64/bool/i32/u8 constants" "${KINDS_OUT}" 0 "${opt}" kinds.tk
    runs "parameter and block let shadow the constant" "in 100
out 5 9" 0 "${opt}" shadow.tk
    runs "same constant name in two modules" "app 30 lib 7" 0 "${opt}" lib.tk app.tk
done

# A non-zero value must be observable as an exit code too (the old global
# folded every constant to 0, which exit-0 alone could not tell apart).
cat > rc.tk <<'EOF'
m=a;
lim=7:i64;
f=main():i64{<lim};
EOF
runs "constant read as the exit code" "" 7 -O2 rc.tk

echo ""
echo "C820: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
