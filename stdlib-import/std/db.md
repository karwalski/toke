# std.db -- Database Operations

## Overview

The `std.db` module provides functions for querying a SQL database. It uses a single implicit connection opened with `db.open` and closed with `db.close`. Query results are returned as `Row` values, with typed accessor functions to extract column values.

The current implementation uses SQLite as the backend.

## Types

### Row

Represents a single row returned from a query. Column values are accessed by name using the `row.*` accessor functions.

## Functions

### db.open(dsn: Str): bool

Opens a database connection using the given data source name. For SQLite, this is a file path or `":memory:"` for an in-memory database. Returns `true` on success, `false` on failure.

**Example:**
```toke
db.open(":memory:");
db.open("/tmp/app.db");
```

### db.close(): void

Closes the current database connection and releases all associated resources.

**Example:**
```toke
db.close();
```

### db.exec(sql: Str; params: [Str]): u64!DbErr

Executes a SQL statement that does not return rows (e.g., `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`). Returns the number of rows affected. The `params` array provides positional bind parameters.

**Example:**
```toke
db.exec("CREATE TABLE t(id INTEGER; name TEXT)"; []);
let n = db.exec("INSERT INTO t VALUES(1;'hello')"; []);
(* n = ok(1) *)
```

### db.one(sql: Str; params: [Str]): Row!DbErr

Executes a query and returns the first row. Returns `DbErr.NotFound` if no rows match, or `DbErr.Query` if the SQL is invalid.

**Example:**
```toke
let row = db.one("SELECT id;name FROM t WHERE id=1"; []);
(* row = ok(Row{...}) *)
```

### db.many(sql: Str; params: [Str]): [Row]!DbErr

Executes a query and returns all matching rows as an array. Returns `DbErr.Query` if the SQL is invalid.

**Example:**
```toke
let rows = db.many("SELECT * FROM t"; []);
(* rows = ok([Row; Row; ...]) *)
```

### row.str(r: Row; col: Str): Str!DbErr

Extracts the value of column `col` from `r` as a string. Returns `DbErr.NotFound` if the column does not exist in the row.

**Example:**
```toke
let name = row.str(r; "name");  (* name = ok("hello") *)
```

### row.u64(r: Row; col: Str): u64!DbErr

Extracts the value of column `col` from `r` as an unsigned 64-bit integer. Returns `DbErr.NotFound` if the column does not exist.

**Example:**
```toke
let id = row.u64(r; "id");  (* id = ok(1) *)
```

### row.i64(r: Row; col: Str): i64!DbErr

Extracts the value of column `col` from `r` as a signed 64-bit integer. Returns `DbErr.NotFound` if the column does not exist.

**Example:**
```toke
let val = row.i64(r; "balance");  (* val = ok(-100) *)
```

### row.f64(r: Row; col: Str): f64!DbErr

Extracts the value of column `col` from `r` as a 64-bit float. Returns `DbErr.NotFound` if the column does not exist.

**Example:**
```toke
let price = row.f64(r; "price");  (* price = ok(19.99) *)
```

### row.bool(r: Row; col: Str): bool!DbErr

Extracts the value of column `col` from `r` as a boolean. Returns `DbErr.NotFound` if the column does not exist.

**Example:**
```toke
let active = row.bool(r; "active");  (* active = ok(true) *)
```

## Error Types

### DbErr

A sum type representing database operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| Connection | Str | Failed to connect to the database |
| Query | Str | The SQL statement is invalid |
| NotFound | Str | No matching row or column found |
| Constraint | Str | A database constraint was violated (e.g., unique, foreign key) |
