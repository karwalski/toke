#!/usr/bin/env bash
# C610_regex_checkre_error_as_value.sh — str.checkre returns an invalid
# pattern's error as a value; the RT008 trap and E4075 stay as they were
# (story 121.45 item 1).
#
# THE DEFECT.  Every regex call (containsre, countre, findall, matches,
# replacere) reads an invalid pattern computed at run time as RT008 and
# exit(1).  str.md told the reader to "validate an untrusted pattern before
# relying on it", but no call could: a server taking a pattern from a request
# was one bad request away from exiting.  Measured on main f605aec9: a program
# calling str.checkre does not compile (no such export).
#
# THE CONTRACT (str.tki, docs/stdlib/str.md "Regular expressions"):
#   str.checkre(pattern:$str):$str!$regexerr
#   $ok  -> the pattern unchanged; the five matching calls will not trap
#           RT008 on it (the same tkre_compile decides both)
#   $err -> $regexerr{msg:$str;pos:i64}: the engine's reason, and the byte
#           offset into the pattern (-1 when the failure is not at a byte)
#
# BOTH DIRECTIONS and the unchanged neighbours:
#   - invalid computed patterns of four kinds are $err with the engine's
#     message and offset, and the program keeps running;
#   - a valid pattern is $ok and unchanged, after a poisoned slot, so a glue
#     that forgot to CLEAR the slot fails;
#   - the checked pattern then drives str.containsre;
#   - checkre's message is the reason RT008 prints for the same pattern;
#   - NEGATIVE CONTROL: the same invalid pattern handed straight to
#     str.containsre still stops the program with RT008 and exit 1;
#   - a literal invalid pattern is still E4075 at --check for containsre, and
#     is NOT E4075 for checkre, whose job is to return that error.
# Each case is its own program, at -O0 and -O2.
#
# Story: 121.45 (item 1)

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

WORK="$(mktemp -d /tmp/tkc_c610_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C610: str.checkre returns the pattern error as a value"
echo "--------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# write_prog BODY — main's statements; the pattern text is built at run time
# with str.concat so the compiler's literal check (E4075) cannot see it.
write_prog() {
    cat > c.tk <<EOF
m=p;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let poison=str.toint("zz");
$1
  <0
};
EOF
}

# run_case NAME EXPECTED_STDOUT EXPECTED_RC BODY
run_case() {
    local name="$1" expected="$2" want_rc="$3" body="$4" opt out rc
    write_prog "${body}"
    for opt in -O0 -O2; do
        rm -f c_bin
        if ! "${TKC}" --allow-all "${opt}" --out c_bin c.tk >compile.log 2>&1; then
            fail "${name} ${opt}: did not compile: $(grep -o '"error_code":"[^"]*"' compile.log | head -1)"
            continue
        fi
        out="$(./c_bin 2>/dev/null)"; rc=$?
        if [ "${out}" = "${expected}" ] && [ "${rc}" = "${want_rc}" ]; then
            pass "${name} ${opt}"
        else
            fail "${name} ${opt}: expected [${expected}] rc ${want_rc}, got [${out}] rc ${rc}"
        fi
    done
}

# err_case NAME PATTERN_TAIL MSG POS — pattern is "x" + PATTERN_TAIL at run time.
err_case() {
    local name="$1" tail="$2" msg="$3" pos="$4"
    run_case "${name}" "${msg}
${pos}
after" 0 "  let p=str.concat(\"x\";\"${tail}\");
  io.println(mt str.checkre(p){\$ok:v \"ok\";\$err:e e.msg});
  io.println(mt str.checkre(p){\$ok:v \"ok\";\$err:e str.fromint(e.pos)});
  io.println(\"after\");"
}

err_case "unbalanced ( is \$err, program continues" "a(b" \
    'unbalanced parenthesis: `(` has no closing `)`' 2
err_case "stray ) is \$err at its byte" "a)" \
    'unbalanced parenthesis: `)` has no opening `(`' 2
err_case "lazy quantifier is \$err" "a*?" \
    'lazy quantifiers (*? +? ?? {n,m}?) are not supported: matching is leftmost-longest; use a negated class, e.g. <[^>]*> for <.*?>' 3
err_case "backreference is \$err" '\\1' \
    'backreference \1 is not supported' 1

run_case "valid pattern is \$ok and unchanged (slot cleared)" '[0-9]+x' 0 \
'  let p=str.concat("[0-9]+";"x");
  io.println(mt str.checkre(p){$ok:v v;$err:e e.msg});'

run_case "checked pattern drives containsre" '1
0' 0 \
'  let q=mt str.checkre(str.concat("[0-9]";"+")){$ok:v v;$err:e "zzz"};
  io.println(str.fromint(str.containsre("ab12";q)));
  io.println(str.fromint(str.containsre("abcd";q)));'

# The message checkre returns is the reason RT008 prints for the same pattern.
write_prog '  let p=str.concat("x";"a(b");
  io.println(mt str.checkre(p){$ok:v "ok";$err:e e.msg});
  let n=str.containsre("xa";p);
  io.println("unreachable");'
if "${TKC}" --allow-all --out c_bin c.tk >compile.log 2>&1; then
    ./c_bin >out.txt 2>err.txt; rc=$?
    msg="$(head -1 out.txt)"
    if [ "${rc}" = "1" ] && grep -q "^RT008: invalid regex pattern in str.containsre: ${msg//[()\`]/.} (at byte 2 of" err.txt \
       && ! grep -q unreachable out.txt; then
        pass "NEGATIVE CONTROL: the same pattern straight into containsre is RT008, exit 1, same reason"
    else
        fail "negative control: rc ${rc}, stdout [$(cat out.txt)], stderr [$(cat err.txt)]"
    fi
else
    fail "negative control did not compile"
fi

# A literal invalid pattern: still E4075 for a matching call, not for checkre.
cat > lit_match.tk <<'EOF'
m=p;
i=str:std.str;
f=main():i64{
  let n=str.containsre("xa";"a(b");
  <0
};
EOF
if "${TKC}" --check lit_match.tk >lit.log 2>&1; then
    fail "literal a(b in containsre passed --check (E4075 lost)"
elif grep -q '"error_code":"E4075"' lit.log; then
    pass "literal invalid pattern in containsre is still E4075"
else
    fail "literal a(b in containsre: expected E4075, got $(grep -o '"error_code":"[^"]*"' lit.log | head -1)"
fi

cat > lit_check.tk <<'EOF'
m=p;
i=io:std.io;
i=str:std.str;
f=main():i64{
  io.println(mt str.checkre("a(b"){$ok:v "ok";$err:e e.msg});
  <0
};
EOF
if "${TKC}" --allow-all --out lit_bin lit_check.tk >lit2.log 2>&1 \
   && [ "$(./lit_bin)" = 'unbalanced parenthesis: `(` has no closing `)`' ]; then
    pass "literal invalid pattern in checkre compiles and returns \$err"
else
    fail "literal a(b in checkre: $(grep -o '"error_code":"[^"]*"' lit2.log | head -1) $(./lit_bin 2>&1 | head -1)"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
