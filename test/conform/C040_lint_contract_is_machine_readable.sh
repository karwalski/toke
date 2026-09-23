#!/usr/bin/env bash
# C040_lint_contract_is_machine_readable.sh — the two halves of `tkc --lint`
# that a machine consumes, the exit code and the `fix` object, must mean what
# they say (stories 127.137, 127.138).
#
# ── 127.137: THE EXIT CODE ────────────────────────────────────────────────
#
# THE DEFECT. `run_lint_mode` ended `if (!do_fix) return 0;`. Measured on
# 2026-09-24, tkc 2.8.0:
#
#   $ tkc --lint r137.tk
#   {... "code":"discarded-value-result","severity":"error", ...}
#   $ echo $?
#   0
#
# The linter printed `"severity":"error"` and exited 0 in the same breath.
# The exit code is the obvious way to wire a linter into CI, so any such step
# passed on a program the linter had just called erroneous. Nothing in this
# repository was fooled, because `make lint` is the C static analyser under a
# confusingly similar name and has nothing to do with `tkc --lint` — which is
# precisely why the next person to wire it up would have been.
#
# THE CONTRACT NOW. The two spellings agree: a `severity:"error"` diagnostic
# left standing in the file exits non-zero (ECOMPILE, 1); hints and warnings
# exit 0. Under `--fix` an error the fixpoint rewrote away is not a failure,
# but `--dry-run` writes nothing, so it still reports on the file on disk.
#
# ── 127.138: THE fix OBJECT ───────────────────────────────────────────────
#
# THE ROW'S PREMISE WAS WRONG, and this suite is the thing that settles it.
# `fix.replacement` for `single-use-let` reads as though it were truncated —
#
#   "replacement":"  io.println(m.get(\"k\")"          (no `.get(0))`)
#
# — but it is not a statement, it is the text for the span `fix.span` names,
# and that span deliberately ENDS at the end of the use identifier, leaving
# the tail of the call in place. Applied at its span it produces exactly what
# `tkc --lint --fix` writes, byte for byte. Read without the span it looks
# broken; read with it, it is correct. A `replacement` is never meaningful on
# its own, and nothing in this repository had ever applied one to find out.
#
# So the assertion this suite adds is the one whose absence let the two paths
# be *suspected* of drifting and could not settle it: an INDEPENDENT applier,
# written against the published JSON alone, is run to a fixpoint over every
# `.tk` file in the tree that compiles, and its result is required to be
# byte-identical to what `tkc --lint --fix` writes AND to still compile.
# Two implementations of one contract, compared on every file that exercises
# it — which is what "machine-readable" has to mean to be worth claiming.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. EXIT CODE FOLLOWS SEVERITY. An error-severity diagnostic exits
#      non-zero; a hint-only file and a clean file exit 0. Case 1b is the
#      control that stops "exit non-zero on any diagnostic" from passing.
#   2. --fix AND --dry-run. A fixed error exits 0; --dry-run, which writes
#      nothing, still exits non-zero.
#   3. THE FIX OBJECT APPLIED, NOT COMPARED AS A STRING. Over the whole
#      tree: apply the JSON, require the result to compile and to equal the
#      built-in rewriter's output exactly.
#   4. A NEGATIVE CONTROL for case 3 — a deliberately corrupted replacement
#      must make it go red. A gate over 200-odd files that has never been
#      seen to fail is not evidence.
#   5. --fix MUST NOT STOP SHORT IN SILENCE. The 16-round cap is a fair loop
#      guard; reaching it without saying so contradicts this function's own
#      "a second --fix is a no-op". Case 5c is the control that stops the
#      note from being printed unconditionally.
#
# Stories: 127.137, 127.138

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0

expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ── Fixtures ─────────────────────────────────────────────────────────────

# One diagnostic, severity "error": a bare `push` whose result is discarded.
cat > "${WORK}/err.tk" <<'EOF'
m=test;
f=main():i64{
  let xs=mut.@(1;2);
  xs.push(3);
  <0
};
EOF

