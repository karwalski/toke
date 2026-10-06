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
# THE FIRST DECISION (127.160). Until comments could survive migration, the
# migrator refused any source containing one (echoed, non-zero exit).
#
# THE FIX (127.161; owner decisions 2026-10-01 §2.6-§2.8). Comments survive.
# prepass() keeps the line map (a block comment leaves its newlines behind),
# and tkc_migrate_keep_comments() puts every comment back on its line:
# own-line, leading and trailing ones where they were, `//` and `#` rewritten
# as `(* *)`, and a comment with code on both sides on one line moved to the
# end of that line with a note on stderr. When a comment cannot be placed
# safely it declines (E9026, exit 4, source echoed): a rewrite that moved code
# across lines, a `(*` that never closes, a line comment holding `(*`/`*)`.
# TKC_MIGRATE_FORCE=1 is still the consent to lose them.
#
# WHAT IS ASSERTED:
#   1. The three 127.160 files (valid multi-file-module source): migrated,
#      exit 0, and byte-identical to the input -- every comment kept in place.
#   2. Legacy source with all three comment forms in own-line, leading,
#      trailing, interior and multi-line positions: exit 0, output compiles
#      with no W1020, every comment present as `(* *)` on its original line,
#      the interior one at the end of its line with a note naming that line.
#   3. NOT A BLANKET REWRITE: comment markers inside a string literal are not
#      comments -- the string survives intact.
#   4. THE OVERRIDE: with TKC_MIGRATE_FORCE=1 the case-2 file migrates,
#      compiles, and has lost every comment.
#   5. TREE-WIDE: over every .tk in the repository, --migrate loses no comment
#      marker (`(*`, `//`, `#`; a `//` or `#` rewritten as `(*` counts as kept).
#   6. INTERIOR COMMENTS IN EVERY REWRITE (design doc §1): keyword case, `$`
#      sigil, `[T]` -> `@T`, `a[i]` -> `.get`, `[..]` literal, `if c {` ->
#      `if(c){`, `=` -> `==`, a reassigned `let` -> `mut.`, `+` concat ->
#      interpolation, `s[i]` -> `.slice`: output compiles, every comment kept
#      on its own line, one note per moved comment.
#   7. THE GUARDS DECLINE BY NAME: a `+` chain split across lines (the
#      concat rewrite joins the lines) and a `(*` that never closes each give
#      exit 4, an E9026 naming the reason, and the source echoed verbatim.
#
# Negative control: run with TKC=<a compiler built before 127.161>; cases 1,
# 2, 6 and 7 go red (the old comment decline: exit 1/4 instead of 0, or a
# different reason).
#
# Story: 127.160, 127.161, 127.197

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
    "${TKC}" --migrate "$f" > "${WORK}/o.tk" 2> "${WORK}/o.err"
    expect "1a-migrated-exit-0 $rel" "$?" 0
    expect "1b-identical $rel" "$(same "$f" "${WORK}/o.tk")" same
done

# ── 2. legacy source carrying all three comment forms ────────────────────
cat > "${WORK}/legacy_c.tk" <<'EOF'
M=legc;
// line comment
F=main():i64{
  # hash comment
  let x=1; (* block comment *)
  (* leading *) let y=2;
  let z=(* interior *)3; // trailing
  (* multi
     line *)
  <x+y+z
};
EOF
"${TKC}" --check "${WORK}/legacy_c.tk" >/dev/null 2>&1
expect "2-premise-does-not-compile" "$([ $? -ne 0 ] && echo dirty || echo clean)" dirty
"${TKC}" --migrate "${WORK}/legacy_c.tk" > "${WORK}/lc.out" 2> "${WORK}/lc.err"
expect "2a-migrates-exit-0" "$?" 0
"${TKC}" --check "${WORK}/lc.out" > "${WORK}/lc.chk" 2>&1
expect "2b-output-compiles" "$?" 0
expect "2c-no-W1020" "$(grep -c W1020 "${WORK}/lc.chk")" 0
expect "2d-line-count-kept" "$(wc -l < "${WORK}/lc.out" | tr -d ' ')" "$(wc -l < "${WORK}/legacy_c.tk" | tr -d ' ')"
# each comment, on its original line, in its normalised form
lineof() { sed -n "${2}p" "$1"; }
expect "2e-line-comment"      "$(lineof "${WORK}/lc.out" 2)" "(* line comment *)"
expect "2f-hash-comment"      "$(lineof "${WORK}/lc.out" 4)" "  (* hash comment *)"
expect "2g-trailing"          "$(lineof "${WORK}/lc.out" 5)" "  let x=1; (* block comment *)"
expect "2h-leading"           "$(lineof "${WORK}/lc.out" 6)" "  (* leading *) let y=2;"
expect "2i-interior-moved"    "$(lineof "${WORK}/lc.out" 7)" "  let z=3; (* interior *) (* trailing *)"
expect "2j-multi-line"        "$(lineof "${WORK}/lc.out" 8)|$(lineof "${WORK}/lc.out" 9)" "  (* multi|     line *)"
expect "2k-note-names-line-7" "$(grep -c 'legacy_c.tk:7: note: comment moved to the end of its line' "${WORK}/lc.err")" 1

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

