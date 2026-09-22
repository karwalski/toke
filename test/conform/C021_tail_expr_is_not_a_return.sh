#!/usr/bin/env bash
# C021_tail_expr_is_not_a_return.sh — a function body's tail expression is a
# hard error (E4071), not a silent 0 (story 127.113).
#
# `f=arith():i64{ 1+2+3 }` compiled clean, `--lint` said nothing, and it
# returned 0 — while the same body written `<1+2+3` returned 6.  The IR showed
# the shape exactly: the exit block loaded the computed value and then threw it
# away, `%t16 = load i64, i64* %t0` followed by `ret i64 0 ; implicit return`.
# Tag dispatch and arithmetic were both correct; only the return discarded the
# answer.
#
# This is a regression of closed story 56.10.2, whose own note records the
# diagnosis and the decision not to act on it: "Match expression at end of
# function without `<` produced `ret ptr null ; implicit return`. All content
# items stored as NULL pointers. Fixed with `let r=...; <r` pattern. tkc
# codegen bug logged (implicit return from match)."  The call site was worked
# around, the codegen defect was logged, and four months later it silently
# returns 0 for i64, for if, for mt and for plain arithmetic.
#
# OWNER DECISION 2026-09-22: `<` is the return operator and a function body
# must use it.  A value expression in tail position is a compile-time
# diagnostic.  Silently returning 0 is the one option that must not stand.
#
# PART 2 IS THE POINT OF THIS TEST.  A diagnostic that fires on everything
# proves nothing, so the same gate runs a battery of *correct* programs that
# it must stay silent on — including the two shapes that broke a regex scanner
# written for this story before it was thrown away: `;` is toke's ARGUMENT
# separator as well as its statement separator, so `io.println(str.concat(
# "a";"b"))` and `@(1;2;3)` are not multi-statement bodies and their tails are
# not tail expressions.
#
# Story: 127.113

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c021_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C021: a tail expression is not a return (E4071)"
echo "--------------------------------------"

# codes_for <file> — every error code the frontend emits for a source, one per
# line.  Reads the structured diagnostics, which is what the repair loop reads.
codes_for() {
    "${TKC}" --allow-all --check "$1" 2>&1 | python3 -c '
import sys, json
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("{"):
        continue
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        continue
    if d.get("severity") == "error":
        print(d.get("error_code", "?"))
'
}

# fix_for <file> — the `fix` field of the first E4071 on a source ('' if none).
fix_for() {
    "${TKC}" --allow-all --check "$1" 2>&1 | python3 -c '
import sys, json
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("{"):
        continue
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        continue
    if d.get("error_code") == "E4071":
        print(d.get("fix", ""))
        break
'
}

# ── Part 0: the behaviour this diagnostic exists to stop.  Before asserting
#    that the compiler rejects the shape, prove the shape really does lose the
#    answer — otherwise the diagnostic is guarding nothing.  `<` is the
#    control: same arithmetic, same types, one operator apart. ──

cat > control.tk <<'TKEOF'
m=t;
f=arith():i64{ <1+2+3 };
f=main():i64{ <arith() };
TKEOF

if "${TKC}" --allow-all -o control.bin control.tk >/dev/null 2>&1 && [ -x control.bin ]; then
    ./control.bin; got=$?
    if [ "${got}" -eq 6 ]; then
        echo "  PASS: control: '<1+2+3' returns 6"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: control: '<1+2+3' returned ${got}, expected 6 —"
        echo "        the harness cannot tell a returned value from a dropped one"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: control: the '<1+2+3' program did not compile"
    FAIL=$((FAIL + 1))
fi

# ── Part 1: every shape that silently returned 0 is now E4071. ──

# 1a — plain arithmetic, the story's own reproduction.
cat > arith.tk <<'TKEOF'
m=t;
f=arith():i64{ 1+2+3 };
TKEOF

# 1b — a complete if/el in tail position.  Returned 0, not 7.
cat > tailif.tk <<'TKEOF'
m=t;
f=tailif(x:i64):i64{ if(x>0){7}el{9} };
TKEOF

# 1c — a local binding in tail position.  infer() answers TY_UNKNOWN for an
#      un-annotated local, so a type-only check would miss this one.
cat > taillet.tk <<'TKEOF'
m=t;
f=taillet(x:i64):i64{ let y=x*2; y };
TKEOF

# 1d — a tail `mt`, which is 56.10.2's original shape.
cat > tailmatch.tk <<'TKEOF'
m=t;
t=$e{code:i64};
f=raise(x:i64):i64!$e{ if(x==0){<$e{code:1}}; <x };
f=tailmatch(x:i64):i64{ mt raise(x){$ok:v v;$err:e 0-1} };
TKEOF

# 1e — a tail call whose value is dropped.  This is the shape loke's T-2
#      workaround lifts a match into, across 17 sites.
cat > tailcall.tk <<'TKEOF'
m=t;
f=helper():i64{ <7 };
f=tailcall():i64{ helper() };
TKEOF

# 1f — not just i64.  56.10.2 was about pointers; `str` is the same defect.
cat > tailstr.tk <<'TKEOF'
m=t;
f=tailstr():str{ "hello" };
TKEOF

for c in "arith:plain arithmetic" \
         "tailif:a complete if/el" \
         "taillet:an un-annotated local binding" \
         "tailmatch:a tail mt (56.10.2's shape)" \
         "tailcall:a dropped call result" \
         "tailstr:a str tail"; do
    f="${c%%:*}"; what="${c#*:}"
    # Capture first: `grep -q` closes the pipe on its first match, and under
    # `pipefail` the upstream's SIGPIPE would fail the whole pipeline.
    got="$(codes_for "${f}.tk")"
    if echo "${got}" | grep -qx 'E4071'; then
        echo "  PASS: ${what} in tail position is E4071"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${what} in tail position produced no E4071:"
        echo "${got}" | sed 's/^/        /' | head -4
        echo "        (silently returns 0 — see ${f}.tk)"
        FAIL=$((FAIL + 1))
    fi
