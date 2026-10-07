---
title: std.db
slug: db
section: reference/stdlib
order: 11
---

**Status: Implemented** -- C runtime backing.

The `std.db` module provides functions for querying a SQL database. Query results are returned as `$dbrow` values, with typed accessor functions to extract column values by column name.

The current implementation uses SQLite as the backend. A connection is opened with `db.open`, which returns a handle.

**Several connections (138.6).** Every `db.open` returns a new handle (1, 2, 3, ... — never reused), and each handle is its own connection. The `*on` forms — `db.execon`, `db.oneon`, `db.manyon`, `db.closeon` — take the handle as their first argument. The handle-less forms — `db.exec`, `db.one`, `db.many`, `db.close` — use the **default connection**: the lowest-numbered one still open, which is the first database the program opened. Opening a second database never repoints them. Before 138.6 there was one global connection: a second `db.open` replaced the first for every later query and returned the same handle.

## Types

### $dbrow

Represents a single row returned from a query. Column values are accessed by name using the `row.*` accessor functions.

| Field | Type | Meaning |
|-------|------|---------|
| cols | @(@($str)) | One `@(name; value)` pair per column, in the query's column order; SQL NULL reads as `""`. The `row.*` accessors read this field, so a `$dbrow{cols:@(...)}` literal works with them too |

### $dberr

A sum type representing database operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $connection | $str | Failed to connect to the database |
| $query | $str | The SQL statement is invalid |
| $notfound | $str | No matching row or column found |
| $constraint | $str | A database constraint was violated (e.g., unique, foreign key) |

**The message is for your logs, not your users (121.21).** A `$dberr`'s text is usually the database driver's own message, and it can quote the SQL, table and column names and constraint names. Log it on the server; send a client a fixed message of your own. Returning it in an HTTP response hands an attacker your schema.

## Functions

### db.open(path: $str): u64!$dberr

Opens (creating if needed) the SQLite database at `path` — `":memory:"` for a private in-memory one — and returns its handle. Needs the `fs.read` and `fs.write` capabilities. Returns `$dberr.$connection`'s arm if the database cannot be opened.

```toke
m=app;
i=db:std.db;

f=twodbs():i64{
  let a=mt db.open("a.db") {$ok:h h;$err:e 0};
  let b=mt db.open("b.db") {$ok:h h;$err:e 0};
  let n=mt db.execon(b;"CREATE TABLE IF NOT EXISTS t(v INTEGER)";@()) {$ok:v 0;$err:e 1};
  db.closeon(b);
  < n
};
```

### db.close(): i64

Closes the default connection (the lowest-numbered one still open). The next one, if any, becomes the default.

### db.closeon(h: u64): bool

Closes the connection `h`. Returns `false` if `h` is not an open handle.

### db.exec(sql: $str; params: @($str)): u64!$dberr

Executes SQL that does not return rows (e.g., `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`) on the default connection. Returns the number of rows the string inserted, updated or deleted. The `params` slice provides positional bind parameters (use `?` placeholders in the SQL).

**Every statement runs, or the call fails (138.6).** A string of several `;`-separated statements runs each in turn. If one fails, the call returns `$dberr.$query` naming the statement (`db.exec: statement 2: ...`); the statements before it have already been applied, so wrap a migration in `BEGIN; ... COMMIT;` to make it all-or-nothing. Bind parameters with more than one statement are refused before anything runs, because which statement a `?` belongs to is ambiguous. Before 138.6 only the first statement ran and the call reported success. Transactions are ordinary statements: `db.exec("BEGIN";@())`, `COMMIT`, `ROLLBACK`.

A count of 0 is `$ok 0` -- every `CREATE`, and an `UPDATE` that matched nothing. (Until the compiler read std.db's error slot, 138.6, a 0 was reported as 1, because under the value-sentinel ABI it would have read as `$err`.)

```toke
m=app;
i=db:std.db;

f=setup():i64{
  let n=db.exec("CREATE TABLE t(id INTEGER; name TEXT)";@());
  <mt n {$ok:v 0;$err:e 1}
};
```

### db.execon(h: u64; sql: $str; params: @($str)): u64!$dberr

`db.exec` on the connection `h`. Returns `$dberr.$connection` if `h` is not an open handle.

### db.one(sql: $str; params: @($str)): $dbrow!$dberr

Executes a query and returns the first matching row. Returns `$dberr.$notfound` if no rows match, or `$dberr.$query` if the SQL is invalid.

```toke
m=app;
i=db:std.db;

f=finduser(id:$str):i64{
  let r=db.one("SELECT id,name FROM t WHERE id=?";@(id));
  <mt r {$ok:v 0;$err:e 1}
};
```

### db.oneon(h: u64; sql: $str; params: @($str)): $dbrow!$dberr

`db.one` on the connection `h`.

### db.many(sql: $str; params: @($str)): @($dbrow)!$dberr

Executes a query and returns all matching rows as a slice. Returns `$dberr.$query` if the SQL is invalid.

```toke
m=app;
i=db:std.db;

f=allrows():i64{
  let rows=db.many("SELECT * FROM t";@());
  <mt rows {$ok:v 0;$err:e 1}
};
```

### db.manyon(h: u64; sql: $str; params: @($str)): @($dbrow)!$dberr

`db.many` on the connection `h`.

### row.str(r: $dbrow; col: $str): $str!$dberr

Extracts the value of column `col` from `r` as a string. Returns `$dberr.$notfound` if the column does not exist in the row.

```toke
m=app;
i=db:std.db;

f=getname():$str{
  let r=db.one("SELECT name FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let name=mt row.str(row;"name") {$ok:s s;$err:e "unknown"};
  < name
};
```

### row.u64(r: $dbrow; col: $str): u64!$dberr

Extracts the value of column `col` from `r` as an unsigned 64-bit integer. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getid():u64{
  let r=db.one("SELECT id FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let id=mt row.u64(row;"id") {$ok:n n;$err:e 0};
  < id
};
```

### row.i64(r: $dbrow; col: $str): i64!$dberr

Extracts the value of column `col` from `r` as a signed 64-bit integer. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getbal():i64{
  let r=db.one("SELECT balance FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let val=mt row.i64(row;"balance") {$ok:n n;$err:e 0};
  < val
};
```

### row.f64(r: $dbrow; col: $str): f64!$dberr

Extracts the value of column `col` from `r` as a 64-bit float. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getprice():f64{
  let r=db.one("SELECT price FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let price=mt row.f64(row;"price") {$ok:f f;$err:e 0.0};
  < price
};
```

### row.bool(r: $dbrow; col: $str): bool!$dberr

Extracts the value of column `col` from `r` as a boolean. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=isactive():bool{
  let r=db.one("SELECT active FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let active=mt row.bool(row;"active") {$ok:b b;$err:e false};
  < active
};
```

## Usage Example

```toke
m=app;
i=db:std.db;
i=log:std.log;

f=main():i64{
  mt db.exec("CREATE TABLE users(id INTEGER; name TEXT; active INTEGER)";@()) {
    $ok:n log.info("table ready";@());
    $err:e log.warn("create failed";@())
  };
  mt db.exec("INSERT INTO users VALUES(?;?;?)";@("1";"alice";"1")) {
    $ok:n log.info("inserted";@());
    $err:e log.warn("insert failed";@())
  };
  let r=db.one("SELECT * FROM users WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let name=mt row.str(row;"name") {$ok:s s;$err:e "unknown"};
  let active=mt row.bool(row;"active") {$ok:b b;$err:e false};
  log.info("found user";@());
  <0
};
```
