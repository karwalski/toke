#!/usr/bin/env bash
# T007_every_module_declares_its_surface.sh — the three ways a `.tki` stopped
# describing the module it names, and the gates that now hold each shut.
#
# Stories: 137.12 (generate the interfaces), 136.47 (a registered module with
# no interface is a gate failure; withdraw the array combinators), 137.10 (one
# name under two kinds), 136.6 (arity reconciliation).
#
# ── 1. REGISTERED WITH NO INTERFACE (136.47) ────────────────────────────────
# `stdlib_module_registered()` accepts any module with a row in
# stdlib_table[], and `stdlib_symbol_for()` then falls through to the generic
# `tk_<mod>_<method>_w` rule, which answers a symbol for ANY spelling.  With
# no `.tki` there is nothing to check a member against in either direction, so
# a misspelling becomes a link error over a mangled name.  Nine modules were
# in that state (127.61's second cause).  The measured count matters here:
# the report said eight, and `infer_stream` is the ninth.
#
# ── 2. ONE NAME, TWO KINDS (137.10) ─────────────────────────────────────────
# `http.tki` declared http.get/post/put/delete as BOTH a `route` and a `func`.
# The resolver answers exactly one symbol per name — the route's — so no
# argument list could reach the function form, and the E4026 diagnostic cited
# the ROUTE's arity for a four-parameter client function, sending the reader
# to the wrong place.  Nothing rejected the shape, so it could recur; the gate
# is the fix and the four unreachable declarations are the cleanup.
#
# WHAT WAS FOUND WHILE FIXING IT, and is not in 137.10: the client-side glue
# is real.  tk_http_get_w / tk_http_post_w / tk_http_put_w / tk_http_delete_w
# all exist in tk_web_glue.c, alongside separate route wrappers
# (tk_http_posthandler_w and friends).  The explicit table in llvm.c maps
# `http.post` to the ROUTE symbol, so the client wrappers are unreachable
# under any spelling a caller can write.  Withdrawing the declarations states
# that truthfully; giving the client surface names of its own is a separate
# decision, and `http.postjson` / `http.postheaders` are the reachable
# spellings until it is made.
#
# ── 3. DOCUMENTED, IMPLEMENTED NOWHERE (136.47) ─────────────────────────────
# Fourteen `arr.*` combinators were documented and resolve to tk_array_*_w
# symbols that exist nowhere.  Documented-and-broken is worse than absent.
# Note what generation does here on its own: scripts/gen_tki.py adds only
# methods whose symbol is DEFINED, so the generated stdlib/array.tki carries
# the 19 members that work and cannot carry the 14 that do not.

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

echo "T007: every registered module declares the surface it really has"
echo "--------------------------------------"

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

GATE="python3 scripts/check_tki_coverage.py"

# ── 1. The gate is green on the tree as committed. ──
GATE_OUT="$(cd "${REPO_ROOT}" && ${GATE} 2>&1)"
GATE_RC=$?
if [ "${GATE_RC}" -eq 0 ]; then
    ok "check_tki_coverage.py is green"
else
    bad "check_tki_coverage.py is red"
    printf '%s' "${GATE_OUT}" | grep -E "^FAIL|^  std\.|^  http" | sed 's/^/      /' | head -12
fi

for want in \
    "names declared under two kinds in one file: 0" \
    "g_stdlib_decls entries disagreeing with the C definition: 0"
do
    if printf '%s' "${GATE_OUT}" | grep -q "${want}"; then
        ok "${want}"
    else
        bad "${want}"
    fi
done

# ── 2. THE NEGATIVE CONTROL that makes the 136.47 gate worth having: strip a
#      module's interface and the gate must go red NAMING that module. A gate
#      nobody has watched fail is not known to work. ──
echo "  ---- negative control: strip an interface, watch the gate go red ----"
VICTIM="${REPO_ROOT}/stdlib/csv.tki"
STASH="$(mktemp /tmp/tkc_t007_victim_XXXXXX)"
restore() { [ -f "${STASH}" ] && cp "${STASH}" "${VICTIM}" && rm -f "${STASH}"; }
trap restore EXIT
cp "${VICTIM}" "${STASH}"
rm -f "${VICTIM}"
STRIPPED="$(cd "${REPO_ROOT}" && ${GATE} 2>&1)"
STRIPPED_RC=$?
restore
trap - EXIT

