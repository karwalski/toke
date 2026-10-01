#!/usr/bin/env bash
# C693_env_get_owned_copy.sh -- env.get and env.getor return a copy the
# program owns, so a later env.set does not change or invalidate it
# (story 121.78; the same defect as os.getenv, 121.75 / C672).
#
# THE DEFECT.  env.c's env_get / env_get_or returned getenv()'s pointer into
# the process environment.  A later setenv of the same name may overwrite
# that storage in place or free it: on Darwin a value taken after
# env.set(X; "first-value") read "SECOND" once env.set(X; "SECOND") had run.
#
# THE FIX.  env_get strdup()s the value; env_get_or returns a fresh copy of
# the value or of the default; env.tki declares "ownership": "caller".
#
# WHAT IS ASSERTED
#   1. env.get: a value taken before an env.set of the same name is unchanged;
#   2. env.getor: the same;
#   3. a second env.get sees the new value;
#   4. env.getor of an unset name gives the default;
#   5. a value inherited from the parent environment is read;
#   6. env.tki declares ownership caller for env.get and env.getor.
#
# Negative control: C693_STDLIB=<a src/stdlib tree> builds the probe against
# it (via TKC_STDLIB_DIR).  With main's tree cases 1 and 2 fail on Darwin
# (glibc's setenv never frees or reuses a value, so there main passes by luck).
#
# Story: 121.78
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C693_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c693_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C693: env.get / env.getor return owned copies (121.78)"
echo "-------------------------------------------------------"

cat > eg.tk <<'EOF'
m=eg;
i=env:std.env;
i=io:std.io;
f=main():i64{
  let s1=env.set("C693_PROBE";"first-value");
  let a=mt env.get("C693_PROBE") {$ok:v v;$err:e "ERR"};
  let s2=env.set("C693_PROBE";"SECOND");
  let s3=env.set("C693_OTHER";"x");
  let s4=env.set("C693_GETOR";"getor-first");
  let g=env.getor("C693_GETOR";"dflt");
  let s5=env.set("C693_GETOR";"GETOR-SECOND");
  let s6=env.set("C693_OTHER2";"y");
  let b=mt env.get("C693_PROBE") {$ok:v v;$err:e "ERR"};
  let u=env.getor("C693_NEVER_SET";"the-default");
  let h=mt env.get("C693_INHERITED") {$ok:v v;$err:e "ERR"};
  io.println(a);
  io.println(g);
  io.println(b);
  io.println(u);
  io.println(h);
  <0
};
EOF
if ! TKC_STDLIB_DIR="${S}" "${TKC}" eg.tk --out eg >eg.cl 2>&1; then
    echo "  FAIL: compile eg.tk"; sed 's/^/      | /' eg.cl | head -5; exit 1
fi
rc=0; env -u C693_NEVER_SET C693_INHERITED=from-parent ./eg --allow-env >out 2>err || rc=$?
[ "${rc}" -eq 0 ] || { fail "probe exited ${rc}"; sed 's/^/      | /' err | head -3; }
line() { sed -n "$1p" out; }
[ "$(line 1)" = "first-value" ] && pass "1 env.get value survives a later env.set" \
    || fail "1 env.get value taken before the env.set now reads '$(line 1)'"
[ "$(line 2)" = "getor-first" ] && pass "2 env.getor value survives a later env.set" \
    || fail "2 env.getor value taken before the env.set now reads '$(line 2)'"
[ "$(line 3)" = "SECOND" ] && pass "3 second env.get sees the new value" \
    || fail "3 second env.get read '$(line 3)'"
[ "$(line 4)" = "the-default" ] && pass "4 env.getor of an unset name is the default" \
    || fail "4 env.getor of an unset name read '$(line 4)'"
[ "$(line 5)" = "from-parent" ] && pass "5 inherited value read" || fail "5 inherited value read '$(line 5)'"

TKI="${REPO_ROOT}/stdlib/env.tki"
if python3 - "${TKI}" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
own = {e["name"]: e.get("ownership") for e in d["exports"] if e.get("kind") == "func"}
sys.exit(0 if own.get("env.get") == "caller" and own.get("env.getor") == "caller" else 1)
EOF
then pass "6 env.tki: env.get and env.getor are ownership caller"
else fail "6 env.tki does not declare ownership caller for env.get and env.getor"
fi

echo "-------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
