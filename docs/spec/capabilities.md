# toke Capability Model [N]

**Status:** implemented (Epic 124.4a–d, 2026-07-06); default is `allow-all` until the
deny-by-default flip (124.4g). ADR: [ADR-0010](../decisions/ADR-0010.md).

toke compiles to native code with no language-level sandbox. Without a capability
model, any program that reaches a `std` filesystem / network / environment /
process sink runs with the full ambient OS authority of the invoking user (audit
finding AMB-01). This model makes authority **explicit and declared**: a program
has no fs / net / env-write / process-spawn authority unless it is granted. The
source grammar is untouched — grants are declared out-of-band.

## 1. Capability classes [N]

Exactly five classes gate the ambient surface:

| Class | Grants | Example sinks |
|-------|--------|---------------|
| `fs.read` | read files / dirs / metadata | `file.read`, `file.list`, `file.stat`, `os.open` (read), `db.open` |
| `fs.write` | create / modify / delete files & dirs | `file.write`, `file.mkdir`, `file.delete`, `os.open` (write), `db.open` |
| `net` | listen / connect / send / receive | `http.get`/`serve*`, `net.listen`, `ws.connect`, `tls.*`, `db` (pg/mysql) |
| `env.write` | mutate the process environment | `env.set`, `os.setenv` |
| `process.spawn` | fork/exec other programs | `process.exec`, `process.spawn`, `process.spawndetached` |

Environment **reads**, closing a handle, and reads/writes on an *already-acquired*
handle are ungated: the check fires once, at the **acquisition** point (open file /
listen / connect / spawn / setenv), matching the Deno model.

## 2. Grant channels [N]

Effective grants at process start = **(compiler-baked set) ∪ (runtime `--allow-*` flags)**.

### 2.1 Runtime flags

Passed to the compiled binary itself:

```
./app --allow-read[=PATH] --allow-write[=PATH] --allow-net[=HOST] \
      --allow-env --allow-run[=PATH] --allow-all
```

A specific grant opts the program into enforcement; `--allow-all` is the deliberately
broad, greppable escape hatch. (Scoped values `=PATH`/`=HOST` are accepted today and
coarsened to the whole class; per-path/per-host scoping is a planned refinement.)

### 2.2 Compile-time baking

Grants declared at compile time are baked into the binary so a shipped artifact
carries its own authority (no manifest needs to travel with it):

- CLI: `tkc app.tk --allow-net --allow-read --cap-enforce -o app`
- `tkc.toml` `[capabilities]` table:

```toml
[capabilities]
net = true
fs_read = "/srv/www"     # quoted path/host = grant (coarsened to the class)
fs_write = false
process_spawn = false
env_write = false
enforce = true           # bake deny-by-default into this binary
```

Keys: `fs_read`/`read`, `fs_write`/`write`, `net`, `env_write`/`env`,
`process_spawn`/`run`, `all`, `enforce`. Precedence: CLI `--allow-*` unions with the
`tkc.toml` set.

Mechanism: the compiler emits strong `@__tk_cap_baked_{grants,present,enforce}`
globals into the entry module, overriding the weak `allow-all` defaults in the runtime
broker (`capabilities.c`). `--show-limits` reports the baked set (`cap-present`,
`cap-grants`, `cap-enforce`).

## 3. Enforcement & diagnostics [N]

At startup `tk_cap_init` (called from `tk_runtime_init`) computes the effective grant
set and mode. Each gated sink calls `tk_cap_check`; a denied call raises **CAP001**
and exits non-zero *before* the syscall (fail-closed):

```
CAP001: capability 'fs.write' required but not granted
  grant it at run time with --allow-write, or declare it in tkc.toml [capabilities]
```

With `TK_DIAG_JSON=1` a machine-parseable line is also emitted so the
generate-compile-repair loop can repair a missing grant mechanically:

```json
{"diagnostic_id":"CAP001","severity":"error","stage":"runtime","capability":"fs.write","fix":"grant --allow-write or tkc.toml [capabilities] fs.write=true"}
```

## 4. Modes & the transition [I]

The broker has two modes:

- `ALLOW_ALL` — nothing is gated (today's default; preserves ambient behaviour).
- `ENFORCE` — only granted classes pass; everything else fails closed.

The default is `ALLOW_ALL` until Epic **124.4g** flips it to `ENFORCE`, bundled
atomically with the first-party manifests (ooke, website) and a grant-aware
corpus/eval harness so training/eval Pass@1 is not destroyed by the change.
Enforcement is fully functional today via `--cap-enforce` (baked) or any runtime
`--allow-*` flag.

## References

- [ADR-0010 — Ambient authority and the toke capability model](../decisions/ADR-0010.md)
- [memory-model.md §6.6/§6.7](memory-model.md) — spatial safety, injection resistance
- Implementation: `src/stdlib/capabilities.{h,c}`, `src/config.c`, `src/main.c`, `src/llvm.c`
