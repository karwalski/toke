# std.db -- Database Operations

## Overview

The `std.db` module provides functions for querying and mutating a SQL
database.  A connection is opened with `db.open` and closed with `db.close`.
Queries that return rows use `db.one` (single row) or `db.many` (all rows).
Statements that modify data use `db.exec`.  Parameterised queries accept a
`[Str]` array of positional bind parameters, preventing SQL injection.

Row values are opaque -- column data is extracted through typed accessor
functions (`row.str`, `row.u64`, `row.i64`, `row.f64`, `row.bool`).

Transactions are managed explicitly with `db.begin`, `db.commit`, and
`db.rollback`.  Prepared statements are created with `db.prepare`.

The current implementation uses SQLite as the backend.

---

## Types

### DbConn

An opaque handle representing an open database connection.  Returned by
`db.open` and consumed by all other `db.*` functions.  A program may hold
more than one `DbConn` at a time.

### Row

Represents a single row returned from a query.  Column values are accessed
by name using the `row.*` accessor functions.  The column name must match
the name used in the SQL `SELECT` clause (or the alias, if one was given).

### DbErr

A sum type representing database operation failures.

| Variant      | Field Type | Meaning                                              |
|--------------|------------|------------------------------------------------------|
| Connection   | Str        | Failed to open or use the database connection        |
| Query        | Str        | The SQL statement is syntactically invalid            |
| NotFound     | Str        | No matching row or column found                      |
| Constraint   | Str        | A database constraint was violated (unique, FK, etc) |

Every function that can fail returns an error union containing `DbErr`.
Pattern-match on the variant to distinguish failure modes:

```toke
let r = db.one("SELECT 1"; []);
match r {
    ok(row)                 => (* use row *)
    err(DbErr.Query; msg)   => log.error(msg);
    err(DbErr.NotFound; _)  => log.warn("no rows");
    _                       => log.error("unexpected db error");
};
```

---

## Connection Lifecycle

### db.open(dsn: Str): DbConn!DbErr

Opens a database connection using the given data source name.  For SQLite
the DSN is a file path, or `":memory:"` for an in-memory database.  Returns
the connection handle on success, or `DbErr.Connection` on failure.

The caller is responsible for closing the connection with `db.close` when it
is no longer needed.

Returns `DbErr.Connection` if the file cannot be opened or the database
engine fails to initialise.

**Example:**
```toke
let conn = db.open(":memory:");
let conn = db.open("/var/data/app.db");
```

### db.close(conn: DbConn): void

Closes the database connection and releases all associated resources.
Any uncommitted transaction is rolled back.  Using the `DbConn` handle
after calling `db.close` is undefined behaviour.

**Example:**
```toke
let conn = db.open(":memory:");
(* ... work with the database ... *)
db.close(conn);
```

---

## Query Functions

### db.exec(sql: Str; params: [Str]): u64!DbErr

Executes a SQL statement that does not return rows -- typically `INSERT`,
`UPDATE`, `DELETE`, or DDL such as `CREATE TABLE`.  Returns the number of
rows affected as a `u64`.  The `params` array supplies positional bind
parameters that replace `?` placeholders in the SQL string, in order.

Returns `DbErr.Query` if the SQL is invalid, or `DbErr.Constraint` if a
unique, foreign-key, or check constraint fails.

**Example:**
```toke
db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY; name TEXT; active BOOL)"; []);
let n = db.exec("INSERT INTO users VALUES (1; 'alice'; true)"; []);  (* n = ok(1) *)
let n = db.exec("INSERT INTO users VALUES (?; ?; ?)"; ["2"; "bob"; "true"]);
```

### db.one(sql: Str; params: [Str]): Row!DbErr

Executes a query and returns the first matching row.  If the query matches
more than one row, only the first is returned and the rest are discarded.
Returns `DbErr.NotFound` when no rows match, or `DbErr.Query` if the SQL
is invalid.

**Example:**
```toke
let row = db.one("SELECT id; name FROM users WHERE id = ?"; ["1"]);
match row {
    ok(r)                  => log.info(row.str(r; "name"));
    err(DbErr.NotFound; _) => log.warn("user not found");
    err(e)                 => log.error("query failed");
};
```

### db.many(sql: Str; params: [Str]): [Row]!DbErr

Executes a query and returns all matching rows as an array.  Returns an
empty array when no rows match -- this is *not* an error.

Returns `DbErr.Query` if the SQL is invalid.

**Example:**
```toke
let rows = db.many("SELECT * FROM users WHERE active = ?"; ["true"]);
match rows {
    ok(rs) => {
        let i = 0;
        while i < arr.len(rs) {
            log.info(row.str(rs[i]; "name"));
            i = i + 1;
        };
    }
    err(e) => log.error("query failed");
};
```

### db.prepare(sql: Str): Stmt!DbErr

Parses and compiles a SQL statement without executing it, returning an
opaque `Stmt` handle.  The prepared statement can be executed repeatedly
with different parameter bindings, avoiding the cost of re-parsing.

