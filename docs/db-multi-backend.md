# std.db Multi-Backend Architecture

## Current State (SQLite only)

The current `db.c` implementation uses a single global `sqlite3 *g_db` handle. Functions operate on this global. The `.tki` interface exposes 8 functions (db.one, db.many, db.exec, row.str, row.u64, row.i64, row.f64, row.bool).

The C header (`db.h`) additionally exposes:
- Connection lifecycle: `db_open()`, `db_close()`
- Metadata: `db_last_insert_id()`, `db_affected_rows()`, `db_columns()`, `db_is_null()`, `db_table_exists()`
- Prepared statements: `db_prepare()`, `db_bind()`, `db_step()`, `db_finalize()`
- Transactions: `db_begin()`, `db_commit()`, `db_rollback()`

## Abstraction Layer Design

### Connection String Routing

```c
int db_open(const char *dsn);
```

The DSN prefix selects the backend:

| Prefix | Backend | Library |
|--------|---------|---------|
| `sqlite:` or plain path | SQLite3 | libsqlite3 (bundled) |
| `postgres://` or `postgresql://` | PostgreSQL | libpq |
| `mysql://` | MySQL/MariaDB | libmysqlclient |
| `dynamo://` | DynamoDB | HTTP API (libcurl) |

### Backend vtable

Each backend implements a function table:

```c
typedef struct DbBackend {
    const char *name;
    int  (*open)(const char *dsn);
    void (*close)(void);
    RowResult      (*one)(const char *sql, StrArray params);
    RowArrayResult (*many)(const char *sql, StrArray params);
    U64Result      (*exec)(const char *sql, StrArray params);
    StmtResult     (*prepare)(int conn_id, const char *sql);
    BoolResult     (*bind)(TkStmt *stmt, StrArray params);
    RowResult      (*step)(TkStmt *stmt);
    void           (*finalize)(TkStmt *stmt);
    BoolResult     (*begin)(int conn_id);
    BoolResult     (*commit)(int conn_id);
    BoolResult     (*rollback)(int conn_id);
    U64Result      (*last_insert_id)(int conn_id);
    U64Result      (*affected_rows)(int conn_id);
    int            (*table_exists)(int conn_id, const char *name);
} DbBackend;
```

### Backend Registration

```c
/* db.c top-level */
static const DbBackend *g_backend = NULL;

/* Backends register themselves via compile-time flags */
#ifdef TK_HAVE_SQLITE
extern const DbBackend db_sqlite_backend;
#endif
#ifdef TK_HAVE_LIBPQ
extern const DbBackend db_postgres_backend;
#endif
#ifdef TK_HAVE_MYSQL
extern const DbBackend db_mysql_backend;
#endif
#ifdef TK_HAVE_DYNAMO
extern const DbBackend db_dynamo_backend;
#endif
```

### What's Backend-Specific vs Generic

**Generic (stays in db.c):**
- DSN parsing and backend selection
- Row accessor functions (row_str, row_u64, etc.) — operate on Row struct
- db_columns(), db_is_null() — operate on Row struct
- Result type definitions

**Backend-Specific (per-backend file):**
- Connection management
- SQL execution (prepare, bind, step)
- Transactions
- Metadata queries (last_insert_id, table_exists)
- Parameter binding syntax ($1 vs ? vs :name)

### Parameter Binding

| Backend | Placeholder | Example |
|---------|------------|---------|
| SQLite | `?` | `SELECT * FROM t WHERE id = ?` |
| PostgreSQL | `$1, $2, ...` | `SELECT * FROM t WHERE id = $1` |
| MySQL | `?` | Same as SQLite |
| DynamoDB | N/A | Key-value API |

The abstraction layer rewrites `?` placeholders to `$N` for PostgreSQL.

## File Layout

```
src/stdlib/
  db.h              # shared types and vtable definition
  db.c              # generic dispatch + row accessors
  db_sqlite.c       # SQLite backend (existing code, extracted)
  db_postgres.c     # PostgreSQL backend (libpq)
  db_mysql.c        # MySQL backend (libmysqlclient)
  db_dynamo.c       # DynamoDB backend (HTTP API)
```

## Build Flags

```makefile
# Always available
CFLAGS += -DTK_HAVE_SQLITE
LDFLAGS += -lsqlite3

# Optional — enable with:
# CFLAGS += -DTK_HAVE_LIBPQ
# LDFLAGS += $(shell pkg-config --libs libpq)

# CFLAGS += -DTK_HAVE_MYSQL
# LDFLAGS += $(shell pkg-config --libs mysqlclient)
```

## Environment Variable Override

```
TK_DB_DSN=postgres://user:pass@host/dbname ./myapp
```

If `TK_DB_DSN` is set, `db_open("")` uses it as the DSN.
