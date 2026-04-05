/*
 * test_db.c — Unit tests for the std.db C library (Story 1.3.3).
 *
 * Build and run: make test-stdlib-db
 * Uses an in-memory SQLite database so no file I/O is needed.
 */

#include <stdio.h>
#include <string.h>
#include "../../src/stdlib/db.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* Empty params helper */
    StrArray no_params = {NULL, 0};

    /* db_open with in-memory database */
    ASSERT(db_open(":memory:") == 0, "db_open(:memory:) returns 0");

    /* CREATE TABLE — zero rows affected */
    U64Result cr = db_exec(
        "CREATE TABLE t(id INTEGER, name TEXT)", no_params);
    ASSERT(!cr.is_err,    "CREATE TABLE ok");
    ASSERT(cr.ok == 0,    "CREATE TABLE affects 0 rows");

    /* INSERT — one row affected */
    U64Result ins = db_exec(
        "INSERT INTO t VALUES(1,'hello')", no_params);
    ASSERT(!ins.is_err,   "INSERT ok");
    ASSERT(ins.ok == 1,   "INSERT affects 1 row");

    /* db_one — happy path */
    RowResult rr = db_one(
        "SELECT id,name FROM t WHERE id=1", no_params);
    ASSERT(!rr.is_err,    "db_one(id=1) ok");

    /* row accessors */
    StrResult sr = row_str(rr.ok, "name");
    ASSERT(!sr.is_err,    "row_str(name) ok");
    ASSERT_STREQ(sr.ok, "hello", "row_str(name)==hello");

    U64Result ur = row_u64(rr.ok, "id");
    ASSERT(!ur.is_err,    "row_u64(id) ok");
    ASSERT(ur.ok == 1,    "row_u64(id)==1");

    /* missing column returns NOT_FOUND */
    StrResult missing = row_str(rr.ok, "nonexistent");
    ASSERT(missing.is_err && missing.err.kind == DB_ERR_NOT_FOUND,
           "row_str(missing col) is NOT_FOUND");

    /* db_one with no matching row returns NOT_FOUND */
    RowResult nf = db_one(
        "SELECT * FROM t WHERE id=99", no_params);
    ASSERT(nf.is_err && nf.err.kind == DB_ERR_NOT_FOUND,
           "db_one(id=99) is NOT_FOUND");

    db_close();

    /* ── Story 29.2.1: prepared statements and transactions ─────────────────── */

    ASSERT(db_open(":memory:") == 0, "29.2.1 db_open(:memory:) for stmt/tx tests");

    /* Set up a fresh table for prepared-statement tests */
    U64Result cr2 = db_exec(
        "CREATE TABLE u(id INTEGER, val TEXT)", no_params);
    ASSERT(!cr2.is_err, "29.2.1 CREATE TABLE u ok");

    db_exec("INSERT INTO u VALUES(1,'alpha')", no_params);
    db_exec("INSERT INTO u VALUES(2,'beta')",  no_params);
    db_exec("INSERT INTO u VALUES(3,'gamma')", no_params);

    /* db_prepare ─────────────────────────────────────────────────────────── */
    StmtResult sr2 = db_prepare(0, "SELECT id,val FROM u ORDER BY id");
    ASSERT(!sr2.is_err,  "29.2.1 db_prepare ok");
    ASSERT(sr2.ok != NULL, "29.2.1 db_prepare returns non-NULL stmt");

    /* db_bind (no params for this query) ─────────────────────────────────── */
    BoolResult br = db_bind(sr2.ok, no_params);
    ASSERT(!br.is_err, "29.2.1 db_bind no_params ok");

    /* db_step row 1 ──────────────────────────────────────────────────────── */
    RowResult step1 = db_step(sr2.ok);
    ASSERT(!step1.is_err, "29.2.1 db_step row 1 ok");
    StrResult v1 = row_str(step1.ok, "val");
    ASSERT(!v1.is_err, "29.2.1 db_step row 1 val ok");
    ASSERT_STREQ(v1.ok, "alpha", "29.2.1 db_step row 1 val==alpha");

    /* db_step row 2 ──────────────────────────────────────────────────────── */
    RowResult step2 = db_step(sr2.ok);
    ASSERT(!step2.is_err, "29.2.1 db_step row 2 ok");
    StrResult v2 = row_str(step2.ok, "val");
    ASSERT_STREQ(v2.ok, "beta", "29.2.1 db_step row 2 val==beta");

    /* db_step row 3 ──────────────────────────────────────────────────────── */
    RowResult step3 = db_step(sr2.ok);
    ASSERT(!step3.is_err, "29.2.1 db_step row 3 ok");
    StrResult v3 = row_str(step3.ok, "val");
    ASSERT_STREQ(v3.ok, "gamma", "29.2.1 db_step row 3 val==gamma");

    /* db_step done sentinel ──────────────────────────────────────────────── */
    RowResult done = db_step(sr2.ok);
    ASSERT(done.is_err, "29.2.1 db_step done is_err==1");
    ASSERT_STREQ(done.err.msg, "done", "29.2.1 db_step done msg==done");

    /* db_finalize ────────────────────────────────────────────────────────── */
    db_finalize(sr2.ok);
    /* If we reach here without a crash the finalize succeeded */
    ASSERT(1, "29.2.1 db_finalize completes without crash");

    /* db_prepare with bound params ───────────────────────────────────────── */
    StmtResult sr3 = db_prepare(0, "SELECT id,val FROM u WHERE id=?");
    ASSERT(!sr3.is_err, "29.2.1 db_prepare with ? ok");
    const char *p1[1] = {"2"};
    StrArray one_param = {p1, 1};
    BoolResult br2 = db_bind(sr3.ok, one_param);
    ASSERT(!br2.is_err, "29.2.1 db_bind id=2 ok");
    RowResult stepP = db_step(sr3.ok);
    ASSERT(!stepP.is_err, "29.2.1 db_step bound param row ok");
    StrResult vP = row_str(stepP.ok, "val");
    ASSERT_STREQ(vP.ok, "beta", "29.2.1 db_step bound param val==beta");
    RowResult doneP = db_step(sr3.ok);
    ASSERT(doneP.is_err && doneP.err.kind == DB_ERR_NOT_FOUND,
           "29.2.1 db_step bound param done sentinel");
    db_finalize(sr3.ok);

    /* ── Transactions: commit ─────────────────────────────────────────────── */
    BoolResult beg1 = db_begin(0);
    ASSERT(!beg1.is_err, "29.2.1 db_begin ok");

    db_exec("INSERT INTO u VALUES(4,'delta')", no_params);

    BoolResult com1 = db_commit(0);
    ASSERT(!com1.is_err, "29.2.1 db_commit ok");

    /* Row should now be visible */
    RowResult committed = db_one("SELECT val FROM u WHERE id=4", no_params);
    ASSERT(!committed.is_err, "29.2.1 committed row visible after COMMIT");
    StrResult cv = row_str(committed.ok, "val");
    ASSERT_STREQ(cv.ok, "delta", "29.2.1 committed val==delta");

    /* ── Transactions: rollback ───────────────────────────────────────────── */
    BoolResult beg2 = db_begin(0);
    ASSERT(!beg2.is_err, "29.2.1 db_begin for rollback ok");

    db_exec("INSERT INTO u VALUES(5,'epsilon')", no_params);

    BoolResult rb = db_rollback(0);
    ASSERT(!rb.is_err, "29.2.1 db_rollback ok");

    /* Row should NOT be visible */
    RowResult rolled = db_one("SELECT val FROM u WHERE id=5", no_params);
    ASSERT(rolled.is_err && rolled.err.kind == DB_ERR_NOT_FOUND,
           "29.2.1 rolled-back row NOT visible after ROLLBACK");

    db_close();

    /* ── Story 29.2.2: metadata and result inspection ────────────────────────── */

    ASSERT(db_open(":memory:") == 0, "29.2.2 db_open(:memory:) for metadata tests");

    U64Result cr3 = db_exec(
        "CREATE TABLE m(id INTEGER PRIMARY KEY AUTOINCREMENT, val TEXT)",
        no_params);
    ASSERT(!cr3.is_err, "29.2.2 CREATE TABLE m ok");

    /* db_last_insert_id after INSERT */
    db_exec("INSERT INTO m(val) VALUES('first')", no_params);
    U64Result lid = db_last_insert_id(0);
    ASSERT(!lid.is_err, "29.2.2 db_last_insert_id ok");
    ASSERT(lid.ok == 1, "29.2.2 db_last_insert_id == 1");

    db_exec("INSERT INTO m(val) VALUES('second')", no_params);
    U64Result lid2 = db_last_insert_id(0);
    ASSERT(!lid2.is_err, "29.2.2 db_last_insert_id second ok");
    ASSERT(lid2.ok == 2, "29.2.2 db_last_insert_id == 2");

    /* db_affected_rows after UPDATE */
    db_exec("INSERT INTO m(val) VALUES('third')",  no_params);
    db_exec("INSERT INTO m(val) VALUES('fourth')", no_params);
    db_exec("UPDATE m SET val='updated' WHERE id >= 2", no_params);
    U64Result aff = db_affected_rows(0);
    ASSERT(!aff.is_err, "29.2.2 db_affected_rows ok");
    ASSERT(aff.ok == 3, "29.2.2 db_affected_rows == 3");

    /* db_columns from a SELECT row */
    RowResult mr = db_one("SELECT id, val FROM m WHERE id=1", no_params);
    ASSERT(!mr.is_err, "29.2.2 db_one for columns test ok");
    StrArray cols = db_columns(mr.ok);
    ASSERT(cols.len == 2, "29.2.2 db_columns len == 2");
    ASSERT(cols.data != NULL, "29.2.2 db_columns data non-NULL");
    ASSERT_STREQ(cols.data[0], "id",  "29.2.2 db_columns[0] == id");
    ASSERT_STREQ(cols.data[1], "val", "29.2.2 db_columns[1] == val");

    /* db_is_null: INSERT NULL value then check */
    db_exec("INSERT INTO m(id, val) VALUES(10, NULL)", no_params);
    RowResult nr = db_one("SELECT id, val FROM m WHERE id=10", no_params);
    ASSERT(!nr.is_err, "29.2.2 db_one NULL row ok");
    ASSERT(db_is_null(nr.ok, "val") == 1,  "29.2.2 db_is_null(val) == 1 for NULL");
    ASSERT(db_is_null(nr.ok, "id")  == 0,  "29.2.2 db_is_null(id)  == 0 for non-NULL");

    /* db_table_exists */
    ASSERT(db_table_exists(0, "m")           == 1, "29.2.2 db_table_exists m == 1");
    ASSERT(db_table_exists(0, "no_such_tbl") == 0, "29.2.2 db_table_exists no_such_tbl == 0");

    db_close();

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
