---
title: Capabilities
slug: capabilities
section: reference
order: 6
---

# Capabilities

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

### Compile-time — `tkc.toml` manifest (recommended)

Declare what the program needs in a `[capabilities]` table in `tkc.toml`. The grants
are **baked into the binary** at compile time, so the shipped program carries its own
allow-list — no runtime flags needed:

```toml
[capabilities]
fs_read       = true
fs_write      = true
net           = true
env_write     = false
process_spawn = false
enforce       = true    # deny-by-default enforcement (the default)
```

Keys: `fs_read`, `fs_write`, `net`, `env_write`, `process_spawn`, plus `all = true`
(grant everything) and `enforce` (set `false` only to opt a build out of enforcement).

### Run time — `--allow-*` flags

A compiled program accepts allow-flags on its own command line, which add to whatever
the manifest baked in:

```
./myapp --allow-read --allow-net        # grant fs.read + net for this run
./myapp --allow-all                     # grant everything (dev / trusted use)
```

Flags: `--allow-read`, `--allow-write`, `--allow-net`, `--allow-env`, `--allow-run`,
`--allow-all`.

## The `CAP001` diagnostic

When a program calls a gated operation it wasn't granted, it aborts with a
machine-parseable error:

```
CAP001: capability 'fs.read' required but not granted
  grant it at run time with --allow-read, or declare it in tkc.toml [capabilities]
```

The generate-compile-repair loop treats `CAP001` like any other diagnostic — the fix
is to add the missing grant to `tkc.toml` or pass the `--allow-*` flag.

## Relationship to the other controls

Capabilities (ADR-0010) are one layer of toke's correct/secure-by-default program:

- **ADR-0011** — parameterized-only SQL, argv-only process exec, context-aware
  escaping (the injection class).
- **ADR-0012** — runtime spatial-safety traps (bounds / nil-deref).
- **ADR-0013** — crypto agility.

There is **no in-runtime WAF** — that dead module was removed (Story 121.13). Put
rate-limiting and edge filtering at an external reverse proxy.