if [ "${STRIPPED_RC}" -ne 0 ]; then
    ok "the gate fails when a registered module loses its interface"
else
    bad "the gate stayed green with std.csv's interface deleted"
fi
if printf '%s' "${STRIPPED}" | grep -q "^  std\.csv$"; then
    ok "the failure names std.csv"
else
    bad "the failure did not name std.csv"
    printf '%s' "${STRIPPED}" | grep -A4 "registered in stdlib_table" | sed 's/^/      /'
fi
if (cd "${REPO_ROOT}" && ${GATE} >/dev/null 2>&1); then
    ok "the gate is green again once the interface is restored"
else
    bad "the gate did not recover after restoring std.csv"
fi

# ── 3. Generation is idempotent and the tree is in sync with the table.
#      This is what stops the interfaces drifting again: the check runs in CI,
#      so an added wrapper with no declaration fails the build rather than
#      waiting for a downstream consumer to find it. ──
if (cd "${REPO_ROOT}" && python3 scripts/gen_tki.py --check >/dev/null 2>&1); then
    ok "stdlib/*.tki is in sync with the builtin table (gen_tki.py --check)"
else
    bad "stdlib/*.tki is behind the builtin table; run scripts/gen_tki.py"
    (cd "${REPO_ROOT}" && python3 scripts/gen_tki.py --check 2>&1) | tail -8 | sed 's/^/      /'
fi

# ── 4. 137.12's named instances: reachable from toke source, and now declared. ──
declared_in() {   # declared_in <file.tki> <export name>
    python3 - "$1" "$2" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
sys.exit(0 if any(e.get("name") == sys.argv[2] for e in d.get("exports", [])) else 1)
PY
}
for pair in "str.tki str.padright" "file.tki file.listglob" \
            "file.tki file.parsetoml" "process.tki process.spawndetached"; do
    set -- ${pair}
    if declared_in "${REPO_ROOT}/stdlib/$1" "$2"; then
        ok "$2 is declared in $1"
    else
        bad "$2 is reachable from toke source but absent from $1"
    fi
done

# json.tki exported 13 functions where the builtin table reaches 62 wrappers.
JSON_N="$(python3 -c "
import json;print(sum(1 for e in json.load(open('${REPO_ROOT}/stdlib/json.tki'))['exports'] if e.get('kind')=='func'))")"
if [ "${JSON_N}" -ge 60 ]; then
    ok "json.tki declares ${JSON_N} functions (was 13, table reaches 62)"
else
    bad "json.tki declares only ${JSON_N} functions"
fi

# ── 5. 137.10: no name under two kinds, and the four unreachable client
#      declarations are gone while the routes stay. ──
for n in http.get http.post http.put http.delete; do
    KINDS="$(python3 -c "
import json;print(' '.join(sorted({e.get('kind','?') for e in json.load(open('${REPO_ROOT}/stdlib/http.tki'))['exports'] if e.get('name')=='${n}'})))")"
    if [ "${KINDS}" = "route" ]; then
        ok "${n} is declared once, as the route the resolver actually answers"
    else
        bad "${n} is declared as [${KINDS}]"
    fi
done

# ── 6. 136.47: the 14 combinators are withdrawn from the documentation, and
#      the generated array interface does not carry them. ──
COMBINATORS="map fold filter each all any count first last max min reduce sort sum"
MISSING_OK=1
for c in ${COMBINATORS}; do
    if declared_in "${REPO_ROOT}/stdlib/array.tki" "array.${c}" 2>/dev/null; then
        bad "array.tki declares array.${c}, whose symbol exists nowhere"
        MISSING_OK=0
    fi
done
[ "${MISSING_OK}" = 1 ] && ok "generated array.tki carries none of the 14 absent combinators"