# Hints only: two single-use-let bindings, nothing of error severity.
cat > "${WORK}/hint.tk" <<'EOF'
m=test;
i=io:std.io;
f=main():i64{
  let m=@("k":@("a";"b"));
  let s=m.get("k");
  io.println(s.get(0));
  <0
};
EOF

# No diagnostics at all.
cat > "${WORK}/clean.tk" <<'EOF'
m=test;
f=main():i64{
  <0
};
EOF

# ── 1. exit code follows severity ────────────────────────────────────────

err_json="$("${TKC}" --lint "${WORK}/err.tk" 2>/dev/null)"; err_rc=$?
expect "1a-error-severity-is-emitted" \
    "$(printf '%s' "${err_json}" | grep -c '"severity":"error"' || true)" "1"
expect "1a-error-exits-nonzero" \
    "$([ "${err_rc}" -ne 0 ] && echo nonzero || echo zero)" "nonzero"

hint_json="$("${TKC}" --lint "${WORK}/hint.tk" 2>/dev/null)"; hint_rc=$?
# The control that keeps 1a honest: this file HAS diagnostics, just none of
# error severity. If the exit code were "non-zero on any diagnostic", 1a would
# pass while meaning nothing, and this case would fail.
expect "1b-hints-are-emitted" \
    "$([ "$(printf '%s' "${hint_json}" | grep -c '"severity":"hint"' || true)" -ge 1 ] \
        && echo yes || echo no)" "yes"
expect "1b-no-error-severity" \
    "$(printf '%s' "${hint_json}" | grep -c '"severity":"error"' || true)" "0"
expect "1b-hints-exit-zero" "${hint_rc}" "0"

"${TKC}" --lint "${WORK}/clean.tk" >/dev/null 2>&1
expect "1c-clean-exits-zero" "$?" "0"

# ── 2. --fix and --dry-run ───────────────────────────────────────────────

cp "${WORK}/err.tk" "${WORK}/err_fixed.tk"
"${TKC}" --lint --fix "${WORK}/err_fixed.tk" >/dev/null 2>&1
expect "2a-fixed-error-exits-zero" "$?" "0"
# ...and the fix really was applied, so 2a is not passing on an unfixed file.
expect "2a-fix-landed" \
    "$(grep -c 'xs=xs\.push(3)' "${WORK}/err_fixed.tk" || true)" "1"

cp "${WORK}/err.tk" "${WORK}/err_dry.tk"
"${TKC}" --lint --fix --dry-run "${WORK}/err_dry.tk" >/dev/null 2>&1
dry_rc=$?
expect "2b-dry-run-exits-nonzero" \
    "$([ "${dry_rc}" -ne 0 ] && echo nonzero || echo zero)" "nonzero"
if cmp -s "${WORK}/err.tk" "${WORK}/err_dry.tk"; then
    echo "PASS 2b-dry-run-wrote-nothing"
    PASS=$((PASS + 1))
else
    echo "FAIL 2b-dry-run-wrote-nothing"
    FAIL=$((FAIL + 1))
fi

# ── 3+4. the fix object, applied ─────────────────────────────────────────
#
# apply_fixes.py is an INDEPENDENT reimplementation of the published contract:
# read the JSON, splice each `fix.replacement` over `fix.span`, right to left,
# skipping any fix that overlaps one already applied — and nothing else. It
# shares no code with src/main.c's rewriter. That is the point: if the two
# ever disagree, one of them is wrong and this goes red.

cat > "${WORK}/apply_fixes.py" <<'PY'
import json, os, subprocess, sys, tempfile

tkc, path = sys.argv[1], sys.argv[2]
corrupt = len(sys.argv) > 3 and sys.argv[3] == "corrupt"

scratch = tempfile.NamedTemporaryFile(suffix=".tk", delete=False)
scratch.close()

