---
title: Capabilities
slug: capabilities
section: reference
order: 6
---

toke compiles to native binaries that are **deny-by-default** for operating-system
authority (ADR-0010). A compiled program cannot read or write files, open sockets,
set environment variables, or spawn processes unless it has been **granted** the
corresponding capability. An ungranted call fails at run time with `CAP001`.

This closes the "ambient authority" class: a toke binary only touches the OS
surface you explicitly allow.

## The five capability classes

| Class | Covers | Grant flag |
|-------|--------|------------|
| `fs_read` | reading files/dirs (`file.read`, `os.open` read, `db` reads) | `--allow-read` |
| `fs_write` | writing/creating/removing files (`file.write`, `os.open` create/trunc) | `--allow-write` |
| `net` | sockets — HTTP client/server, TLS, DB connections | `--allow-net` |
| `env_write` | mutating the environment (`env.set`, dotenv load) | `--allow-env` |
| `process_spawn` | spawning subprocesses (`process.spawn`) | `--allow-run` |

## Granting capabilities

A grant is either a **whole class** or a **list of scopes** for that class
(story 138.19):

| Class | A scope is | Example |
|-------|------------|---------|
| `fs_read`, `fs_write` | a path; the grant covers it and everything beneath it | `./content`, `/etc/ssl/certs`, `~/.config/app` |
| `net` | `connect:HOST[:PORT]` or `listen:ADDR:PORT` | `connect:api.example.com:443`, `connect:*.example.com`, `connect:198.51.100.0/24`, `listen:127.0.0.1:8080` |
| `process_spawn` | a command: an absolute path, or a name found on `PATH` at start-up | `/usr/bin/git`, `git` |
| `env_write` | a variable name, or a prefix ending in `*` | `TZ`, `TOKE_*` |

Paths are compared after resolving symlinks and `..`, component by component, so
`./content` does not cover `./content-old`, and a symlink inside `./content` that
points elsewhere is not covered. Hosts are compared with the name the program
passes, before DNS: `connect:127.0.0.1:80` does not cover `localhost`.
`file.listglob` under a scoped grant returns only the matches it found through
directories inside the scope (a match reached through `..` or a symlink that leads
out is left out), and refuses a pattern holding `\` (glob reads it as an escape:
write a literal `*` as `[*]`). A path that passes through a directory outside the
scope on its way back in (`www/../outside/../www/f`) is refused, though it ends
inside. `db.open` and the files SQL opens (`ATTACH`, `VACUUM INTO`) are
checked like any other path, and an SQLite URI filename (`file:...`) is refused
under a scoped grant: name the database by its path.

### Compile-time — `tkc.toml` manifest (recommended)

Declare what the program needs in a `[capabilities]` table in `tkc.toml`. The grants
are **baked into the binary** at compile time, so the shipped program carries its own
allow-list — no runtime flags needed:

```toml
[capabilities]
fs_read       = ["./content", "/etc/ssl/certs"]
fs_write      = ["./cache"]
net           = ["listen:127.0.0.1:8080"]
env_write     = false
process_spawn = false
```

Keys: `fs_read`, `fs_write`, `net`, `env_write`, `process_spawn`, `all = true`
(grant everything), `runtime` (below) and `enforce`. A value is `true` (the whole
class), `false` (none), or an array of scopes on one line. The same grants can be
baked with compile-time flags: `tkc app.tk --allow-read=./content,/etc/ssl/certs`.

**`enforce` does nothing.** Enforcement is always on; `enforce = true`,
`enforce = false` and `--cap-enforce` are accepted for compatibility and ignored. A
program built with `enforce = false` is still denied every ungranted call (`CAP001`).
The only way to switch enforcement off is `all = true` (or `--allow-all`).

**A quoted string is not a scope yet.** `fs_read = "/srv/www"` still grants the
**whole** class in this release — the program can read any file — and the compiler
warns (`W9012`). From toke 3.4.0 it means `fs_read = ["/srv/www"]`. Write `true` for
the whole class, or the array form to scope it now.

### Run time — `--allow-*` flags

A compiled program accepts allow-flags on its own command line:

```
./myapp --allow-read --allow-net               # whole classes
./myapp --allow-read=./content --allow-net=connect:api.example.com:443
./myapp --allow-all                            # grant everything (dev / trusted use)
```

Flags: `--allow-read`, `--allow-write`, `--allow-net`, `--allow-env`, `--allow-run`,
each optionally `=scope,scope,...`, and `--allow-all`. What they do depends on the
binary:

- **Built with no grants** (no `[capabilities]` class, no compile-time `--allow-*`):
  the run-time flags **are** the grants.
- **Built with grants:** run-time flags can only **narrow** them. A flag for a class
  limits that class to where both the baked grant and the flag allow it; it can never
  add a class or a scope, and `--allow-all` is refused (`CAP003`). Put `runtime = "union"` in
  `[capabilities]` to let a launcher widen the grants instead.

## The `CAP001`, `CAP002` and `CAP003` diagnostics

When a program calls a gated operation whose class it wasn't granted, it aborts with
a machine-parseable error:

```
CAP001: capability 'fs.read' required but not granted
  grant it at run time with --allow-read, or declare it in tkc.toml [capabilities]
```

When the class is granted but not for this path, host, command or key, the code is
`CAP002`, and it names the exact scope that would admit the call:

```
CAP002: capability 'fs.read' is granted, but not for '/srv/www/data.json'
  grant this scope at run time with --allow-read=/srv/www/data.json
```

With `TK_DIAG_JSON=1` each also prints one JSON line (`diagnostic_id`, `capability`,
for CAP002 `scope`, and a `fix` where one edit is sure to admit the call). The
generate-compile-repair loop treats them like any other diagnostic: CAP001 means add
a grant, CAP002 means widen one. A run-time flag the program cannot honour (a
malformed scope, or `--allow-all` on a binary built with grants) stops it before
`main` with `CAP003: <flag> refused: <reason>`, and a JSON line with no `fix`. See
[the capability spec](/docs/spec/capabilities/) §3 for when each `fix` is given.

## Relationship to the other controls

Capabilities (ADR-0010) are one layer of toke's correct/secure-by-default program:

- **ADR-0011** — parameterized-only SQL, argv-only process exec, context-aware
  escaping (the injection class).
- **ADR-0012** — runtime spatial-safety traps (bounds / nil-deref).
- **ADR-0013** — crypto agility.

There is **no in-runtime WAF** — that dead module was removed (Story 121.13). Put
rate-limiting and edge filtering at an external reverse proxy.