# The original form of this check grepped for a line STARTING with `arr.`,
# which no row in this table does -- the first cell is the bare symbol name
# -- so it always took the "no longer lists them" branch and asserted
# nothing.  Assert the thing that matters instead: every module-style row
# for one of the fourteen carries the marker, and none is missed.
CS="${REPO_ROOT}/docs/reference/combinator-status.md"
UNMARKED=""
for c in ${COMBINATORS}; do
    ROW="$(grep -F "\`arr.${c}(" "${CS}" || true)"
    [ -z "${ROW}" ] && continue
    printf '%s' "${ROW}" | grep -q "WITHDRAWN 136.47" || UNMARKED="${UNMARKED} ${c}"
done
if [ -z "${UNMARKED}" ]; then
    ok "every module-style arr.* row in combinator-status.md is marked WITHDRAWN 136.47"
else
    bad "combinator-status.md still presents these as available:${UNMARKED}"
fi

# The markdown table must stay a table: the marker goes INSIDE the notes
# column.  A checkpoint of this change appended it as an extra cell, which
# gave those rows an eighth column and broke the render.
# BOTH failure shapes count: an eighth cell, and -- the shape the checkpoint
# actually produced -- a trailing cell with no closing pipe.  A guard of the
# form `/\|$/` would skip the second, which is the one that happened.
BADCOLS="$(awk '
    /^\| / && /arr\./ {
        line = $0
        sub(/[ \t]+$/, "", line)
        n = gsub(/\|/, "|", line)
        if (line !~ /\|$/ || n - 1 != 7) printf "%s ", NR
    }' "${CS}")"
if [ -z "${BADCOLS}" ]; then
    ok "no combinator row has drifted off the table's 7 columns"
else
    bad "malformed table row(s) at line(s): ${BADCOLS}"
fi

# 136.47's other half: the idiom guide must no longer tell authors to prefer
# the module spelling.  That sentence is why the form kept reaching new code.
IG="${REPO_ROOT}/docs/spec/idiom-v0.4.md"
if grep -qE 'Prefer `arr\.' "${IG}"; then
    bad "idiom-v0.4.md still tells authors to prefer module-style arr.* combinators"
elif grep -q 'Do not use the module-style' "${IG}"; then
    ok "idiom-v0.4.md withdraws the module-style spelling and names the receiver form"
else
    bad "idiom-v0.4.md neither promises nor withdraws the module-style spelling"
fi

# ── 7. 136.6, the two the row put first. Their interfaces now state the
#      arity the implementation really has. NOTE what this does NOT claim:
#      src/types.c:1434 (`if (is_std) return;`) means a hand-written std
#      interface is never enforced against a call, so the pre-fix state was a
#      wrong document, not an unset register — the compiler rejected the
#      documented call either way. ──
arity_of() {
    python3 -c "
import json,sys
d=json.load(open('${REPO_ROOT}/stdlib/$1'))
print(next((len(e.get('params',[])) for e in d['exports']
            if e.get('name')=='$2' and e.get('kind')=='func'), -1))"
}
[ "$(arity_of dashboard.tki dashboard.serve)" = "2" ] \
    && ok "dashboard.serve declares 2 parameters, as tk_dashboard_serve_w takes" \
    || bad "dashboard.serve declares $(arity_of dashboard.tki dashboard.serve), not 2"

check_case() {
    local name="$1" exp_rc="$2" src="$3"
    local tmp; tmp="$(mktemp -d /tmp/tkc_t007_XXXXXX)"
    printf '%s\n' "${src}" > "${tmp}/case.tk"
    local rc=0
    ( cd "${tmp}" && "${TKC}" --check --diag-json case.tk ) >/dev/null 2>&1 || rc=$?
    rm -rf "${tmp}"
    [ "${rc}" = "${exp_rc}" ] && ok "${name}" || bad "${name} (expected ${exp_rc}, got ${rc})"
}
check_case "dashboard.serve(d;port) — the real signature — compiles" 0 '
m=t;
i=d:std.dashboard;
f=main():i64{
  let x=d.new("t");
  let r=d.serve(x;8080);
  <0
};'

# ── 8. Narrowing guard: 136.44 holds std.tls, so this change leaves tls.tki
#      exactly as it found it. Five of its declarations have no wrapper, and
#      the quarantine list is where that is recorded until 136.44 lands. ──
if (cd "${REPO_ROOT}" && git diff --quiet HEAD -- stdlib/tls.tki 2>/dev/null); then
    ok "stdlib/tls.tki is untouched (136.44 owns it)"
else
    bad "stdlib/tls.tki was modified; 136.44 owns that file"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
