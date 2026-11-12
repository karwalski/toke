#!/usr/bin/env bash
# C1280_tkir_section_caps.sh -- `--emit-tkir` writes every type and data
# entry a module needs, and refuses one past the format limit with E9010
# instead of writing a wrong index (story 127.344; AGENTS.md §6).
#
# THE DEFECT.  src/tkir.c kept the type and data tables as fixed arrays of
# 256 and 1024 entries.  Past them enc_add_i64 / enc_add_str /
# enc_ensure_scalar / enc_type_idx_from_name / enc_compound / enc_map returned
# index 0, so a module with 1200 distinct integer literals was written with
# data_count=1024, 178 CONST_DATA operands pointing at entry 0, and exit 0.
#
# NOW the tables grow on demand up to the format's own limit -- the sections
# store their counts and type references as u16, so 65535 entries -- and a
# module past it gets E9010 ("compiler limit exceeded"), exit 1, no file.
#
# CASES.
#   1. The 127.344 repro (12 functions each returning a sum of 100 distinct
#      literals; main returns their total mod 11): --emit-tkir exits 0,
#      `--read-tkir` reports Data: 1201 (1200 + the 11), and the binary built
#      --from-tkir exits like the one built from source (10).
#   2. 400 struct types, each with a field of the previous one: Types: 405
#      (5 scalars + 400), and entries past 256 are listed.
#   3. Data boundary: 65534 distinct literals + main's 0 = 65535 entries ->
#      exit 0, Data: 65535.
#   4. One more literal -> exit 1, an E9010 JSON diagnostic naming the data
#      section limit, and no .tkir written.
#
# The type-section limit (65535) is enforced by the same helper but is not
# exercised here: reaching it takes 65531 struct declarations and ~14 s.
#
# Negative control (run 2026-11-10): with ENC_MAX_DATA set back to 1024 in
# src/tkir.c, cases 1, 3 and 4 FAIL (1 and 3 are refused with E9010 at 1025
# entries; 4 names the limit 1024, not 65535); with ENC_MAX_TYPES set back to
# 256, case 2 FAILS.  On the base tree (96fd1c27) the 127.344 repro's .tkir
# decodes with data_count=1024 and 178 CONST_DATA operands at index 0, exit 0.
#
# Story: 127.344

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1280_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1280: --emit-tkir type/data sections grow to the u16 limit, then E9010"
echo "-----------------------------------------------------------------------"

# gen_lits <count> <file>: functions g0.. returning sums of <count> distinct
# literals in all (600 per function, inside the nesting limit), plus main.
gen_lits() {
    awk -v n="$1" -v per="${3:-600}" 'BEGIN {
        print "m=lits;"; v = 100000; f = 0
        while (n > 0) {
            k = (n < per) ? n : per; n -= k
            line = "f=g" f "():i64{<"
            for (i = 0; i < k; i++) line = line (i ? "+" : "") (v + i)
            print line "};"; v += k; f++
        }
        print "f=main():i64{<0};"
    }' > "$2"
}

data_count() { "${TKC}" --read-tkir "$1" 2>/dev/null | sed -n 's/^Data: //p'; }

