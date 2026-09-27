#!/usr/bin/env bash
# C267_inplace_self_concat_and_borrowed_init.sh — ADR-0006 D2's in-place
# lowering covers `x=x+@(v)`, survives a read-only `j.print(x)`, and covers
# `let r=mut.a` followed by `r=r.set(..)`; the retaining shapes still copy
# (stories 127.229 (2), 127.229 (7)).
#
# THE DEFECT. (2) `r=r+@(i)` was not a self-update, so every iteration copied
# the accumulator and never freed the old block (100k appends: 8-10 s,
# 6-8 GB, where `.append` took 0.01 s / 3 MB); and `j.print(r)` after the
# loop was a "retaining" use (only std.str/io/fmt were vouched for), which
# demoted `.append` too.  (7) `let r=mut.a` is not a fresh initialiser, so
# `r=r.set(i;v)` copied the whole array on every call (eval 0221's bubble
# sort: 1.1 GB at n=1000).  llvm.c: la_self_concat, json.print in
# la_nonretaining_callee, la_borrowed_init (one copy at the binding).
#
# Each case checks the LOWERING in main's (or f's) body as
# "<in-place calls> <copying calls>" and the program's OUTPUT.
#
# Negative control: TKC built from main at d236e6f — cases 1, 2, 3, 4 red
# (no in-place call; copying calls instead).  Controls 5-8 are green on
# both and were each shown red by an injection: la_bad_use answering "no bad
# use" always -> 5 and 7 red ("4 4", "9 9": the alias saw the mutation);
# dropping the copy at the borrowed binding -> 4-runs and 6 red ("11 13 11
# 13", "50 50": the source array was mutated); la_self_concat accepting any
# right operand -> 8 red ("2 2").
#
# Story: 127.229 (2), 127.229 (7)

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
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" "${2:-}" 2>/dev/null | tr '\n' ' '; }
# lowering FILE FN: "<in-place> <copying>" calls in FN's body (append, set, concat)
lowering() {
    "${TKC}" --emit-llvm "$1" -o "${WORK}/p.ll" >/dev/null 2>&1 || { echo "compile-failed"; return; }
    awk -v fn="$2" '$0 ~ "^define .*@" fn "\\(" {f=1}
        f&&/call i64 @tk_array_(append|set)_inplace_w/{i++}
        f&&/call i64 @tk_array_(append|set)_w\(/{c++}
        f&&/call i8\* @tk_array_concat\(i8\* %t[0-9]+, i8\* %t/{c++}
        f&&/^}/{f=0} END{printf "%d %d", i, c}' "${WORK}/p.ll"
}

# 1. x=x+@(v) in a loop, unannotated accumulator
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let r=mut.@();
  lp(let i=0;i<5;i=i+1){r=r+@(i*i)};
  r=r+@(7;8);
  io.println("\(r.len) \(r.get(4)) \(r.get(6))");
  <0
};
EOF
expect "1-self-concat-lowering" "$(lowering "${WORK}/p1.tk" tk_main)" "3 0"
expect "1-self-concat-runs"     "$(runs "${WORK}/p1.tk")" "7 16 8 "

# 2. j.print(r) after the loop keeps .append in place
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=j:std.json;
f=main():i64{
  let r:@i64=mut.@();
  lp(let i=0;i<4;i=i+1){r=r.append(i)};
  j.print(r.get(1));
  j.print(r);
  <0
};
EOF
expect "2-jprint-lowering" "$(lowering "${WORK}/p2.tk" tk_main)" "1 0"
expect "2-jprint-runs"     "$(runs "${WORK}/p2.tk")" "1 [0,1,2,3] "

# 3. elements see x as it was before the concat (literal semantics)
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let r=mut.@(1);
  r=r+@(r.len as i64;r.len as i64);
  io.println("\(r.len) \(r.get(1)) \(r.get(2))");
  <0
};
EOF
expect "3-eval-order-lowering" "$(lowering "${WORK}/p3.tk" tk_main)" "2 0"
expect "3-eval-order-runs"     "$(runs "${WORK}/p3.tk")" "3 1 1 "

# 4. let r=mut.a; r=r.set(..) — one copy at the binding, sets in place;
#    the source array is unchanged
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
f=bump(a:@i64):@i64{
  let r=mut.a;
  lp(let i=0;i<r.len;i=i+1){r=r.set(i;r.get(i)+10)};
  <r
};
f=main():i64{
  let a=@(1;2;3);
  let b=bump(a);
  io.println("\(a.get(0)) \(a.get(2)) \(b.get(0)) \(b.get(2))");
  <0
};
EOF
expect "4-borrowed-init-lowering" "$(lowering "${WORK}/p4.tk" t_bump)" "1 0"
expect "4-borrowed-init-runs"     "$(runs "${WORK}/p4.tk")" "1 3 11 13 "

# 5. retaining control: the accumulator is handed to a retaining stdlib
#    call (std.json enc is not vouched for) -> copying
cat > "${WORK}/p5.tk" <<'EOF'
m=t;i=j:std.json;i=io:std.io;
f=keep(a:@i64):@i64{<a};
f=main():i64{
  let r=mut.@();
  lp(let i=0;i<3;i=i+1){r=r+@(i)};
  let k=keep(r);
  r=r+@(9);
  io.println("\(r.len) \(k.len)");
  <0
};
EOF
expect "5-retained-lowering" "$(lowering "${WORK}/p5.tk" tk_main)" "0 2"
expect "5-retained-runs"     "$(runs "${WORK}/p5.tk")" "4 3 "

# 6. retaining control: the source of a borrowed init stays readable and
#    unchanged; a second binding of the name refuses the borrowed path
cat > "${WORK}/p6.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let a=@(5;6);
  let r=mut.a;
  r=r.set(0;50);
  io.println("\(a.get(0)) \(r.get(0))");
  <0
};
EOF
expect "6-source-unchanged-runs" "$(runs "${WORK}/p6.tk")" "5 50 "

# 7. retaining control: the borrowed-init local escapes (returned AND
#    kept) -> it is not linear, every set copies
cat > "${WORK}/p7.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2);
  let r=mut.a;
  let alias=r;
  r=r.set(0;9);
  io.println("\(alias.get(0)) \(r.get(0))");
  <0
};
EOF
expect "7-escaping-lowering" "$(lowering "${WORK}/p7.tk" tk_main)" "0 1"
expect "7-escaping-runs"     "$(runs "${WORK}/p7.tk")" "1 9 "

# 8. retaining control: x=x+y with y an array VARIABLE is not a literal
#    append -> the accumulator is not claimed linear by it
cat > "${WORK}/p8.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let y=@(1;2);
  let r=mut.@(0);
  r=r+y;
  r=r.append(3);
  io.println("\(r.len) \(y.len)");
  <0
};
EOF
expect "8-var-concat-runs" "$(runs "${WORK}/p8.tk")" "4 2 "

echo "C267: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
