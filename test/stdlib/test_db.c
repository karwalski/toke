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

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