# ── 6. interior comments in every rewrite construct ──────────────────────
cat > "${WORK}/legacy_i.tk" <<'EOF'
M=legi;
I=io:std.io;
T=Point{x:i64;y:i64};
F=area(p:Point):i64{<p.x(* w *)*p.y};
F=pick(xs:[i64](* list *);i:i64):i64{<xs[i](* item *)};
F=cls(n:i64):i64{
  if n = 0 (* zero *) { <1 }el{ <2 };
  <0
};
F=main():i64{
  let xs=[1;(* two *)2;3];
  let n=0;
  n=n+(* inc *)1;
  let s="a" + (* glue *) "b";
  let c=s[0] (* first *);
  io.println(s);
  <area(Point{x:2;y:(* y *)3})+pick(xs;1)+cls(n)
};
EOF
"${TKC}" --migrate "${WORK}/legacy_i.tk" > "${WORK}/li.out" 2> "${WORK}/li.err"
expect "6a-migrates-exit-0" "$?" 0
"${TKC}" --check "${WORK}/li.out" >/dev/null 2>&1
expect "6b-output-compiles" "$?" 0
for pair in "4:w" "5:list" "5:item" "7:zero" "11:two" "13:inc" "14:glue" "15:first" "17:y"; do
    ln="${pair%%:*}"; word="${pair#*:}"
    expect "6c-kept-on-line-${ln}-${word}" \
        "$(sed -n "${ln}p" "${WORK}/li.out" | grep -cF "(* ${word} *)")" 1
done
expect "6d-one-note-per-moved-comment" "$(grep -c 'note: comment moved' "${WORK}/li.err")" 9
# the rewrites themselves still happened
for frag in 'p:$point' 'xs:@i64' 'xs.get(i)' 'if(n == 0)' '@(1;2;3)' 'mut.0' 'let s="ab"' 's.slice(0;1)'; do
    expect "6e-rewritten ${frag}" "$(grep -cF "${frag}" "${WORK}/li.out")" 1
done

# ── 7. the guards decline, by name ──────────────────────────────────────
cat > "${WORK}/legacy_g.tk" <<'EOF'
M=legg;
I=io:std.io;
F=main():i64{
  (* greeting *)
  let s="a" +
    "b";
  io.println(s);
  <0
};
EOF
"${TKC}" --migrate "${WORK}/legacy_g.tk" > "${WORK}/lg.out" 2> "${WORK}/lg.err"
expect "7a-line-map-guard-exit-4" "$?" 4
expect "7b-verbatim" "$(same "${WORK}/legacy_g.tk" "${WORK}/lg.out")" same
expect "7c-names-the-reason" "$(grep -c '"error_code":"E9026".*moved code across lines' "${WORK}/lg.err")" 1
printf 'M=legu;\n(* never closed\nF=main():i64{<0};\n' > "${WORK}/legacy_u.tk"
"${TKC}" --migrate "${WORK}/legacy_u.tk" > "${WORK}/lu.out" 2> "${WORK}/lu.err"
expect "7d-unclosed-exit-4" "$?" 4
expect "7e-verbatim" "$(same "${WORK}/legacy_u.tk" "${WORK}/lu.out")" same
expect "7f-names-line-2" "$(grep -c '"error_code":"E9026".*never closed.*"line":2,' "${WORK}/lu.err")" 1

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
