---
title: std.db
slug: db
section: reference/stdlib
order: 11
---

**Status: Implemented** -- C runtime backing.

The `std.db` module provides functions for querying a SQL database. Query results are returned as `$row` values, with typed accessor functions to extract column values by column name.

The current implementation uses SQLite as the backend. The database connection is configured via environment variable or runtime context; there is no explicit `open`/`close` API.

## Types

### $row

Represents a single row returned from a query. Column values are accessed by name using the `row.*` accessor functions.

| Field | Type | Meaning |
|-------|------|---------|
| cols | @(@($str)) | Column values as a 2D string array |

### $dberr

A sum type representing database operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $connection | $str | Failed to connect to the database |
| $query | $str | The SQL statement is invalid |
| $notfound | $str | No matching row or column found |
| $constraint | $str | A database constraint was violated (e.g., unique, foreign key) |

## Functions

### db.exec(sql: $str; params: @($str)): u64!$dberr

Executes a SQL statement that does not return rows (e.g., `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`). Returns the number of rows affected. The `params` slice provides positional bind parameters (use `?` placeholders in the SQL).

```toke
m=app;
i=db:std.db;

f=setup():i64{
  let n=db.exec("CREATE TABLE t(id INTEGER; name TEXT)";@());
  n|{$ok:v 0;$err:e 1}
};
```

### db.one(sql: $str; params: @($str)): $row!$dberr

Executes a query and returns the first matching row. Returns `$dberr.$notfound` if no rows match, or `$dberr.$query` if the SQL is invalid.

```toke
m=app;
i=db:std.db;

f=finduser(id:$str):i64{
  let r=db.one("SELECT id,name FROM t WHERE id=?";@(id));
  r|{$ok:v 0;$err:e 1}
};
```

### db.many(sql: $str; params: @($str)): @($row)!$dberr

Executes a query and returns all matching rows as a slice. Returns `$dberr.$query` if the SQL is invalid.

```toke
m=app;
i=db:std.db;

f=allrows():i64{
  let rows=db.many("SELECT * FROM t";@());
  rows|{$ok:v 0;$err:e 1}
};
```

### row.str(r: $row; col: $str): $str!$dberr

Extracts the value of column `col` from `r` as a string. Returns `$dberr.$notfound` if the column does not exist in the row.

```toke
m=app;
i=db:std.db;

f=getname():$str{
  let r=db.one("SELECT name FROM t WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let name=row.str(row;"name")|{$ok:s s;$err:e "unknown"};
  < name
};
```

### row.u64(r: $row; col: $str): u64!$dberr

Extracts the value of column `col` from `r` as an unsigned 64-bit integer. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getid():u64{
  let r=db.one("SELECT id FROM t WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let id=row.u64(row;"id")|{$ok:n n;$err:e 0};
  < id
};
```

### row.i64(r: $row; col: $str): i64!$dberr

Extracts the value of column `col` from `r` as a signed 64-bit integer. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getbal():i64{
  let r=db.one("SELECT balance FROM t WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let val=row.i64(row;"balance")|{$ok:n n;$err:e 0};
  < val
};
```

### row.f64(r: $row; col: $str): f64!$dberr

Extracts the value of column `col` from `r` as a 64-bit float. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=getprice():f64{
  let r=db.one("SELECT price FROM t WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let price=row.f64(row;"price")|{$ok:f f;$err:e 0.0};
  < price
};
```

### row.bool(r: $row; col: $str): bool!$dberr

Extracts the value of column `col` from `r` as a boolean. Returns `$dberr.$notfound` if the column does not exist.

```toke
m=app;
i=db:std.db;

f=isactive():bool{
  let r=db.one("SELECT active FROM t WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let active=row.bool(row;"active")|{$ok:b b;$err:e false};
  < active
};
```

## Usage Example

```toke
m=app;
i=db:std.db;
i=log:std.log;

f=main():i64{
  db.exec("CREATE TABLE users(id INTEGER; name TEXT; active INTEGER)";@())|{
    $ok:n log.info("table ready";@());
    $err:e log.warn("create failed";@())
  };
  db.exec("INSERT INTO users VALUES(?;?;?)";@("1";"alice";"1"))|{
    $ok:n log.info("inserted";@());
    $err:e log.warn("insert failed";@())
  };
  let r=db.one("SELECT * FROM users WHERE id=?";@("1"));
  let row=r|{$ok:v v;$err:e $row{cols:@()}};
  let name=row.str(row;"name")|{$ok:s s;$err:e "unknown"};
  let active=row.bool(row;"active")|{$ok:b b;$err:e false};
  log.info("found user";@());
  <0
};
```
