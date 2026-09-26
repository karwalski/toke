#!/usr/bin/env bash
# C056_loop_var_typed.sh — a loop variable `lp(let v=init;..)` is typed like
# the equivalent `let` (story 127.178).
#
# THE DEFECT. LOOP_INIT bindings were resolved (find_binding_before, cc2ac3e)
# but never typed, so `v as u64` on `lp(let v=0;..)` drew no W1001. Six
# A-SRT-0017 corpus records had drawn that W1001 only because the old
# first-binding lookup read an earlier `let v=... as i64`; cc2ac3e made the
# lookup right and the warning vanished.
#
# WHAT IS ASSERTED:
#   1. `(v-1)` over `lp(let v=0;..)`'s counter is typed
#      i64, so `(v-1) as u64` is W1001, in both the counter and the shadowing
#      shape the corpus records have. (127.189 deliberately stopped W1001 on
#      the counter itself — provably non-negative — and 127.204 on a literal
#      cast, so neither can witness the typing any more; `v-1` can.)
#   2. A loop variable typed by a cast init carries that type.
#   3. Correct loops stay error-free and run: mixing the i64 counter with a
#      u64 `.len` in the condition, indexing, arithmetic, append.
#
# Negative control: TKC built from 39b6cfd — cases 1 and 2 lack W1001 (red).
#
# Story: 127.178

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# every diagnostic as code@line
diags() { "${TKC}" --check "$1" 2>&1 | grep '"error_code"' | sed -E 's/.*"error_code":"([A-Z][0-9]+)".*"line":([0-9]+).*/\1@\2/' | tr '\n' ' '; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/a.tk" <<'EOF2'
m=t;
f=g():@u64{
  let out=mut.@();
  lp(let v=0;v<3;v=v+1){ out=out.append((v-1) as u64) };
  <out
};
EOF2
expect "1-int-lit-counter-cast-is-W1001" "$(diags "${WORK}/a.tk")" "W1001@4 "

cat > "${WORK}/b.tk" <<'EOF2'
m=t;
f=g(arr:@u64):@u64{
  let out=mut.@();
  lp(let i=0;i<arr.len;i=i+1){
    let v=arr.get(i);
    out=out.append(v)
  };
  lp(let v=0;v<2;v=v+1){ out=out.append((v-1) as u64) };
  <out
};
EOF2
expect "1-shadowing-counter-cast-is-W1001" "$(diags "${WORK}/b.tk")" "W1001@8 "

cat > "${WORK}/c.tk" <<'EOF2'
m=t;
f=g(n:u64):u64{
  let s=mut.0 as u64;
  lp(let k=n as i64;k>0;k=k-1){ s=s+(k as u64) };
  <s
};
EOF2
expect "2-cast-init-typed" "$(diags "${WORK}/c.tk")" "W1001@4 W1001@4 "

cat > "${WORK}/ok.tk" <<'EOF2'
m=t;
i=io:std.io;
f=main():i64{
  let xs=@(5;6;7);
  let acc=mut.@();
  let sum=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    sum=sum+xs.get(i)*i;
    acc=acc.append(i+1)
  };
  io.println("\(sum) \(acc.len) \(acc.get(2))");
  <0
};
EOF2
expect "3-correct-loop-clean" "$(diags "${WORK}/ok.tk")" ""
"${TKC}" "${WORK}/ok.tk" -o "${WORK}/ok" >/dev/null 2>&1
expect "3-correct-loop-runs" "$("${WORK}/ok" 2>&1)" "20 3 3"

echo "C056: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