def lint(buf):
    with open(scratch.name, "wb") as fh:
        fh.write(buf)
    p = subprocess.run([tkc, "--lint", scratch.name],
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    diags = []
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                diags.append(json.loads(line))
            except ValueError:
                pass
    return diags

def apply_round(buf, diags):
    fixes = [d["fix"] for d in diags if "fix" in d]
    # span_start descending, then span_end descending — stable among full ties,
    # matching the selection sort in lint_apply_fixes().
    fixes.sort(key=lambda f: (-f["span"]["start"], -f["span"]["end"]))
    applied, min_start = 0, -1
    for f in fixes:
        s, e = f["span"]["start"], f["span"]["end"]
        rep = f["replacement"].encode()
        if corrupt:
            rep = rep[:-1] if len(rep) > 1 else rep + b"@"
        if s < 0 or e > len(buf) or s > e:
            continue
        if min_start >= 0 and e > min_start:
            continue
        buf = buf[:s] + rep + buf[e:]
        min_start = s
        applied += 1
    return buf, applied

try:
    buf = open(path, "rb").read()
    # Run to a real fixpoint. src/main.c caps its own loop at 16 rounds AFTER
    # the first application, and test/stress/stress_many_locals.tk needs 17 —
    # so a budget-for-budget race between the two reports a difference that is
    # about iteration counts, not about the contract. The question this gate
    # asks is "does applying the published JSON reach the same place", so both
    # sides are driven to a fixpoint (see the caller's re-run loop).
    for _ in range(64):
        buf, n = apply_round(buf, lint(buf))
        if n == 0:
            break
    sys.stdout.buffer.write(buf)
finally:
    os.unlink(scratch.name)
PY

# Candidate corpus, built once: every .tk in the tree that compiles AND emits
# at least one fixable diagnostic. Anything else exercises nothing here.
: > "${WORK}/candidates.txt"
while IFS= read -r f; do
    "${TKC}" --check "$f" >/dev/null 2>&1 || continue
    "${TKC}" --lint "$f" 2>/dev/null | grep -q '"fix"' || continue
    echo "$f" >> "${WORK}/candidates.txt"
done < <(find "${REPO_ROOT}" -name '*.tk' -type f ! -path '*/.git/*' | sort)

sweep() {
    # sweep <list-file> [corrupt] → "<checked> <mismatch> <broken>"
    local list="$1" mode="${2:-}"
    local checked=0 mismatch=0 broken=0 f
    while IFS= read -r f; do
        checked=$((checked + 1))
        python3 "${WORK}/apply_fixes.py" "${TKC}" "$f" ${mode} \
            > "${WORK}/json.tk" 2>/dev/null
        cp "$f" "${WORK}/builtin.tk"
        # Drive the built-in rewriter to its own fixpoint too: its internal
        # loop stops after 16 rounds, so one invocation is not always the end.
        local i
        for i in 1 2 3 4 5 6 7 8; do
            cp "${WORK}/builtin.tk" "${WORK}/builtin.prev"
            "${TKC}" --lint --fix "${WORK}/builtin.tk" >/dev/null 2>&1
            cmp -s "${WORK}/builtin.prev" "${WORK}/builtin.tk" && break
        done
        if ! cmp -s "${WORK}/json.tk" "${WORK}/builtin.tk"; then
            mismatch=$((mismatch + 1))
            [ "${mismatch}" -le 3 ] && echo "        drift: $f" >&2
        fi
        "${TKC}" --check "${WORK}/json.tk" >/dev/null 2>&1 || {
            broken=$((broken + 1))
            [ "${broken}" -le 3 ] && echo "        broken: $f" >&2
        }
    done < "$list"
    echo "${checked} ${mismatch} ${broken}"
}

read -r N_CHECKED N_MISMATCH N_BROKEN <<< "$(sweep "${WORK}/candidates.txt")"

# The gate must not be able to pass vacuously: with an empty corpus 3a/3b
# assert nothing at all.
expect "3-corpus-is-not-empty" \
    "$([ "${N_CHECKED}" -ge 50 ] && echo enough || echo "only ${N_CHECKED}")" "enough"
expect "3a-json-matches-builtin-rewriter" "${N_MISMATCH}" "0"
expect "3b-applied-fixes-still-compile" "${N_BROKEN}" "0"
echo "      (swept ${N_CHECKED} files that compile and emit at least one fix)"

# ── 4. negative control ──────────────────────────────────────────────────
#
# Corrupt every replacement by one byte and require the sweep to notice. An
# applier that silently did nothing would also report 0 mismatches above, so
# 4a asserts the injection actually landed before 4b reads anything into it.

head -20 "${WORK}/candidates.txt" > "${WORK}/control.txt"

cat > "${WORK}/ctl.tk" <<'EOF'
m=test;
i=io:std.io;
f=main():i64{
  let m=@("k":@("a";"b"));
  let s=m.get("k");
  io.println(s.get(0));
  <0
};
EOF
python3 "${WORK}/apply_fixes.py" "${TKC}" "${WORK}/ctl.tk"         > "${WORK}/ctl_ok.tk"  2>/dev/null
python3 "${WORK}/apply_fixes.py" "${TKC}" "${WORK}/ctl.tk" corrupt > "${WORK}/ctl_bad.tk" 2>/dev/null
if cmp -s "${WORK}/ctl_ok.tk" "${WORK}/ctl_bad.tk"; then
    echo "FAIL 4a-injection-landed"
    echo "        corrupting every replacement changed nothing, so 4b would"
    echo "        be measuring an inert applier, not a gate that can fail"
    FAIL=$((FAIL + 1))
else
    echo "PASS 4a-injection-landed"
    PASS=$((PASS + 1))
fi

read -r C_CHECKED C_MISMATCH C_BROKEN <<< "$(sweep "${WORK}/control.txt" corrupt 2>/dev/null)"
expect "4b-control-gate-goes-red" \
    "$([ "${C_MISMATCH}" -gt 0 ] && echo red || echo green)" "red"
echo "      (control: ${C_MISMATCH} of ${C_CHECKED} mismatched, ${C_BROKEN} stopped compiling)"

# ── 5. --fix must not stop short in silence ─────────────────────────────
#
# Each round applies at most one fix per overlapping group, so a chain of N
# single-use bindings needs N rounds, and src/main.c caps the loop at 16 after
# the first application. The cap is a fair loop guard. Reaching it silently is
# not: run_lint_mode's own contract says "the cycle repeats until no fix
# applies (so a second --fix is a no-op)", and anything that ran --fix once and
# believed that got a partially fixed file with no indication. Measured before
# the fix: 22 chained bindings, one --fix, 5 left, exit 0, stderr silent.

python3 - "${WORK}/chain.tk" <<'GEN'
import sys
n = 22
L = ["m=t;", "f=main():i64{"]
L += ["  let v%d=%d;" % (i, i) for i in range(n)]
L += ["  <" + "+".join("v%d" % i for i in range(n)), "};"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
GEN

cp "${WORK}/chain.tk" "${WORK}/chain_fixed.tk"
chain_err="$("${TKC}" --lint --fix "${WORK}/chain_fixed.tk" 2>&1 >/dev/null)"
expect "5a-cap-is-reported" \
    "$(printf '%s' "${chain_err}" | grep -c 'stopped at the .*-round limit' || true)" "1"
# ...and the note is telling the truth: bindings really are left behind.
left="$(grep -c 'let v' "${WORK}/chain_fixed.tk" || true)"
expect "5a-cap-really-was-hit" \
    "$([ "${left}" -gt 0 ] && echo "left-behind" || echo "none-left")" "left-behind"
# Running again continues, exactly as the note says.
"${TKC}" --lint --fix "${WORK}/chain_fixed.tk" >/dev/null 2>&1
expect "5b-second-run-finishes" "$(grep -c 'let v' "${WORK}/chain_fixed.tk" || true)" "0"

# Control: a file that DOES reach its fixpoint in one run must stay silent,
# so 5a is measuring the cap and not a note printed unconditionally.
cp "${WORK}/hint.tk" "${WORK}/short_fixed.tk"
short_err="$("${TKC}" --lint --fix "${WORK}/short_fixed.tk" 2>&1 >/dev/null)"
expect "5c-no-note-when-fixpoint-reached" \
    "$(printf '%s' "${short_err}" | grep -c 'round limit' || true)" "0"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
