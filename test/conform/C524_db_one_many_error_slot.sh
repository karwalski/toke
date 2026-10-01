#!/usr/bin/env bash
# C524_db_one_many_error_slot.sh — db.one, db.many, db.oneon and db.manyon
# keep the error slot, so `$err:e` binds the DbErr payload (story 121.49
# item 4; residue of 138.6).
#
# THE DEFECT, measured on main (7014e929):
#   tk_db_one_w / tk_db_many_w / tk_db_oneon_w / tk_db_manyon_w returned a
#   bare 0 on failure and never wrote @tk_current_error, so they sat on
#   stdlib_tables.c's g_err_slot_bare[] list and the compiler decided them on
#   the value sentinel.  `$err:e` then bound 0: `mt e {$notfound:m ...}` on
#   a query that found no row loaded the sum tag through a null pointer.
#   Measured with part 2 below on a tkc and stdlib built from main: the
#   optimiser turned that undefined load into printing "ok" for every one of
#   the five failures (no RT005, no crash).  Only db.exec/execon (138.6)
#   carried the payload.
#
# NOW: the four glues store 0 on success and a {DbErr tag, msg} box on every
# failure (no row = $notfound, bad SQL = $query, bad handle = $connection),
# and their bare-list entries are deleted, so llvm.c derives them as slot
# wrappers (C375 checks the list is exact).
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, db.c + db_glue.c under ASan+UBSan): for each of the
#     four, the slot after success is 0 even when it held a stale error, and
#     after each failure is a box with the right tag and message.
#   Part 2 (a toke program, end to end): the same failures matched with
#     `mt e {$notfound:m ...; $query:m ...; ...}` print their variant and
#     message; an empty db.many is $ok with 0 rows; a success after a
#     failure is $ok.
#
# Negative control: C524_STDLIB=<dir> builds both parts against another
# stdlib tree and TKC=<tkc> picks the compiler.  Main's tree (7014e929) and
# its tkc: 18 of 20 fail -- all 13 of part 1 (the slot is never written, so
# the stale value survives a success too) and the five $err lines of part 2.
#
# Story: 121.49
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C524_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" --objects db -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c524_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C524: db.one / db.many / *on keep the error slot (121.49)"
echo "---------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "db.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_db_open_w(int64_t);
int64_t tk_db_exec_w(int64_t, int64_t);
int64_t tk_db_one_w(int64_t, int64_t);    int64_t tk_db_oneon_w(int64_t, int64_t, int64_t);
int64_t tk_db_many_w(int64_t, int64_t);   int64_t tk_db_manyon_w(int64_t, int64_t, int64_t);
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
#define S(x) ((int64_t)(intptr_t)(x))
static int64_t strarr(int n) { int64_t *b = calloc((size_t)n + 1, sizeof(int64_t)); b[0] = n; return S(b + 1); }
static int slot_is(int64_t tag, const char *needle) {
    int64_t *box = (int64_t *)(intptr_t)tk_current_error;
    if (!box || tk_current_error == 1) return 0;
    const char *m = (const char *)(intptr_t)box[1];
    return box[0] == tag && m && strstr(m, needle) != NULL;
}
#define STALE() (tk_current_error = 1)
int main(void) {
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int64_t none = strarr(0);
    int64_t h = tk_db_open_w(S(":memory:"));
    tk_db_exec_w(S("CREATE TABLE t(v INTEGER); INSERT INTO t VALUES (1)"), none);
    int64_t r;

    STALE(); r = tk_db_one_w(S("SELECT v FROM t"), none);
    CHECK(r && tk_current_error == 0, "db.one found a row: value, slot cleared");
    STALE(); r = tk_db_oneon_w(h, S("SELECT v FROM t"), none);
    CHECK(r && tk_current_error == 0, "db.oneon found a row: value, slot cleared");
    STALE(); r = tk_db_many_w(S("SELECT v FROM t"), none);
    CHECK(r && tk_current_error == 0, "db.many: value, slot cleared");
    STALE(); r = tk_db_manyon_w(h, S("SELECT v FROM t WHERE v = 99"), none);
    CHECK(r && tk_current_error == 0, "db.manyon with no rows: an empty array, slot cleared");

    tk_current_error = 0; r = tk_db_one_w(S("SELECT v FROM t WHERE v = 99"), none);
    CHECK(r == 0 && slot_is(DB_ERR_NOT_FOUND, "no rows"), "db.one with no row: $notfound \"no rows returned\"");
    tk_current_error = 0; r = tk_db_oneon_w(h, S("SELECT v FROM t WHERE v = 99"), none);
    CHECK(r == 0 && slot_is(DB_ERR_NOT_FOUND, "no rows"), "db.oneon with no row: $notfound");
    tk_current_error = 0; r = tk_db_one_w(S("SELECT nope FROM t"), none);
    CHECK(r == 0 && slot_is(DB_ERR_QUERY, "nope"), "db.one with bad SQL: $query naming the column");
    tk_current_error = 0; r = tk_db_many_w(S("SELECT nope FROM t"), none);
    CHECK(r == 0 && slot_is(DB_ERR_QUERY, "nope"), "db.many with bad SQL: $query naming the column");
    tk_current_error = 0; r = tk_db_manyon_w(h, S("SELECT nope FROM t"), none);
    CHECK(r == 0 && slot_is(DB_ERR_QUERY, "nope"), "db.manyon with bad SQL: $query");
    tk_current_error = 0; r = tk_db_oneon_w(0, S("SELECT 1"), none);
    CHECK(r == 0 && slot_is(DB_ERR_CONNECTION, "handle"), "db.oneon(0): $connection");
    tk_current_error = 0; r = tk_db_manyon_w(-3, S("SELECT 1"), none);
    CHECK(r == 0 && slot_is(DB_ERR_CONNECTION, "handle"), "db.manyon(-3): $connection");
    tk_current_error = 0; r = tk_db_oneon_w(h + 40, S("SELECT 1"), none);
    CHECK(r == 0 && slot_is(DB_ERR_CONNECTION, "handle"), "db.oneon on a handle never opened: $connection");
    tk_current_error = 0; r = tk_db_many_w(0, none);
    CHECK(r == 0 && slot_is(DB_ERR_QUERY, "null sql"), "db.many with a null sql: $query");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
if ! "${CC}" "${CF[@]}" -o t t.c "${LINK[@]}" -lsqlite3 > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a 'ERROR: AddressSanitizer\|runtime error' t.out | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
mkdir e2e && cd e2e
cat > slot.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=why(e:DbErr):str{ <mt e {$connection:m "connection: \(m)"; $query:m "query"; $notfound:m "notfound: \(m)"; $constraint:m "constraint"} };
f=main():i64{
  let o=mt db.open(":memory:") { $ok:h h; $err:e 0 };
  let x=mt db.exec("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1)";@()) { $ok:n 0; $err:e 1 };
  let a:str=mt db.one("SELECT v FROM t WHERE v = 99";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("one-norow: \(a)");
  let b:str=mt db.one("SELECT nope FROM t";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("one-badsql: \(b)");
  let c:str=mt db.many("SELECT nope FROM t";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("many-badsql: \(c)");
  let d:str=mt db.oneon(o;"SELECT v FROM t WHERE v = 99";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("oneon-norow: \(d)");
  let g:str=mt db.manyon(o+40;"SELECT 1";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("manyon-badhandle: \(g)");
  let k:str=mt db.many("SELECT v FROM t WHERE v = 99";@()) { $ok:w "ok \(w.len)"; $err:e why(e) };
  io.println("many-empty: \(k)");
  let z:str=mt db.one("SELECT v FROM t";@()) { $ok:w "ok"; $err:e why(e) };
  io.println("one-after-errors: \(z)");
  <x
};
TOKE
if ! TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out slot slot.tk > slot.log 2>&1; then
    echo "  FAIL: slot.tk did not compile"; sed 's/^/        /' slot.log | head -6; FAIL=$((FAIL + 1))
else
    ./slot --allow-all > slot.out 2>&1
    for want in "one-norow: notfound: no rows returned" \
                "one-badsql: query" \
                "many-badsql: query" \
                "oneon-norow: notfound: no rows returned" \
                "manyon-badhandle: connection: db: no open connection with that handle" \
                "many-empty: ok 0" \
                "one-after-errors: ok"; do
        if grep -a -q -x -F -- "${want}" slot.out; then
            echo "  PASS: ${want}"; PASS=$((PASS + 1))
        else
            key="${want%%:*}"
            echo "  FAIL: expected '${want}', got '$(grep -a -F -- "${key}:" slot.out | head -1)'"
            FAIL=$((FAIL + 1))
        fi
    done
    if [ "${FAIL}" -ne 0 ]; then sed 's/^/        | /' slot.out | head -12; fi
fi

echo ""
echo "C524: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
