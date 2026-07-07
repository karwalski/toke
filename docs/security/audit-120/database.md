## 120.10 — Database layer & injection

> **DAT-01 / DAT-05 RESOLVED 2026-07-07 (Story 121.11b) — the query builder was
> removed.** The string-concatenation SQL builder (`TkQueryBuilder`,
> `tk_db_newquery/settable/setfield/setfieldint/buildinsert/buildupdate/qexecute_w`)
> and the legacy `tk_db_query/insert/delete/execute/connect/lastinsertid_w` wrappers
> were deleted from `db_glue.c`. They were **never in `db.tki`** (unreachable from
> toke), had zero callers, and were unused in the corpus — the injection (DAT-01)
> and 4096-byte-buffer overflow (DAT-05) lived only in that dead code. The **live**
> toke-facing db API — `db.exec` / `db.one` / `db.many`, typed `("str", "[str]")` —
> is already parameterized (SQL + bound `[str]` values), which is ADR-0011's
> guarantee. The interim 121.11 escaping / 121.12 bounded-append fixes are
> superseded by the removal. If a typed query-builder is ever wanted, expose it via
> `db.tki` built on the parameterized `db_exec` path, not string concatenation.

Scope: `src/stdlib/db.c` (SQLite backend + row accessors), `src/stdlib/db_postgres.c`
(libpq backend), `src/stdlib/db_mysql.c` (MySQL/MariaDB backend), `src/stdlib/db_glue.c`
(i64-ABI wrappers exposed to compiled toke programs), plus `src/stdlib/db.h` and the
callable-builtin registry `src/stdlib_decls_gen.h`. Method: manual static review of every
query path, tracing from the toke-callable glue symbols down to the driver call, checking
whether user data reaches the driver as a bound parameter or as concatenated SQL text, plus
review of connection-string/credential handling, TLS posture, and error propagation. No
build/execution was performed. This document feeds the ADR-0011 parameterized-query guarantee.

Key structural fact: the primary path `db.one` / `db.many` / `db.exec` (the only functions
exported in `stdlib/db.tki`) is genuinely parameterized on the SQLite and PostgreSQL
backends. However, `db_glue.c` also registers a **query-builder** and a set of **legacy
raw-SQL wrappers** as first-class callable builtins in `src/stdlib_decls_gen.h`
(`tk_db_buildinsert_w`, `tk_db_qexecute_w`, `tk_db_query_w`, `tk_db_insert_w`,
`tk_db_delete_w`, `tk_db_execute_w`, …). These bypass parameterization entirely and are the
main injection surface. A second structural fact: `db_glue.c` calls the SQLite functions in
`db.c` **directly**; the `DbBackend` vtable and the `db_postgres_backend` / `db_mysql_backend`
structs are never dispatched anywhere in the tree (grep finds no consumer), so the
MySQL/Postgres backends are effectively dead code today. Findings against them are recorded
because they will become live the moment a backend selector is wired in.

---

### DAT-01 — Query builder emits string-concatenated SQL with no escaping (SQL injection)
- Severity: High
- Reachability: remote-auth (values flow from request data through an ooke handler into the builder)
- File: `src/stdlib/db_glue.c:283` (`tk_db_buildinsert_w`), `:308` (`tk_db_buildupdate_w`), `:335` (`tk_db_qexecute_w`); field/value ingestion at `:245` (`tk_db_setfield_w`), `:236` (`tk_db_settable_w`)

Description: `tk_db_buildinsert_w` and `tk_db_buildupdate_w` build SQL by interpolating the
table name, column names, and string values straight into the statement. String values are
wrapped in single quotes with `snprintf(..., "'%s'", qb->values[i])` (lines 300, 320-321,
328-329) and **no single-quote / backslash escaping is performed**. The table and field
names are interpolated raw (`"INSERT INTO %s ("`, line 289; `"%s"` field names, line 292).
The values originate from `tk_db_setfield_w`, which accepts an arbitrary `"field=value"`
string from toke code (line 249-257), and the table from `tk_db_settable_w` (line 239).
`tk_db_qexecute_w` (line 335) feeds the result directly to `db_exec`. All four symbols are
registered as callable builtins in `src/stdlib_decls_gen.h` (lines 85-86, 104, 116, 118), so
a toke/ooke program can reach them with attacker-controlled input.