# ── 1. the 127.344 repro ────────────────────────────────────────────────────
awk 'BEGIN {
    print "m=many;"; v = 1000
    for (f = 0; f < 12; f++) {
        line = "f=g" f "():i64{<"
        for (i = 0; i < 100; i++) line = line (i ? "+" : "") (v++)
        print line "};"
    }
    line = "f=main():i64{<("
    for (f = 0; f < 12; f++) line = line (f ? "+" : "") "g" f "()"
    print line ")%11};"
}' > "${WORK}/many.tk"
if "${TKC}" "${WORK}/many.tk" --emit-tkir --out "${WORK}/many.tkir" >"${WORK}/c1.log" 2>&1; then
    got="$(data_count "${WORK}/many.tkir")"
    if [ "${got}" = "1201" ]; then
        pass "1200 distinct literals + 11 -> Data: 1201 (was capped at 1024)"
    else
        fail "1200 distinct literals: expected Data: 1201, got '${got}'"
    fi
    if "${TKC}" "${WORK}/many.tk" --out "${WORK}/many_src" >"${WORK}/c1b.log" 2>&1 &&
       "${TKC}" --from-tkir "${WORK}/many.tkir" --out "${WORK}/many_ir" >>"${WORK}/c1b.log" 2>&1; then
        "${WORK}/many_src" </dev/null; a=$?
        "${WORK}/many_ir"  </dev/null; b=$?
        if [ "${a}" -eq "${b}" ]; then
            pass "built from source and from .tkir: same exit (${a})"
        else
            fail "built from source exits ${a}, from .tkir ${b}"
        fi
    else
        fail "could not build the repro: $(head -c 300 "${WORK}/c1b.log")"
    fi
else
    fail "--emit-tkir on the repro failed: $(head -c 300 "${WORK}/c1.log")"
fi

# ── 2. more than 256 type entries ───────────────────────────────────────────
awk 'BEGIN {
    print "m=tys;"; L = "abcdefghijklmnopqrstuvwxyz"
    for (i = 0; i < 400; i++) {
        n = "T" substr(L, int(i / 26) % 26 + 1, 1) substr(L, i % 26 + 1, 1)
        if (i == 0) print "t=" n "{x:i64};"
        else        print "t=" n "{x:i64;p:" prev "};"
        prev = n
    }
    print "f=main():i64{<7};"
}' > "${WORK}/tys.tk"
if "${TKC}" "${WORK}/tys.tk" --emit-tkir --out "${WORK}/tys.tkir" >"${WORK}/c2.log" 2>&1; then
    "${TKC}" --read-tkir "${WORK}/tys.tkir" >"${WORK}/tys.dump" 2>&1
    if grep -qx 'Types: 405' "${WORK}/tys.dump" &&
       grep -q "^  \[404\] struct 'Tpj' fields=2$" "${WORK}/tys.dump"; then
        pass "400 struct types -> Types: 405, entry 404 is the last struct"
    else
        fail "400 struct types: $(grep '^Types' "${WORK}/tys.dump")"
    fi
else
    fail "--emit-tkir on 400 structs failed: $(head -c 300 "${WORK}/c2.log")"
fi

# ── 3. exactly at the data limit ────────────────────────────────────────────
gen_lits 65534 "${WORK}/edge.tk"
if "${TKC}" "${WORK}/edge.tk" --emit-tkir --out "${WORK}/edge.tkir" >"${WORK}/c3.log" 2>&1; then
    got="$(data_count "${WORK}/edge.tkir")"
    if [ "${got}" = "65535" ]; then
        pass "65535 data entries (the u16 limit) -> written, Data: 65535"
    else
        fail "65535 data entries: expected Data: 65535, got '${got}'"
    fi
else
    fail "65535 data entries refused: $(head -c 300 "${WORK}/c3.log")"
fi

# ── 4. one past the data limit ──────────────────────────────────────────────
gen_lits 65535 "${WORK}/over.tk"
"${TKC}" "${WORK}/over.tk" --emit-tkir --out "${WORK}/over.tkir" >"${WORK}/c4.log" 2>&1
rc=$?
if [ "${rc}" -eq 1 ] &&
   grep -q '"error_code":"E9010"' "${WORK}/c4.log" &&
   grep -q 'more than 65535 data entries' "${WORK}/c4.log" &&
   [ ! -e "${WORK}/over.tkir" ]; then
    pass "65536 data entries -> E9010, exit 1, no .tkir written"
else
    fail "65536 data entries: rc=${rc}, file $( [ -e "${WORK}/over.tkir" ] && echo written || echo absent): $(head -c 300 "${WORK}/c4.log")"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
