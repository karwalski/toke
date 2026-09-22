#!/usr/bin/env bash
# C024_type_name_is_not_a_value.sh — three diagnostic-quality defects that all
# make the compiler's OUTPUT lie rather than its code generation go wrong.
#
#  A (137.4) — a BARE type name in a value position was not checked at all.
#      toke keeps type names and value names in one namespace, and
#      resolve_ident (names.c) took whatever scope_lookup returned without
#      ever inspecting its DeclKind.  So `take(thing)` and `thing.n`, where
#      `thing` is a declared type and no local of that name exists, resolved
#      clean, type-checked clean, and were handed to clang — which failed on
#      `use of undefined value '%thing'`, naming an LLVM temporary instead of
#      the program.  E4033 already existed but lived in types.c gated on a
#      NODE_TYPE_IDENT base, so it caught only the `$`-prefixed spelling
#      WITH a field access.  In the 137 migration the bare spelling hid a
#      genuinely missing parameter and three functions referencing a `store`
#      declared nowhere — while the sibling function took `store:$store`
#      correctly, so the two sat side by side for months.
#
#  B (127.116) — every diagnostic raised inside a string interpolation
#      reported `line 1`.  The type checker replays the interpolation through
#      its own lex/parse of a throwaway wrapper program and rebased only the
#      OFFSETS, so `line` and `col` stayed as the wrapper's own counters while
#      `offset` and `source_line` were right and disagreed with them.  A
#      confidently wrong location is worse than none: it is what points the
#      automated repair loop at the wrong line, the harm AGENTS.md §3.1 exists
#      to prevent for the `fix` field.  These cases assert the LINE NUMBER,
#      not just the code.
#
#  C (127.117) — E4025 was emitted twice for one field error, because it
#      lacked the tc_first_report() guard E4033 and E4035 use.  It takes a USE
#      of the binding to show: bind_init_type() re-enters the initialiser only
#      when something needs the binding's type.  Duplicate records inflate the
#      diagnostic counts that feed published figures and invite the repair
#      loop to apply one fix twice.
#
# The controls are the point of this file as much as the defects.  A bare user
# type name is not legal in ANY type position — `x:thing`, `as thing` and
# `thing{...}` are all E2005/E2003 parse errors both before and after — so the
# fix cannot reach a legitimate spelling.  Cases 5-10 pin that.
#
# Stories: 137.4, 127.116, 127.117, 127.90, 136.36

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

WORK="$(mktemp -d /tmp/tkc_typename_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C024: a type name is not a value; a diagnostic knows its own line"
echo "----------------------------------------------------------------"

# diag_at CODE COUNT LINE NAME SOURCE — `--check` must emit exactly COUNT
# records of CODE, every one of them on LINE.
#
# The count is asserted, not just presence (127.117), and so is the line
# (127.116): each was a separate way for the output to be wrong about a
# program the compiler had understood correctly.
diag_at() {
    local code="$1" want="$2" line="$3" name="$4" src="$5"
    printf '%s\n' "${src}" > "c.tk"
    local out n nl
    out="$("${TKC}" --check "c.tk" 2>&1)"
    n="$(printf '%s\n' "${out}" | grep -c "\"error_code\":\"${code}\"" || true)"
    nl="$(printf '%s\n' "${out}" | grep "\"error_code\":\"${code}\"" | grep -c "\"line\":${line}," || true)"
    if [ "${n}" = "${want}" ] && [ "${nl}" = "${want}" ]; then
        echo "  PASS: ${name} — ${want}x ${code}, line ${line}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected ${want}x ${code} at line ${line}, got ${n} records (${nl} on that line)"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -4
        FAIL=$((FAIL + 1))
    fi
}

# refuses CODE NAME SOURCE — the compiler must REFUSE to produce a binary.
#
# Asserted against `--out`, not `--check` alone: 137.4's whole harm is that a
# program reached clang, so "no binary" is the guarantee, and `--check` seeing
# it too is asserted separately by diag_at.
refuses() {
    local code="$1" name="$2" src="$3"
    printf '%s\n' "${src}" > "r.tk"
    local out rc=0
    rm -f r_bin
    out="$("${TKC}" --allow-all --out r_bin "r.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && [ ! -x r_bin ] && printf '%s' "${out}" | grep -q "\"error_code\":\"${code}\""; then
        echo "  PASS: ${name} refused with ${code}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected a refusal carrying ${code}, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        [ -x r_bin ] && echo "      it produced a binary, which printed: $(./r_bin 2>&1 | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

# runs NAME EXPECTED SOURCE — a correct program must still build and run.
runs() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local out rc=0
    rm -f ok_bin
    if ! "${TKC}" --allow-all --out ok_bin "ok.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./ok_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}' (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# ── A1: the reported case — a bare type name as a call argument ──────────
# Before: `--check` exit 0; the build died at clang with
# "use of undefined value '%thing'" and an E9003 pointing at line 0.
diag_at "E4033" 1 6 "bare type name as a call argument" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=take(x:$thing):i64{<x.n};
f=main():i64{
  io.println("\(take(thing))");
  <0
};'
refuses "E4033" "bare type name as a call argument (no binary)" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=take(x:$thing):i64{<x.n};
f=main():i64{
  io.println("\(take(thing))");
  <0
};'

# ── A2: a bare type name with a field access ─────────────────────────────
# 127.90 closed the `$thing.n` spelling only.  This is the spelling the
# report renders it in, and it produced NO diagnostic at all.
diag_at "E4033" 1 5 "bare type name with a field access" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  io.println("\(thing.n)");
  <0
};'

# ── A3: every other value position is the same hole ──────────────────────
# Listed separately because a fix scoped to "call arguments" would pass A1
# and leave the rest standing.
diag_at "E4033" 1 5 "bare type name on the RHS of a let" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  let q=thing;
  io.println("\(q)");
  <0
};'
diag_at "E4033" 1 5 "bare type name in an arithmetic expression" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  let q=thing+1;
  io.println("\(q)");
  <0
};'
diag_at "E4033" 1 4 "bare type name in a return" \
'm=main;
t=thing{n:i64};
f=g():i64{
  <thing
};
f=main():i64{<g()};'
diag_at "E4033" 1 5 "bare type name as a condition" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  if(thing>0){ io.println("x"); };
  <0
};'

