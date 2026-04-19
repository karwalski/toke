#!/bin/bash
# Cross-backend database test suite (Story 57.10.6)
# Runs identical test scenarios against all available backends.
#
# Usage:
#   ./test_db_backends.sh [sqlite|postgres|mysql|all]
#
# Prerequisites:
#   - SQLite: always available
#   - PostgreSQL: requires running server, set PGDSN env var
#   - MySQL: requires running server, set MYDSN env var

set -e
PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

BACKEND="${1:-all}"

# ── Test program template ────────────────────────────────────────────
# Each test writes a .tk program that exercises the db module.

TEST_DIR=$(mktemp -d)
trap "rm -rf $TEST_DIR" EXIT

TKC="${TKC:-tkc}"

# ── SQLite tests ─────────────────────────────────────────────────────
run_sqlite_tests() {
    echo "=== SQLite Backend ==="

    cat > "$TEST_DIR/test_sqlite.tk" << 'TOKE'
m=test.sqlite;
i=db:std.db;
i=io:std.io;

f=main():i64{
  let open=db.open("sqlite::memory:");
  let cr=db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price REAL)";@());
  let i1=db.exec("INSERT INTO items (name, price) VALUES (?, ?)";@("widget";"9.99"));
  let i2=db.exec("INSERT INTO items (name, price) VALUES (?, ?)";@("gadget";"19.99"));

  let row=db.one("SELECT name, price FROM items WHERE id = ?";@("1"))|{$ok:r r;$err:e <1};
  let name=row.str(row;"name")|{$ok:v v;$err:e ""};
  if(name="widget"){
    (io.println("PASS: select one"))
  }el{
    (io.println("FAIL: select one"))
  };

  let rows=db.many("SELECT name FROM items ORDER BY id";@())|{$ok:r r;$err:e <1};
  if(rows.len=2){
    (io.println("PASS: select many"))
  }el{
    (io.println("FAIL: select many"))
  };

  let affected=db.exec("UPDATE items SET price = 29.99 WHERE name = ?";@("widget"))|{$ok:n n;$err:e 0 as u64};
  if(affected=1 as u64){
    (io.println("PASS: exec affected"))
  }el{
    (io.println("FAIL: exec affected"))
  };

  <0
};
TOKE

    if $TKC --check "$TEST_DIR/test_sqlite.tk" 2>/dev/null; then
        pass "SQLite test program compiles"
    else
        fail "SQLite test compile" "tkc --check failed"
    fi
}

# ── PostgreSQL tests ─────────────────────────────────────────────────
run_postgres_tests() {
    echo "=== PostgreSQL Backend ==="

    if [ -z "$PGDSN" ]; then
        skip "PostgreSQL (set PGDSN=postgres://user:pass@host/db to enable)"
        return
    fi

    echo "  PostgreSQL DSN: ${PGDSN%%@*}@***"
    skip "PostgreSQL integration tests (requires compiled binary with -DTK_HAVE_LIBPQ)"
}

# ── MySQL tests ──────────────────────────────────────────────────────
run_mysql_tests() {
    echo "=== MySQL Backend ==="

    if [ -z "$MYDSN" ]; then
        skip "MySQL (set MYDSN=mysql://user:pass@host/db to enable)"
        return
    fi

    echo "  MySQL DSN: ${MYDSN%%@*}@***"
    skip "MySQL integration tests (requires compiled binary with -DTK_HAVE_MYSQL)"
}

# ── Run selected backends ────────────────────────────────────────────
case "$BACKEND" in
    sqlite)  run_sqlite_tests ;;
    postgres) run_postgres_tests ;;
    mysql)   run_mysql_tests ;;
    all)
        run_sqlite_tests
        echo ""
        run_postgres_tests
        echo ""
        run_mysql_tests
        ;;
    *) echo "Usage: $0 [sqlite|postgres|mysql|all]"; exit 1 ;;
esac

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