done

# ── Part 2: and it stays silent on correct programs.  Without this half a
#    diagnostic that fired unconditionally would score full marks above. ──

cat > clean.tk <<'TKEOF'
m=t;
i=io:std.io;
t=$e2{code:i64};
f=helper():i64{ <7 };
f=good():i64{ <1+2+3 };
f=printthenreturn():i64{ io.println("hi"); <0 };
f=iffull(x:i64):i64{ if(x>0){<7}el{<9} };
f=early(x:i64):i64{ if(x>0){<7}; <9 };
f=loopt():i64{ lp(let i=0;i<3;i=i+1){ io.println("x") }; <0 };
f=raise2(x:i64):i64!$e2{ if(x==0){<$e2{code:1}}; <x };
f=sidematch(x:i64):i64{ mt raise2(x){$ok:v io.println("ok");$err:e io.println("err")}; <0 };
f=retmatch(x:i64):i64{ mt raise2(x){$ok:v <v;$err:e <0} };
TKEOF

# The two shapes that broke a regex scanner for this story.  `;` separates
# ARGUMENTS as well as statements, so neither body has a tail expression at all.
cat > semicolons.tk <<'TKEOF'
m=t;
i=io:std.io;
f=argsep():i64{ io.println(str.concat("a";"b")); <1 };
f=arraylit():i64{ let a=@(1;2;3); <a.get(0) };
f=xorish(x:i64):i64{ <(x*18) ^ (x/3) };
TKEOF

for c in "clean:11 correct functions, every ambiguous shape included" \
         "semicolons:';' as an argument separator, not a statement break"; do
    f="${c%%:*}"; what="${c#*:}"
    n="$(codes_for "${f}.tk" | wc -l | tr -d ' ')"
    if [ "${n}" -eq 0 ]; then
        echo "  PASS: no diagnostic on ${what}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${what} produced ${n} error(s) — E4071 over-fires:"
        "${TKC}" --allow-all --check "${f}.tk" 2>&1 | sed 's/^/        /' | head -4
        FAIL=$((FAIL + 1))
    fi
done

# A void-returning tail call discards nothing; it is not this diagnostic, and
# flagging it would put E4071 on every procedure in the repo.
cat > voidtail.tk <<'TKEOF'
m=t;
i=io:std.io;
f=say():void{ io.println("hi") };
TKEOF

vt="$(codes_for voidtail.tk)"
if ! echo "${vt}" | grep -qx 'E4071'; then
    echo "  PASS: a void tail call is not E4071"
    PASS=$((PASS + 1))
else
    echo "  FAIL: E4071 fired on a void function's tail call"
    FAIL=$((FAIL + 1))
fi

# ── Part 3: the `fix` field.  AGENTS.md 3.1 — an incorrect `fix` breaks the
#    automated repair loop, and 131.78 is the standing example of an "obvious"
#    suggested fix that was wrong.  So `fix` is present only where prefixing
#    `<` is the single reading the source can have, and absent where a second
#    reading exists. ──

if [ "$(fix_for arith.tk)" = "prefix the expression with '<' to return it" ]; then
    echo "  PASS: a discarded arithmetic value carries the '<' fix"
    PASS=$((PASS + 1))
else
    echo "  FAIL: expected the '<' fix on arith.tk, got [$(fix_for arith.tk)]"
    FAIL=$((FAIL + 1))
fi

for c in "tailcall:a call may be there for its side effect" \
         "tailif:an if wants '<' inside each arm, not in front" \
         "tailmatch:an mt wants '<' inside each arm, not in front"; do
    f="${c%%:*}"; why="${c#*:}"
    got="$(fix_for "${f}.tk")"
    if [ -z "${got}" ]; then
        echo "  PASS: no fix offered where the repair is ambiguous (${why})"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${f}.tk offered a fix [${got}] but ${why}"
        FAIL=$((FAIL + 1))
    fi
done

# ── Part 4: the fix the diagnostic suggests must actually be the repair.  A
#    `fix` that does not compile, or that compiles to a different answer, is
#    worse than none — it is what the repair loop applies unread. ──

cat > repaired.tk <<'TKEOF'
m=t;
f=arith():i64{ <1+2+3 };
f=main():i64{ <arith() };
TKEOF

if [ "$(codes_for repaired.tk | wc -l | tr -d ' ')" -eq 0 ] \
   && "${TKC}" --allow-all -o repaired.bin repaired.tk >/dev/null 2>&1; then
    ./repaired.bin; got=$?
    if [ "${got}" -eq 6 ]; then
        echo "  PASS: applying the suggested fix clears E4071 and returns 6"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: the repaired program returned ${got}, expected 6"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: the repaired program still does not compile clean"
    codes_for repaired.tk | sed 's/^/        /' | head -4
    FAIL=$((FAIL + 1))
fi

# ── Part 5: the code is documented.  check-error-codes gates this repo-wide,
#    but an undocumented E-code makes the diagnostic unusable to a reader who
#    meets it for the first time, so it is asserted where it is introduced. ──

if grep -q '^### E4071$' "${REPO_ROOT}/docs/reference/errors.md"; then
    echo "  PASS: E4071 is documented in docs/reference/errors.md"
    PASS=$((PASS + 1))
else
    echo "  FAIL: E4071 has no section in docs/reference/errors.md"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