Impact: A value such as `name=x','admin'); DROP TABLE users;--` closes the quote and injects
arbitrary SQL. On SQLite (`sqlite3_exec` via `db_exec`) this yields full read/write of the
database and, because there is no language sandbox, is limited only by the DB file's OS
permissions. This directly violates the ADR-0011 "all queries parameterized" guarantee.

Recommended fix: Do not build executable SQL by concatenation. Rewrite the builder to emit a
parameterized statement (`INSERT INTO t (a,b) VALUES (?,?)`) and pass the values through the
existing bound-parameter path (`db_exec(sql, params)`), which already uses
`sqlite3_bind_text`. Validate/whitelist identifiers (table and column names) against
`[A-Za-z0-9_]` since identifiers cannot be bound as parameters. Consider deprecating the
builder in favour of the parameterized `db.exec` API.

---

### DAT-02 — Legacy raw-SQL wrappers accept a single pre-built SQL string with no params
- Severity: Medium
- Reachability: remote-auth
- File: `src/stdlib/db_glue.c:115` (`tk_db_query_w`), `:136` (`tk_db_insert_w`), `:146` (`tk_db_delete_w`), `:156` (`tk_db_execute_w`)

Description: These four wrappers each take one `sql` argument and call `db_many`/`db_exec`
with `empty_params()` (lines 119, 140, 150, 160). They provide no mechanism to pass bound
parameters, so any dynamic data must be concatenated into `sql` by the caller before it
reaches the wrapper. They are registered as callable builtins (`src/stdlib_decls_gen.h`
lines 90 area, 89, 95, 92, 105). Because they exist alongside the safe `*params` variants,
they are an attractive footgun: a developer who reaches for `db.query(sql)` with an
interpolated string gets an unparameterized query with no compiler warning.

Impact: Encourages/permits classic SQL injection at the toke source level while appearing to
be a supported DB API. Undermines ADR-0011 by shipping a first-class unparameterized path.

Recommended fix: Remove these legacy wrappers or gate them behind an explicit
`db.execRaw`-style name that documents the danger, and ensure no supported example/framework
code uses them. Prefer routing all callers to `db.exec(sql, params)` /
`db.many(sql, params)`. If retained for migration, add a compiler lint that flags a
string-concatenation expression passed as the `sql` argument.

---

### DAT-03 — MySQL backend silently discards bound parameters
- Severity: High
- Reachability: build-time (backend is compiled under `-DTK_HAVE_MYSQL` but not yet dispatched; becomes remote-auth once wired)
- File: `src/stdlib/db_mysql.c:123` (`my_one`), `:154` (`my_many`), `:185` (`my_exec`)

