#!/usr/bin/env bash
# C058_migrate_never_drops_comments.sh — `tkc --migrate` must not delete a
# comment and report success (stories 127.160, 127.161).
#
# THE DEFECT. The migrator's prepass() strips every `(* *)`, `//` and `#`
# comment BEFORE it lexes, and all six postpasses compute offsets against the
# stripped buffer, so nothing downstream can put a comment back (127.161 —
# architectural; the design for real preservation is
# docs/architecture/migrate-comment-preservation.md). 127.57 declined on source
# that already compiles, which removed 121 of 124 affected files, but only
# because the code path stopped being entered. Two populations still reached
# it and lost every comment with exit 0:
#
#   * a valid file of a MULTI-FILE module (127.160). Its import cannot be
#     resolved by a standalone --check, so the 127.57 decline could not tell it
#     was clean: examples/bookmarks-api/{api,main}.tk, examples/mortgage/calc.tk.
#   * genuinely legacy source that carries comments — the migrator's real
#     customer, and every comment in it.
#
# THE DECISION. Until comments survive migration, the migrator refuses any
# source containing one: the source is echoed byte for byte (the redirect this
# mode is used through keeps the original), the exit code is non-zero, and
# stderr names the file, the number of comments and the first one's line.
# TKC_MIGRATE_FORCE=1 is the explicit consent to lose them. This needs no
# answer to 127.160's "is an import-only failure clean?" question: the
# multi-file files are protected because they have comments, not because a
# second heuristic guessed they were valid.
#
# WHAT IS ASSERTED:
#   1. The three 127.160 files: declined, echoed verbatim, file named.
#   2. Legacy source with all three comment forms: declined, echoed verbatim;
#      the count (3) and first line (2) in the message are right.
#   3. NOT A BLANKET REFUSAL: legacy source with no comment — but with `//`,
#      `#` and `(*` inside a string literal — still migrates, exit 0, output
#      changed and compiling, the string intact.
#   4. THE OVERRIDE, which is also the witness that the decline is load-
#      bearing: with TKC_MIGRATE_FORCE=1 the case-2 file migrates, compiles,
#      and has lost every comment.
#   5. TREE-WIDE: over every .tk in the repository, --migrate loses no comment
#      marker (`(*`, `//`, `#`, strings included on both sides, so a string
#      the migrator keeps cancels out).
#
# Negative control: run with TKC=<a compiler built without this change>;
# cases 1, 2 and 5 go red (see the 127.160/127.161 report).
#
# Story: 127.160, 127.161

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0
expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"; PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}
same() { cmp -s "$1" "$2" && echo same || echo differs; }
markers() { awk 'BEGIN{c=0} {c+=gsub(/\(\*/,""); c+=gsub(/\/\//,""); c+=gsub(/#/,"")} END{print c+0}' "$1"; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c058.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

# ── 1. the three multi-file-module files of 127.160 ──────────────────────
for rel in examples/bookmarks-api/api.tk examples/bookmarks-api/main.tk \
           examples/mortgage/calc.tk; do
    f="${REPO_ROOT}/${rel}"
    if [ ! -f "$f" ]; then expect "1-present $rel" missing present; continue; fi
    "${TKC}" --migrate "$f" > "${WORK}/o.tk" 2> "${WORK}/o.err"; rc=$?
    expect "1a-declined $rel" "$([ $rc -ne 0 ] && echo nonzero || echo zero)" nonzero
    expect "1b-verbatim $rel" "$(same "$f" "${WORK}/o.tk")" same
    expect "1c-names-file $rel" "$(grep -c "'${f}' has [0-9]* comment" "${WORK}/o.err")" 1
done

# ── 2. legacy source carrying all three comment forms ────────────────────
cat > "${WORK}/legacy_c.tk" <<'EOF'
M=legc;
// line comment
F=main():i64{
  # hash comment
  let x=1; (* block comment *)
  <x
};
EOF
"${TKC}" --check "${WORK}/legacy_c.tk" >/dev/null 2>&1
expect "2-premise-does-not-compile" "$([ $? -ne 0 ] && echo dirty || echo clean)" dirty
"${TKC}" --migrate "${WORK}/legacy_c.tk" > "${WORK}/lc.out" 2> "${WORK}/lc.err"; rc=$?
expect "2a-declined" "$([ $rc -ne 0 ] && echo nonzero || echo zero)" nonzero
expect "2b-verbatim" "$(same "${WORK}/legacy_c.tk" "${WORK}/lc.out")" same
expect "2c-count-and-line" "$(grep -c 'has 3 comment(s) (first at line 2)' "${WORK}/lc.err")" 1

# ── 3. not a blanket refusal; comment markers in strings are not comments ─
cat > "${WORK}/legacy_s.tk" <<'EOF'
M=legs;
i=io:std.io;
F=main():i64{
  io.println("http://x.y/#frag (*not*)");
  <0
};
EOF
"${TKC}" --check "${WORK}/legacy_s.tk" >/dev/null 2>&1
expect "3-premise-does-not-compile" "$([ $? -ne 0 ] && echo dirty || echo clean)" dirty
"${TKC}" --migrate "${WORK}/legacy_s.tk" > "${WORK}/ls.tk" 2>/dev/null
expect "3a-migrates-exit-0" "$?" 0
expect "3b-output-changed" "$(same "${WORK}/legacy_s.tk" "${WORK}/ls.tk")" differs
"${TKC}" --check "${WORK}/ls.tk" >/dev/null 2>&1
expect "3c-output-compiles" "$?" 0
expect "3d-string-intact" "$(grep -c '"http://x.y/#frag (\*not\*)"' "${WORK}/ls.tk")" 1

# ── 4. the override, and the witness that the decline is what protects ────
TKC_MIGRATE_FORCE=1 "${TKC}" --migrate "${WORK}/legacy_c.tk" > "${WORK}/lf.tk" 2>/dev/null
expect "4a-forced-exit-0" "$?" 0
"${TKC}" --check "${WORK}/lf.tk" >/dev/null 2>&1
expect "4b-forced-output-compiles" "$?" 0
expect "4c-forced-loses-comments" "$(markers "${WORK}/lf.tk")" 0

# ── 5. tree-wide: --migrate loses no comment marker ──────────────────────
n=0; lost=0
while IFS= read -r f; do
    n=$((n + 1))
    before="$(markers "$f")"
    [ "$before" -eq 0 ] && continue
    "${TKC}" --migrate "$f" > "${WORK}/t.tk" 2>/dev/null
    after="$(markers "${WORK}/t.tk")"
    if [ "$after" -lt "$before" ]; then
        lost=$((lost + 1))
        [ "$lost" -le 5 ] && echo "        loses comments: ${f#"${REPO_ROOT}/"} ($before -> $after)"
    fi
done < <(find "${REPO_ROOT}" -name '*.tk' -type f ! -path '*/.git/*' | sort)
expect "5-corpus-is-not-empty" "$([ "$n" -ge 100 ] && echo enough || echo "only $n")" enough
expect "5a-no-file-loses-a-comment" "$lost" 0
echo "      (swept $n .tk files)"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
