#!/usr/bin/env bash
# C310_db_connections_exec.sh — std.db: several connections, every statement
# of a db.exec runs or the call fails, and the error slot is truthful
# (story 138.6; loke handoff B6: T-30, T-40, T-22, R8, N-5).
#
# THE DEFECTS.
#   1. db.c kept one process-global sqlite3*: a second db.open replaced it,
#      returned the same "handle" (1), and every later query went to the
#      database opened last (T-30: ha=1 hb=1, table ta unreadable).
#   2. db_exec prepared only the first statement of its string and reported
#      ok: "CREATE TABLE x(a); CREATE INDEX ix_x ON x(a);" left no index (T-40).
#   3. row.i64/u64/f64/bool returned the bare value with 0 as the error
#      sentinel, so a stored 0 / 0.0 / false / COUNT(*)=0 read as $err (T-22,
#      N-5).  The glue now keeps @tk_current_error truthful on every return
#      (0 on success, a {DbErr tag, msg} box on failure), which is what lets a
#      compiler decide these calls on the slot.  Whether the compiler DOES so
#      is llvm.c's is_stdlib_slot_wrapper() list -- outside this suite's
#      stdlib; part 1 below asserts the glue half only.
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, db.c + db_glue.c under ASan+UBSan):
#     - two opens return two different handles; each handle reads its own db
#     - the default connection (handle-less calls) is the FIRST open, and a
#       second open does not repoint it; db.close moves it to the next one
#     - a closed handle is refused ($connection), and is never reused
#     - a multi-statement exec runs every statement; a failing statement is
#       $query naming its number; bind params with two statements are refused
#       before anything runs; BEGIN/ROLLBACK inside one string works
#     - row.i64/f64/bool of a stored 0 / 0.0 / false: value 0 AND slot 0;
#       a missing column: slot = box with tag 2 ($notfound)
#   Part 2 (toke programs, end to end):
#     - T-30's shape: ha != hb, table ta still readable after opening b
#     - db.execon/oneon/manyon/closeon on two files
#     - T-40's shape: table and index both exist after one exec
#
# Negative control: C310_STDLIB=<dir> builds both parts against another
# stdlib tree.  Measured 2026-09-27 with a copy of this tree carrying three
# injected defects -- db_exec_on stops after the first statement, conn_db(0)
# answers the LATEST connection, tk_row_i64_w does not clear the slot on
# success: 12 of 25 fail (T-30, T-40, every multi-statement case, both
# row.i64 slot cases).  Main's db.c (c15dd11) cannot link part 1 at all: it
# has no *on wrappers.
#
# Story: 138.6
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C310_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" db)" \
    || { echo "FAIL: stdlib link set: see stderr above"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c310_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C310: std.db connections, multi-statement exec, error slot (138.6)"
echo "------------------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "db.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_db_open_w(int64_t);   int64_t tk_db_close_w(void);
int64_t tk_db_closeon_w(int64_t);
int64_t tk_db_exec_w(int64_t, int64_t);   int64_t tk_db_execon_w(int64_t, int64_t, int64_t);
int64_t tk_db_one_w(int64_t, int64_t);    int64_t tk_db_oneon_w(int64_t, int64_t, int64_t);
int64_t tk_db_manyon_w(int64_t, int64_t, int64_t);
int64_t tk_row_i64_w(int64_t, int64_t);   int64_t tk_row_f64_w(int64_t, int64_t);
int64_t tk_row_bool_w(int64_t, int64_t);  int64_t tk_row_str_w(int64_t, int64_t);
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
#define S(x) ((int64_t)(intptr_t)(x))
static int64_t strarr(const char **v, int n) {   /* a toke [str]: len at [-1] */
    int64_t *b = calloc((size_t)n + 1, sizeof(int64_t));
    b[0] = n; for (int i = 0; i < n; i++) b[i + 1] = S(v[i]);
    return S(b + 1);
}
static const char *str_of(int64_t row, const char *col) {
    int64_t v = tk_row_str_w(row, S(col)); return v ? (const char *)(intptr_t)v : "(err)";
}
/* 138.6 (GB): exec reports its row count, 0 included; success is the slot. */
#define EXOK(call) ((void)(call), tk_current_error == 0)
int main(void) {
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int64_t none = strarr(NULL, 0);
    int64_t a = tk_db_open_w(S("a.db")), b = tk_db_open_w(S("b.db"));
    CHECK(a > 0 && b > 0 && a != b, "two db.open calls return two different handles");
    CHECK(EXOK(tk_db_execon_w(a, S("CREATE TABLE ta(v INTEGER); INSERT INTO ta VALUES (7)"), none)),
          "execon(a): create + insert");
    CHECK(EXOK(tk_db_execon_w(b, S("CREATE TABLE tb(v INTEGER)"), none)), "execon(b): create");
    int64_t r = tk_db_oneon_w(a, S("SELECT v FROM ta"), none);
    CHECK(r && !strcmp(str_of(r, "v"), "7"), "oneon(a) reads a's table");
    CHECK(tk_db_oneon_w(b, S("SELECT v FROM ta"), none) == 0, "oneon(b) does not see a's table");
    r = tk_db_one_w(S("SELECT v FROM ta"), none);
    CHECK(r != 0, "handle-less db.one still reads the FIRST database after a second open");
    CHECK(tk_db_manyon_w(b, S("SELECT * FROM tb"), none) != 0, "manyon(b) on an empty table is ok");

    /* multi-statement exec */
    int64_t n = EXOK(tk_db_exec_w(S("CREATE TABLE x(a TEXT); CREATE INDEX ix_x ON x(a); -- tail\n"), none));
    int64_t q = tk_db_one_w(S("SELECT count(*) AS n FROM sqlite_master WHERE name='ix_x'"), none);
    CHECK(n != 0 && q && !strcmp(str_of(q, "n"), "1"), "exec runs the second statement (the index exists)");
    tk_current_error = 0;
    n = tk_db_exec_w(S("CREATE TABLE y(a); SELECT nope FROM nowhere; CREATE TABLE z(a)"), none);
    int64_t *box = (int64_t *)(intptr_t)tk_current_error;
    CHECK(n == 0 && box && box[0] == DB_ERR_QUERY &&
          strstr((const char *)(intptr_t)box[1], "statement 2"),
          "a failing second statement is $query naming statement 2");
    q = tk_db_one_w(S("SELECT count(*) AS n FROM sqlite_master WHERE name IN ('y','z')"), none);
    CHECK(q && !strcmp(str_of(q, "n"), "1"), "... the first ran, the third did not");
    const char *pv[] = {"1", "2"};
    n = tk_db_exec_w(S("INSERT INTO ta VALUES (?); INSERT INTO ta VALUES (?)"), strarr(pv, 2));
    q = tk_db_one_w(S("SELECT count(*) AS n FROM ta"), none);
    CHECK(n == 0 && q && !strcmp(str_of(q, "n"), "1"),
          "bind params with two statements are refused before anything runs");
    n = EXOK(tk_db_exec_w(S("BEGIN; INSERT INTO ta VALUES (9); ROLLBACK;"), none));
    q = tk_db_one_w(S("SELECT count(*) AS n FROM ta"), none);
    CHECK(n != 0 && q && !strcmp(str_of(q, "n"), "1"), "BEGIN; ...; ROLLBACK in one string rolls back");
    n = EXOK(tk_db_exec_w(S("BEGIN"), none));
    n = n && EXOK(tk_db_exec_w(S("INSERT INTO ta VALUES (10)"), none));
    n = n && EXOK(tk_db_exec_w(S("COMMIT"), none));
    q = tk_db_one_w(S("SELECT count(*) AS n FROM ta"), none);
    CHECK(n && q && !strcmp(str_of(q, "n"), "2"), "BEGIN / INSERT / COMMIT as three calls commits");

    /* error slot of the scalar accessors */
    r = tk_db_one_w(S("SELECT 0 AS z, 0.0 AS d, 0 AS f, 5 AS k"), none);
    tk_current_error = 1;
    int64_t v = tk_row_i64_w(r, S("z"));
    CHECK(v == 0 && tk_current_error == 0, "row.i64 of a stored 0: value 0, slot cleared");
    tk_current_error = 1;
    v = tk_row_f64_w(r, S("d"));
    CHECK(v == 0 && tk_current_error == 0, "row.f64 of a stored 0.0: value 0.0, slot cleared");
    tk_current_error = 1;
    v = tk_row_bool_w(r, S("f"));
    CHECK(v == 0 && tk_current_error == 0, "row.bool of a stored false: value false, slot cleared");
    v = tk_row_i64_w(r, S("nosuch"));
    box = (int64_t *)(intptr_t)tk_current_error;
    CHECK(v == 0 && box && box[0] == DB_ERR_NOT_FOUND, "row.i64 of a missing column: $notfound box");
    q = tk_db_one_w(S("SELECT COUNT(*) AS n FROM ta WHERE v < 0"), none);
    tk_current_error = 1;
    v = tk_row_i64_w(q, S("n"));
    CHECK(v == 0 && tk_current_error == 0, "row.i64 of COUNT(*) = 0: value 0, slot cleared");

    /* close */
    CHECK(tk_db_closeon_w(b) == 1 && tk_db_closeon_w(b) == 0, "closeon(b) closes once, then reports false");
    tk_current_error = 0;
    CHECK(tk_db_execon_w(b, S("SELECT 1"), none) == 0 && tk_current_error &&
          ((int64_t *)(intptr_t)tk_current_error)[0] == DB_ERR_CONNECTION,
          "execon on a closed handle is $connection");
    int64_t c = tk_db_open_w(S(":memory:"));
    CHECK(c > b, "a new open never reuses a closed handle");
    tk_db_close_w();   /* closes a, the default */
    CHECK(tk_db_one_w(S("SELECT v FROM ta"), none) == 0 &&
          tk_db_one_w(S("SELECT 1 AS one"), none) != 0,
          "db.close closes the default; the next open connection becomes the default");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c \
        "${LINK[@]}" -lsqlite3 > cc.log 2>&1; then
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
run_tk() {   # name expected-substring
    local name=$1 want=$2 got
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        echo "  FAIL: ${name}.tk did not compile"; sed 's/^/        /' "${name}.log" | head -4; FAIL=$((FAIL + 1)); return
    fi
    got="$(./"${name}" --allow-all 2>&1)"
    if printf '%s' "${got}" | grep -a -q -F -- "${want}"; then
        echo "  PASS: ${name}: ${want}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1))
    fi
}
cat > t30.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=main():i64{
  let ha=mt db.open("t30a.db") { $ok:h h; $err:e 999 };
  let x1=mt db.exec("CREATE TABLE IF NOT EXISTS ta(v INTEGER); INSERT INTO ta VALUES (7)";@()) { $ok:n 0; $err:e 1 };
  let hb=mt db.open("t30b.db") { $ok:h h; $err:e 999 };
  let r2:str=mt db.one("SELECT v FROM ta";@()) { $ok:w "ok"; $err:e "err" };
  io.println("ha=\(ha) hb=\(hb) ta-after-b=\(r2) setup=\(x1)");
  <0
};
TOKE
run_tk t30 "ha=1 hb=2 ta-after-b=ok setup=0"
cat > on.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=main():i64{
  let ha=mt db.open("a.db") { $ok:h h; $err:e 0 };
  let hb=mt db.open("b.db") { $ok:h h; $err:e 0 };
  let x1=mt db.execon(ha;"CREATE TABLE ta(v INTEGER); INSERT INTO ta VALUES (7); INSERT INTO ta VALUES (8);";@()) { $ok:n n; $err:e 99 };
  let q1:str=mt db.oneon(hb;"SELECT count(*) AS n FROM ta";@()) { $ok:w "ok"; $err:e "err" };
  let q2:str=mt db.oneon(ha;"SELECT count(*) AS n FROM ta";@()) { $ok:w mt row.str(w;"n") { $ok:s s; $err:e "?" }; $err:e "err" };
  let rs=mt db.manyon(ha;"SELECT v FROM ta";@()) { $ok:w w.len; $err:e 99 };
  let c=db.closeon(hb);
  let q4:str=mt db.oneon(hb;"SELECT 1 AS n";@()) { $ok:w "ok"; $err:e "err" };
  io.println("x1=\(x1) ta-on-b=\(q1) ta-on-a=\(q2) many=\(rs) closed-after=\(q4)");
  <0
};
TOKE
run_tk on "x1=2 ta-on-b=err ta-on-a=2 many=2 closed-after=err"
cat > t40.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=main():i64{
  let o=mt db.open(":memory:") { $ok:c "open-ok"; $err:e "open-err" };
  let r=mt db.exec("CREATE TABLE x (a TEXT); CREATE INDEX ix_x ON x(a);";@()) { $ok:n "exec-ok"; $err:e "exec-err" };
  let k:str=mt db.one("SELECT count(*) AS n FROM sqlite_master WHERE name='ix_x'";@()) { $ok:w mt row.str(w;"n") { $ok:s s; $err:e "?" }; $err:e "q-err" };
  io.println("\(o) \(r) index_ix_x=\(k)");
  <0
};
TOKE
run_tk t40 "open-ok exec-ok index_ix_x=1"
# 138.6 zero-value half (Batch GB, llvm.c): row.* and db.exec are slot
# wrappers, so a stored 0 / 0.0 / false, COUNT(*)=0 and an exec that changed
# no rows are $ok with that value (T-22, N-5).  Red before llvm.c listed them
# (and while db_glue.c still reported an exec count of 0 as 1).
cat > t22.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=main():i64{
  let o=mt db.open(":memory:") { $ok:h 0; $err:e 1 };
  let r=mt db.one("SELECT 2.5 AS x, 0.0 AS z, 0 AS i, 0 = 1 AS b";@()) { $ok:row row; $err:e 0 };
  let a:str=mt row.f64(r;"x") { $ok:v "ok \((v*10.0) as i64)"; $err:e "err" };
  let b:str=mt row.f64(r;"z") { $ok:v "ok \((v*10.0) as i64)"; $err:e "err" };
  let c:str=mt row.f64(r;"nosuch") { $ok:v "ok"; $err:e "err" };
  let d:str=mt row.i64(r;"i") { $ok:v "ok \(v)"; $err:e "err" };
  let e:str=mt row.bool(r;"b") { $ok:v "ok \(v)"; $err:e "err" };
  io.println("x: \(a) | zero: \(b) | nosuch: \(c) | i0: \(d) | bfalse: \(e)");
  <o
};
TOKE
run_tk t22 "x: ok 25 | zero: ok 0 | nosuch: err | i0: ok 0 | bfalse: ok false"
cat > n5.tk <<'TOKE'
m=main;i=io:std.io;i=db:std.db;
f=main():i64{
  let o=mt db.open(":memory:") { $ok:h 0; $err:e 1 };
  let x=mt db.exec("CREATE TABLE t (v INTEGER)";@()) { $ok:n "ok \(n)"; $err:e "err" };
  let u=mt db.exec("UPDATE t SET v=1 WHERE v=5";@()) { $ok:n "ok \(n)"; $err:e "err" };
  let r=mt db.one("SELECT count(*) AS n FROM t";@()) { $ok:row row; $err:e 0 };
  let k:str=mt row.i64(r;"n") { $ok:v "ok \(v)"; $err:e "err" };
  io.println("create: \(x) | update0: \(u) | count0: \(k)");
  <o
};
TOKE
run_tk n5 "create: ok 0 | update0: ok 0 | count0: ok 0"

echo "------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