# ── A4: the controls 137.4 names ─────────────────────────────────────────
# An undeclared name is still E3011, and the `$` spelling is still E4031/
# E4033 — the new check must not swallow either.
diag_at "E3011" 1 4 "an undeclared name is still E3011, not E4033" \
'm=main;
i=io:std.io;
f=main():i64{
  io.println("\(neverdeclared)");
  <0
};'

# ── B: a diagnostic inside an interpolation knows its own line ───────────
# Same three codes 127.116 confirmed, each on a line that is not 1 and not
# the line the enclosing statement would give by accident.
diag_at "E4025" 1 7 "E4025 inside an interpolation reports the real line" \
'm=main;
i=io:std.io;
t=pt{a:i64};
f=main():i64{
  let p=$pt{a:1};

  io.println("\(p.qq)");
  <0
};'
diag_at "E4033" 1 6 "E4033 inside an interpolation reports the real line" \
'm=main;
i=io:std.io;
t=pt{a:i64};
f=main():i64{

  io.println("\($pt.a)");
  <0
};'
diag_at "E4035" 1 7 "E4035 inside an interpolation reports the real line" \
'm=main;
i=io:std.io;
f=main():i64{
  let m:@(str:i64)=@("a":1);


  io.println("\(m.values)");
  <0
};'
# The same expression OUTSIDE an interpolation was always right; pinned so
# the two spellings cannot drift apart again.
diag_at "E4025" 1 6 "the same field error outside an interpolation" \
'm=main;
i=io:std.io;
t=pt{a:i64};
f=main():i64{
  let p=$pt{a:1};
  let q=p.qq;
  io.println("\(q)");
  <0
};'

# ── C: one field error, one diagnostic record ────────────────────────────
# `let q=p.qq` alone reported once; it took a USE of q to make
# bind_init_type() re-enter the initialiser and report a second time.  The
# count above is what carries this — diag_at asserts it exactly.
diag_at "E4025" 1 6 "a used bad-field binding reports once, not twice" \
'm=main;
i=io:std.io;
t=pt{a:i64};
f=main():i64{
  let p=$pt{a:1};
  let q=p.qq;
  io.println("\(q) \(q)");
  <0
};'
# Two DIFFERENT bad fields must still produce two records: the guard is
# per-node, and a fix that deduplicated by code would hide real errors.
diag_at "E4025" 2 6 "two distinct bad fields still report twice" \
'm=main;
i=io:std.io;
t=pt{a:i64;b:i64};
f=main():i64{
  let p=$pt{a:1;b:2};
  let q=p.qq;let r=p.rr;
  io.println("\(q) \(r)");
  <0
};'

# ── The blast-radius controls: legitimate spellings are untouched ────────
# A bare user type name is not legal in any TYPE position either — each of
# these was already a parse error before the change, which is why extending
# resolve_ident cannot reach a correct program.  Pinned as runnable programs
# in their correct ($-prefixed) spelling instead.
runs "a type used correctly: annotation, literal, param, field" "7" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=take(x:$thing):i64{<x.n};
f=main():i64{
  let t:$thing=$thing{n:7};
  io.println("\(take(t))");
  <0
};'
runs "a local may share a name with a type" "3" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  let thing=3;
  io.println("\(thing)");
  <0
};'
runs "an as-cast to a declared type" "4" \
'm=main;
i=io:std.io;
t=thing{n:i64};
f=main():i64{
  let d=4.7;
  let q=d as $i64;
  io.println("\(q)");
  <0
};'
runs "a sum-type variant constructor is not a type name" "200" \
'm=t;
i=io:std.io;
t=$err{$bad:$str};
f=lit(n:i64):i64!$err{if(n<0){<$bad("neg")};<n*2};
f=main():i64{
  let a=mt lit(0-5){$ok:v 100;$err:e 200};
  io.println("\(a)");
  <0
};'
runs "a module alias is not a type name" "hi" \
'm=main;
i=io:std.io;
f=main():i64{io.println("hi");<0};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