Use `db.exec` or `db.one`/`db.many` with the `Stmt` handle in place of a
raw SQL string.  Returns `DbErr.Query` if the SQL is invalid.

**Example:**
```toke
let stmt = db.prepare("INSERT INTO users VALUES (?; ?; ?)");
db.exec(stmt; ["3"; "carol"; "true"]);
db.exec(stmt; ["4"; "dave"; "false"]);
```

---

## Row Accessor Functions

All row accessors take a `Row` and a column name, and return the column
value converted to the requested type.  They return `DbErr.NotFound` if
the column name does not exist in the row.

### row.str(r: Row; col: Str): Str!DbErr

Extracts column `col` from `r` as a string.

**Example:**
```toke
let name = row.str(r; "name");  (* name = ok("alice") *)
```

### row.u64(r: Row; col: Str): u64!DbErr

Extracts column `col` from `r` as an unsigned 64-bit integer.

**Example:**
```toke
let id = row.u64(r; "id");  (* id = ok(1) *)
```

### row.i64(r: Row; col: Str): i64!DbErr

Extracts column `col` from `r` as a signed 64-bit integer.

**Example:**
```toke
let balance = row.i64(r; "balance");  (* balance = ok(-100) *)
```

### row.f64(r: Row; col: Str): f64!DbErr

Extracts column `col` from `r` as a 64-bit float.

**Example:**
```toke
let price = row.f64(r; "price");  (* price = ok(19.99) *)
```

### row.bool(r: Row; col: Str): bool!DbErr

Extracts column `col` from `r` as a boolean.

**Example:**
```toke
let active = row.bool(r; "active");  (* active = ok(true) *)
```

---

## Transaction Functions

Transactions group multiple statements into an atomic unit.  Either all
statements succeed (commit) or none take effect (rollback).  An open
transaction is automatically rolled back if `db.close` is called before
`db.commit`.

### db.begin(conn: DbConn): void!DbErr

Begins a new transaction on the given connection.  Nested transactions are
not supported -- calling `db.begin` while a transaction is already active
returns `DbErr.Query`.

**Example:**
```toke
db.begin(conn);
```

### db.commit(conn: DbConn): void!DbErr

Commits the active transaction, making all changes since `db.begin`
permanent.  Returns `DbErr.Query` if no transaction is active, or
`DbErr.Constraint` if a deferred constraint check fails at commit time.

**Example:**
```toke
db.commit(conn);
```

### db.rollback(conn: DbConn): void!DbErr

Rolls back the active transaction, discarding all changes since `db.begin`.
Returns `DbErr.Query` if no transaction is active.

**Example:**
```toke
db.rollback(conn);
```

---

## Patterns

### Parameterised Query

Always use bind parameters instead of string interpolation to avoid SQL
injection.  Each `?` in the SQL string is replaced by the corresponding
element of the `params` array, in order.

```toke
let user_input = "alice";
let row = db.one("SELECT id; name FROM users WHERE name = ?"; [user_input]);
match row {
    ok(r) => {
        let id = row.u64(r; "id");
        log.info(str.concat("found user id: "; str.from_int(id)));
    }
    err(DbErr.NotFound; _) => log.warn("no such user");
    err(e)                 => log.error("query failed");
};
```

### Transaction with Rollback on Error

A typical pattern: begin a transaction, perform several writes, and roll
back if any step fails.

```toke
let conn = db.open("/var/data/ledger.db");

db.begin(conn);

let debit = db.exec(
    "UPDATE accounts SET balance = balance - ? WHERE id = ?";
    ["100"; "1"]
);

match debit {
    ok(_) => {
        let credit = db.exec(
            "UPDATE accounts SET balance = balance + ? WHERE id = ?";
            ["100"; "2"]
        );
        match credit {
            ok(_)  => db.commit(conn);
            err(_) => db.rollback(conn);
        };
    }
    err(_) => db.rollback(conn);
};

db.close(conn);
```

### Full Lifecycle Example

Create a table, insert rows, query them, and extract column values.

```toke
let conn = db.open(":memory:");

db.exec("CREATE TABLE items (id INTEGER PRIMARY KEY; name TEXT; price REAL; in_stock BOOL)"; []);

db.exec("INSERT INTO items VALUES (1; 'widget'; 9.99; true)"; []);
db.exec("INSERT INTO items VALUES (2; 'gadget'; 24.50; false)"; []);

let rows = db.many("SELECT * FROM items WHERE in_stock = ?"; ["true"]);
match rows {
    ok(rs) => {
        let i = 0;
        while i < arr.len(rs) {
            let name  = row.str(rs[i]; "name");
            let price = row.f64(rs[i]; "price");
            log.info(str.concat(name; str.concat(" $"; str.from_float(price))));
            i = i + 1;
        };
    }
    err(e) => log.error("query failed");
};

db.close(conn);
```
