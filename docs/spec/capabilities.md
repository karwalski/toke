---
title: toke Capability Model
slug: capabilities
section: spec
order: 11
---

**Status:** Normative [N]; implemented (Epic 124.4a–d, 2026-07-06); deny-by-default since the
124.4g flip (§4). ADR: [ADR-0010](/docs/decisions/ADR-0010/).

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
| `fs.read` | read files / dirs / metadata | `file.read`, `file.list`, `file.stat`, `os.open` (read), `db.open`, `toml.loadfile`, `toon.load`, `yaml.load`, `md.renderfile`, `crypto.sha256file` / `crypto.sha256verify`, `i18n.load`, `template.load`, `vecstore.collection` |
| `fs.write` | create / modify / delete files & dirs | `file.write`, `file.mkdir`, `file.delete`, `os.open` (write), `db.open`, `log.openaccess` / `log.openerror` (not for `/dev/stderr` or `/dev/stdout`) |
| `net` | listen / connect / send / receive | `http.get`/`serve*`, `net.listen`, `ws.connect`, `tls.*`, `db` (pg/mysql) |
| `env.write` | mutate the process environment | `env.set`, `os.setenv` |
| `process.spawn` | fork/exec other programs | `process.exec`, `process.spawn`, `process.spawndetached` |

Environment **reads**, closing a handle, and reads/writes on an *already-acquired*
handle are ungated: the check fires once, at the **acquisition** point (open file /
listen / connect / spawn / setenv), matching the Deno model.

### 1.1 Which entries are gated, and how that is kept true [I]

The sink list is not maintained by hand. `scripts/check_capability_sinks.py` walks
every stdlib entry a program can call (each `.tki` function's wrapper, and the
symbols `src/stdlib_tables.c` and `src/llvm.c` name) through the C call graph to
every fs / net / env-write / spawn primitive it can reach, and fails when one is
reached with no `TK_REQUIRE` of its class on the path. Conformance case
`C470_capability_sinks_gated.sh` runs it, and shows it failing when a gate is
removed (story 121.22).

What it cannot yet pass is listed, entry by entry, in
`scripts/check_capability_sinks_skiplist.txt`: sinks that are not the program's
authority (entropy from `/dev/urandom`, the server's own pre-forked workers, a log
file's rotation after it was opened under a grant), and **known gaps**. There are
no known gaps as of 2026-10-01. Story 121.42 closed them: `llm.*` takes `net`
before it connects; `i18n.load`, `template.load` and `vecstore.collection` take
`fs.read`, and `vecstore.flush` / `vecstore.close` take `fs.write`;
`toon.load`, `yaml.load`, `md.renderfile` and `crypto.sha256file` /
`sha256verify` take `fs.read`; `router.serve` (and `chart.serve` /
`dashboard.serve`, which serve through it) takes `net` before it binds;
`http.downloadfile` takes `fs.write` as well as `net`, before it fetches
anything; and `http.servedir` / `http.servepages` take `fs.read` when they
register, since the mount hands the server the authority to read files under it
on every request.
The skiplist is self-failing: a line whose gap is fixed must be deleted in the same
commit.

## 2. Grant channels [N]

Effective grants at process start = **(compiler-baked set) ∪ (runtime `--allow-*` flags)**.

### 2.1 Runtime flags

Passed to the compiled binary itself:

```
./app --allow-read[=PATH] --allow-write[=PATH] --allow-net[=HOST] \
      --allow-env --allow-run[=PATH] --allow-all
```

Every class is denied unless granted (§4); `--allow-all` is the deliberately broad,
greppable escape hatch. A scoped value `=PATH`/`=HOST` is accepted and **ignored**:
`--allow-read=/srv/www` grants `fs.read` for every path (§2.2). Per-path/per-host
scoping is not implemented.

### 2.2 Compile-time baking

Grants declared at compile time are baked into the binary so a shipped artifact
carries its own authority (no manifest needs to travel with it):

- CLI: `tkc app.tk --allow-net --allow-read -o app`
- `tkc.toml` `[capabilities]` table (read from `./tkc.toml`, or `--config=PATH`):

```toml
[capabilities]
net = true
fs_read = true
fs_write = false
process_spawn = false
env_write = false
```

Keys: `fs_read`/`read`, `fs_write`/`write`, `net`, `env_write`/`env`,
`process_spawn`/`run`, `all`, and `enforce` (below). A value is `true`/`1`,
`false`/`0`, or a quoted string. Precedence: CLI `--allow-*` unions with the
`tkc.toml` set, and the runtime flags of §2.1 union with both.

What the broker does with each value (`src/config.c`, `src/stdlib/capabilities.c`;
121.42 (8), measured 2026-09-30):

- **Every grant is a whole class.** There is no per-path or per-host scoping. A
  non-empty quoted string is read as `true`: `fs_read = "/srv/www"` grants
  `fs.read` for **every** path — the program can read `/etc/hosts` — not only
  `/srv/www`. An empty string `""` is read as `false`. Write `true`, not a path,
  so the manifest does not claim a restriction the binary does not have.
- **`enforce` does not switch enforcement.** Enforcement is the default (§4),
  so `enforce = true` and `--cap-enforce` change nothing, and `enforce = false`
  does **not** turn enforcement off: a program compiled with only
  `enforce = false` still stops with CAP001 at its first gated call. The key and
  flag are accepted for compatibility and recorded (`cap-enforce` in
  `--show-limits`); the runtime no longer reads the value.
- **The only way out of enforcement is `all = true`** (or `--allow-all`, at
  compile or run time), which puts the broker in `ALLOW_ALL`. Granting each of
  the five classes one by one passes every gate as well, but the broker stays in
  `ENFORCE`.

Mechanism: the compiler emits strong `@__tk_cap_baked_{grants,present,enforce}`
globals into the entry module, overriding the weak defaults in the runtime broker
(`capabilities.c`: no grants, not present). `__tk_cap_baked_enforce` is emitted but
vestigial. `--show-limits` reports the baked set (`cap-present`, `cap-grants`,
`cap-enforce`).

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

- `ALLOW_ALL` — nothing is gated. Reached only by an explicit `--allow-all`
  (baked or at run time).
- `ENFORCE` — only granted classes pass; everything else fails closed.

The default is `ENFORCE`: Epic **124.4g** flipped it (2026-07-06), together with
the first-party manifests (ooke, website) and grant-aware run harnesses, which
compile with `--allow-all` so that measurement is unaffected.

## References

- [ADR-0010 — Ambient authority and the toke capability model](/docs/decisions/ADR-0010/)
- [memory-model.md §6.6/§6.7](/docs/spec/memory-model/) — spatial safety, injection resistance
- Implementation: `src/stdlib/capabilities.{h,c}`, `src/config.c`, `src/main.c`, `src/llvm.c`
