#!/usr/bin/env bash
# C534_get_spelled_call_is_fallible.sh — `alias.get(k)` on an import alias is
# classified as the fallible call it is, at the `mt`, the `let` and the `!`
# (story 127.249 item 3).
#
# THE DEFECT.  parse_get_postfix lowers every one-argument `.get(k)` to a
# NODE_INDEX_EXPR, so `args.get(n)`, `env.get(k)` -- and any imported module's
# own `get` -- never reached classify_fallible_call()'s qualified-call path:
# the `mt` decided on the value sentinel (an ok value of 0 took `$err`), the
# `$err:e` payload was never bound (`e.msg` RT005; a nested `mt e {..}` died
# at codegen with E9005), and `!` propagated on the sentinel.
#
# NOW: an index expression whose base is an import alias (not a value that
# shadows it) is classified as the call `alias.get` everywhere a qualified
# call is.  args.get/env.get themselves stay on the value sentinel until
# their glues fill the error slot (Batch E1, 127.241, merged as 7748162f);
# case 4 pins them.  Cases 1-2 pin the mechanism with a user module's `get`.
#
# Negative control: TKC=<main d3a083b8's toke> -- case 1 prints "err" for
# the ok 0, and case 2 fails to build (E9005 x2).  Case 3 (a map's .get) is
# green on both.  Case 4 (added once E1's glues merged, main 7748162f): main
# fails to build it (E9005 on the nested `mt e` over EnvErr).
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

cat > kv.tk <<'EOF2'
m=kv;
t=$kverr{$missing:str;$bad:i64};
f=get(n:i64):i64!$kverr{ if(n<0){ <$missing("no \(n)") }; if(n>100){ <$bad(n) }; <n-1 };
EOF2
"${TKC}" --emit-interface --check -I . kv.tk >/dev/null 2>&1

cat > a.tk <<'EOF2'
m=main;
i=io:std.io;
i=kv:kv;
f=main():i64{
  let a=mt kv.get(1) { $ok:v "ok\(v)"; $err:e "err" };
  let r=kv.get(1);
  let b=mt r { $ok:v "ok\(v)"; $err:e "err" };
  let c=mt kv.get(0-1) { $ok:v "ok\(v)"; $err:e "err" };
  io.println("\(a) \(b) \(c)");
  <0
};
EOF2
expect "1 an ok value of 0 from kv.get, direct and let-bound" "$(run a.tk kv.tk)" "ok0 ok0 err|rc=0"

cat > u.tk <<'EOF2'
m=main;
i=io:std.io;
i=kv:kv;
f=w(n:i64):i64!$kverr{ let v=kv.get(n)!$kverr; <v*10 };
f=main():i64{
  let b=mt kv.get(0-2) { $ok:v "ok\(v)"; $err:e mt e { $missing:m m; $bad:k "bad\(k)" } };
  let r=kv.get(500);
  let c=mt r { $ok:v "ok\(v)"; $err:e mt e { $missing:m m; $bad:k "bad\(k)" } };
  let d=mt w(1) { $ok:v "ok\(v)"; $err:e "err" };
  let g=mt w(0-4) { $ok:v "ok\(v)"; $err:e mt e { $missing:m m; $bad:k "bad\(k)" } };
  io.println("\(b) \(c) \(d) \(g)");
  <0
};
EOF2
expect "2 the payload binds, direct, let-bound and through !" "$(run u.tk kv.tk)" "no -2 bad500 ok0 no -4|rc=0"

cat > m.tk <<'EOF2'
m=m;
i=io:std.io;
f=main():i64{
  let m=@("a":0;"b":7);
  let a=mt m.get("a") { $ok:v "ok\(v)"; $err:e "miss" };
  let b=mt m.get("zz") { $ok:v "ok\(v)"; $err:e "miss" };
  io.println("\(a) \(b)");
  <0
};
EOF2
expect "3 control: a map's .get keeps the map path" "$(run m.tk)" "ok0 miss|rc=0"

# 4. args.get / env.get themselves, now that their glues fill the error slot
#    (127.241, Batch E1): the six shapes -- a nested `mt e` over EnvErr's
#    variants, an ok value, `e.msg` on ArgsErr, an in-range index, a
#    let-bound `mt r`, and `!` into a function declaring EnvErr.
cat > ae.tk <<'EOF2'
m=ae;
i=io:std.io;
i=env:std.env;
i=args:std.args;
f=g():str!EnvErr{ let v=env.get("C534_UNSET_VAR")!EnvErr; <v };
f=main():i64{
  let a=mt env.get("C534_UNSET_VAR") { $ok:v "ok:\(v)"; $err:e mt e { $notfound:m "nf:\(m)"; $invalid:m "inv:\(m)" } };
  let b=mt env.get("HOME") { $ok:v "ok"; $err:e "err" };
  let c=mt args.get(7 as u64) { $ok:v "ok:\(v)"; $err:e "err:\(e.msg)" };
  let d=mt args.get(0 as u64) { $ok:v "ok"; $err:e "err:\(e.msg)" };
  let r=args.get(9 as u64);
  let x=mt r { $ok:v "ok"; $err:e "lerr:\(e.msg)" };
  let y=mt g() { $ok:v "ok"; $err:e mt e { $notfound:m "pnf:\(m)"; $invalid:m "pinv" } };
  io.println(a); io.println(b); io.println(c); io.println(d); io.println(x); io.println(y);
  <0
};
EOF2
expect "4 args.get / env.get bind their payload in all six shapes" "$(unset C534_UNSET_VAR; HOME="${HOME:-/}" run ae.tk)" \
    "nf:env.get: environment variable not set|ok|err:args.get: index out of bounds|ok|lerr:args.get: index out of bounds|pnf:env.get: environment variable not set|rc=0"

echo "C534: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