Description: `my_one`, `my_many` and `my_exec` explicitly drop the parameter array
(`(void)params;`, with the TODO at line 123 "use prepared statements for parameterised
queries") and pass the raw `sql` string straight to `mysql_query()` (lines 125, 156, 187).
The `?` placeholders in the SQL are therefore never substituted with the (safely escaped)
values. This means the parameterized contract that `db.one`/`db.many`/`db.exec` advertise is
a no-op on MySQL: callers relying on ADR-0011 receive zero parameterization. A query written
as `SELECT * FROM u WHERE name = ?` would either error (placeholder left dangling) or, if a
framework compensates by concatenating, execute attacker data as SQL.

Impact: Complete loss of the parameterization guarantee for the MySQL backend. High impact
because it is a silent, guarantee-breaking behaviour rather than a hard failure. Currently
gated by the fact that `db_glue.c` never dispatches to `db_mysql_backend` (the vtable is
unused), so it is not reachable in shipped builds today — hence build-time reachability.

Recommended fix: Implement `my_one`/`my_many`/`my_exec` using
`mysql_stmt_prepare` + `mysql_stmt_bind_param` + `mysql_stmt_execute` (the machinery already
exists in `my_prepare`/`my_bind`), or route them through the prepared-statement path. Until
implemented, these functions should return `DB_ERR_QUERY` when `params.len > 0` rather than
executing an unparameterized statement. Add a regression test that a placeholder query with a
`'`-containing parameter cannot inject.

---

### DAT-04 — MySQL `my_table_exists` concatenates the table name into SQL
- Severity: Medium
- Reachability: build-time (same unused-backend caveat as DAT-03)
- File: `src/stdlib/db_mysql.c:247`

Description: `my_table_exists` formats the caller-supplied `name` directly into the query
with `snprintf(sql, sizeof(sql), "... AND table_name='%s' LIMIT 1", name)` (lines 247-249)
and executes it via `mysql_query`. No escaping is applied. By contrast, the SQLite
(`db.c:182`) and Postgres (`db_postgres.c:214-217`) implementations correctly bind the name
as a parameter. If `name` is attacker-influenced, a value like `x' OR '1'='1` alters the
query; more dangerous payloads are truncated by the 512-byte buffer but injection within that
window is still possible.

Impact: SQL injection through the table-existence check on MySQL once the backend is wired.
Lower severity than DAT-01 because the statement is a read-only existence probe.

Recommended fix: Use a bound parameter (`table_name=?`) via a prepared statement, matching the
SQLite/Postgres implementations.

---

### DAT-05 — Query-builder INSERT/UPDATE can overflow the 4096-byte SQL buffer
- Severity: High
- Reachability: remote-auth
- File: `src/stdlib/db_glue.c:287-303` (`tk_db_buildinsert_w`), `:311-330` (`tk_db_buildupdate_w`)

Description: The builder writes into a fixed `malloc(4096)` buffer using a running `off`
offset and `snprintf(buf + off, 4096 - off, ...)`. `off` is incremented by `snprintf`'s
return value, which is the number of bytes that *would* have been written (C99 semantics),
not the number actually written. The struct allows up to 16 fields (`field_count >= 16`
cap, line 248/271) with field names up to 127 bytes (`fields[16][128]`) and string values up
to 255 bytes (`values[16][256]`), i.e. well over 4096 bytes of potential content. Once the
accumulated `off` reaches or exceeds 4096, the expression `4096 - off` — an `int` promoted to
`size_t` for `snprintf`'s size argument — becomes zero or a huge positive value, and
`buf + off` points past the end of the 4096-byte allocation. Subsequent `snprintf` calls then
write past the heap buffer.

Impact: Heap buffer overflow driven by attacker-controlled field/value content, i.e. memory
corruption in a compiled program that runs with full ambient OS authority. Potential crash or,
depending on heap layout, exploitable write. Reachable through the same registered builder
builtins as DAT-01.

Recommended fix: Track remaining space with a `size_t` and clamp: if
`off >= bufsize` stop appending, or compute the required size first and allocate
dynamically. Never pass a possibly-negative `int` as `snprintf`'s size. A parameterized
rewrite (DAT-01 fix) also eliminates most of the length pressure.

---

### DAT-06 — No TLS enforcement / plaintext-credential fallback for the MySQL backend
- Severity: Medium
- Reachability: build-time (unused backend) / network-adjacent once wired
- File: `src/stdlib/db_mysql.c:65-86` (`my_open`), DSN parse at `:19-63`

Description: `my_open` calls `mysql_real_connect` with no SSL configuration
(`MYSQL_OPT_SSL_MODE` is never set, no CA/cert options). With default client settings the
connection will use TLS if the server offers it but will silently fall back to plaintext
otherwise — there is no `SSL_MODE_REQUIRED` enforcement. Credentials parsed from the
`mysql://user:pass@host/db` DSN (lines 29-42) are then sent over whatever transport the
server negotiates. The Postgres backend (`db_postgres.c:47` `PQconnectdb(dsn)`) delegates TLS
entirely to the DSN `sslmode` field with no default hardening, and SQLite is file-based
(N/A).

Impact: Database credentials and query data may traverse the network in cleartext, enabling
MITM credential theft and data disclosure, without any diagnostic to the operator.

Recommended fix: Set `mysql_options(g_mysql, MYSQL_OPT_SSL_MODE, &SSL_MODE_REQUIRED)` (or
`VERIFY_CA`) by default, allowing opt-out only via an explicit DSN parameter. Document that
Postgres DSNs should specify `sslmode=verify-full`. Consider a warning when a DB connection
negotiates a non-TLS transport.

---

### DAT-07 — Driver error strings propagated verbatim to the application
- Severity: Low
- Reachability: remote-auth (only if the app forwards `DbErr.msg` to responses)
- File: `src/stdlib/db.c:71,80,105,123` (`sqlite3_errmsg`), `src/stdlib/db_postgres.c:88,112,136` (`PQerrorMessage`), `src/stdlib/db_mysql.c:127,158,189` (`mysql_error`)

Description: On error, the backends store the raw driver error message into `DbErr.msg` and
return it to toke code. These messages can contain SQL fragments, column/table names, and
constraint details. This is not itself a vulnerability, but if an ooke handler renders
`DbErr.msg` into an HTTP response (a common pattern), it leaks schema/internal details useful
for injection reconnaissance.

Impact: Information disclosure that aids attackers; low direct severity.

Recommended fix: Document that `DbErr.msg` is for server-side logging only and must not be
returned to clients; provide a sanitized/opaque public error variant. This is primarily an
ooke-layer concern to be cross-referenced in the framework audit.

---

### DAT-08 — `exec_simple` leaks the SQLite `errmsg` allocation and mishandles ownership
- Severity: Low
- Reachability: local
- File: `src/stdlib/db.c:285-295`

Description: `exec_simple` (used by `db_begin`/`db_commit`/`db_rollback`) captures the
`char *errmsg` allocated by `sqlite3_exec` and stores it into `res.err.msg` without ever
calling `sqlite3_free(errmsg)`. The in-code comment (lines 289-293) acknowledges the pointer
is retained and "callers must not free it," which both leaks the allocation on every failed
transaction control statement and stores a pointer with driver-owned lifetime into a struct
the caller treats as borrowed. On the non-error path there is no leak.

Impact: Memory leak proportional to transaction-control failures; a long-running server (the
expected ooke deployment) accumulates leaked error strings. Not directly exploitable but a
reliability/DoS concern under sustained failure conditions.

Recommended fix: Copy the message into an owned/arena buffer and `sqlite3_free(errmsg)`
immediately, mirroring how the SQLite docs require. Align lifetime semantics with the other
`make_err` call sites (which use static string literals or driver strings valid until the
next call).

---

### Dynamic-testing follow-up
The following warrant runtime/dynamic proof beyond this static review:
- DAT-05: build a fuzzer/PoC that drives `tk_db_newquery_w` → repeated `tk_db_setfield_w`
  with 16 maximal-length fields → `tk_db_buildinsert_w`, run under ASAN, and confirm the
  heap overflow when `off` exceeds 4096. Measure the exact field/value lengths that trip it.
- DAT-01: end-to-end injection PoC — call `tk_db_setfield_w("name=x','y'); DROP TABLE t;--")`
  then `tk_db_qexecute_w` against a throwaway SQLite DB and confirm multi-statement execution.
- DAT-03: once/if a MySQL backend selector is wired, run a placeholder query with a `'`-laden
  parameter and confirm whether it errors or injects; verify the prepared-statement path
  (`my_prepare`/`my_bind`) correctly escapes.
- DAT-06: connect the MySQL backend against a server with TLS disabled and packet-capture to
  confirm cleartext credential transmission.
- General: ASAN/valgrind pass over `collect_row`/`pg_collect_row`/`my_collect_row` for the
  unchecked `malloc` on `n == 0` columns and the `my_many` pre-sizing vs. fetch-count paths.

### Positive observations (already-correct defenses)
- The primary API is genuinely parameterized: `db_one`/`db_many`/`db_exec` in `db.c`
  (lines 66-128) use `sqlite3_prepare_v2` + `sqlite3_bind_text` — user data never enters the
  SQL text.
- The SQLite prepared-statement path (`db_prepare`/`db_bind`/`db_step`, lines 202-265) and
  `db_table_exists` (line 178-185) both bind parameters correctly.
- The PostgreSQL backend uses `PQexecParams` / `PQprepare` + `PQexecPrepared` throughout
  (`db_postgres.c:83,107,130,217,249,280`), passing values out-of-band — parameterized by
  construction — and `pg_table_exists` binds the table name (line 216-217).
- The MySQL *prepared-statement* path (`my_prepare`/`my_bind`, `db_mysql.c:268-359`) uses
  `mysql_stmt_bind_param`, which parameterizes correctly (in contrast to the one/many/exec
  path flagged in DAT-03).
- Only `db.one`/`db.many`/`db.exec` and the row accessors are exposed in the public
  `stdlib/db.tki` interface; the dangerous builder/legacy wrappers are absent from that
  contract (though still reachable as raw builtins — see DAT-01/DAT-02).
