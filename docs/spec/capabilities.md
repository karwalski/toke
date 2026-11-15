---
title: toke Capability Model
slug: capabilities
section: spec
order: 11
---

**Status:** Normative [N]; implemented (Epic 124.4a–d, 2026-07-06); deny-by-default since the
124.4g flip (§4); scoped grants and run-time narrowing since story 138.19 (2026-11-15, owner
answer register 3.4: decisions D1–D4 of
[the 138.19 design note](/docs/decisions/138.19-scoped-capability-grants/)). ADR:
[ADR-0010](/docs/decisions/ADR-0010/).

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
reached with no gate of its class on the path. Since 138.19 it also fails when the
only gate on the path is the **class-only** `TK_REQUIRE` (a `CLASSONLY` finding):
every sink must pass a **scoped** gate (§1.3), which is given the path, host,
command or key the sink is about to use. Conformance case
`C470_capability_sinks_gated.sh` runs it, and shows it failing when a gate is
removed (story 121.22) and when a scoped gate is put back on the class-only form
(story 138.19).

What it cannot yet pass is listed, entry by entry, in
`scripts/check_capability_sinks_skiplist.txt`: sinks that are not the program's
authority (entropy from `/dev/urandom`, the server's own pre-forked workers, a log
file's rotation after it was opened under a grant, the broker's own
canonicalisation of a scope), **known gaps** (`ungated`) and entries still on the
class-only gate (`classonly`). There are no known gaps and no class-only entries
as of 2026-11-15. Story 121.42 closed the last gaps: `llm.*` takes `net`
before it connects; `i18n.load`, `template.load` and `vecstore.collection` take
`fs.read`, and `vecstore.flush` / `vecstore.close` take `fs.write`;
`toon.load`, `yaml.load`, `md.renderfile` and `crypto.sha256file` /
`sha256verify` take `fs.read`; `router.serve` (and `chart.serve` /
`dashboard.serve`, which serve through it) takes `net` before it binds;
`http.downloadfile` takes `fs.write` as well as `net`, before it fetches
anything; and `http.servedir` / `http.servepages` take `fs.read` when they
register, since the mount hands the server the authority to read files under it
on every request. Story 138.19 converted every sink to a scoped gate.
The skiplist is self-failing: a line whose gap is fixed must be deleted in the same
commit.

### 1.2 Grants: whole class or scopes [N]

A grant is either the **whole class** or a **list of scopes** for that class. The
class set does not change; `net` takes its direction inside the scope.

| Class | Scope form | Matches |
|---|---|---|
| `fs.read`, `fs.write` | a path: `/srv/www`, `./content`, `~/.config/loke` | the path itself and everything beneath it, compared **component by component** after canonicalisation (§1.3). `/srv/www` does not match `/srv/wwwx`. |
| `net` | `connect:HOST[:PORT]` or `listen:ADDR:PORT` | `HOST` is a DNS name, `*.example.com` (one or more labels, never the bare apex), an IPv4 literal, a bracketed IPv6 literal (`[::1]`) or a CIDR block. `PORT` is a number or `*`; an omitted connect port means any port. A listen `ADDR` is an IP literal and the port is required, so `listen:0.0.0.0:8080` is visibly different from `listen:127.0.0.1:8080`. |
| `process.spawn` | a command: `/usr/bin/git` or `git` | the executable that would run, after `PATH` lookup (§1.3) |
| `env.write` | a variable name: `TZ`, `LANG`, `TOKE_*` | the key passed to `env.set` / `os.setenv`, with one trailing `*` as a prefix wildcard |

A malformed scope — an empty string, a `net` scope without `connect:` / `listen:`,
a listen scope without an IP literal and a port, a connect port of 0, `~user`, an
env key with `=` or a `*` that is not last — is refused: a manifest parse error
naming its line, a compile-time flag error, or (§2.1) a one-line refusal at start-up.

### 1.3 How a scope is checked [N]

Each sink calls a scoped gate with the value it is about to use
(`TK_REQUIRE_PATH(cap, path)`, `TK_REQUIRE_NET(TK_CAP_NET_CONNECT|LISTEN, host, port)`,
`TK_REQUIRE_CMD(argv0)`, `TK_REQUIRE_ENV(key)`). A whole-class grant answers at once,
with no canonicalisation, so `true`, a bare `--allow-x` and `--allow-all` behave as
before. Scopes are parsed and canonicalised once, at start-up; a check is a linear
scan of a manifest-sized list, once per acquisition.

- **Paths.** The requested path is resolved with `realpath()` on its longest
  existing prefix, and the components that do not exist yet are appended (`.`
  dropped, `..` taken lexically). A file being created inside a granted directory
  therefore passes; a `..` or a symlink that escapes the directory fails. A
  component that exists but does not resolve (a dangling symlink, a loop) fails:
  a symlink cannot pass by pointing at something not yet created. Granted paths
  are canonicalised the same way at start-up; a relative grant resolves against
  the **working directory at process start**, and `~` expands from `HOME` at
  start-up. Directory-taking readers (`file.list`, `http.servedir`) are checked on
  the directory; `file.listglob` on its pattern's directory before the first glob
  component (a `..` after a glob component is checked as `/`); `i18n.load` on the
  directory of the `<base>[.<locale>].<ext>` files it reads; `db.open` on the
  database path (`":memory:"` included: it is checked as a path like any other
  name). The residual race is check-then-open (a component swapped for a symlink
  between the check and the `open`); it is accepted, as Deno accepts it.
- **Hosts.** A connect scope is matched against the **name the program passed**,
  before DNS resolution, case-insensitively and without a final dot. An IP or CIDR
  scope matches only an IP literal: `connect:127.0.0.1:80` does not admit
  `localhost`. A name grant does not cover the addresses the name resolves to.
  Through an HTTP proxy, the origin and the proxy must both be admitted. The HTTP
  client does not follow redirects, so every request is one acquisition. A listen
  scope matches the exact bind address and port: `http.serve` and `tls.listen`
  bind `0.0.0.0`; `router.serve` the address it was given (`localhost` is
  `127.0.0.1`); the `net.listen(port)` shorthand and `net.portavailable` bind
  `127.0.0.1`; `net.accept` is checked on the address its listener is bound to.
- **Commands.** An `argv[0]` containing `/` is canonicalised and compared with the
  granted commands. A bare name is searched on the `PATH` captured at start-up —
  the same snapshot `process.spawn` executes through — and the resolved path is
  compared. A bare-name grant (`git`) is resolved once, at start-up, and pinned;
  changing `PATH` later widens nothing. `process.exec` and `process.spawndetached`,
  which split a command string, are checked on its first token. Only the
  executable is scoped, not its arguments.
- **A value the sink cannot use** (a null path, an unparseable URL or address) is
  denied under a scoped grant: no scope can admit it.
- **A class-only gate under a scoped grant fails closed** (§1.1): it passes only
  when the whole class is granted, and otherwise stops with CAP002.

## 2. Grant channels [N]

Grants come from the compiler-baked set (§2.2) and the run-time `--allow-*` flags
(§2.1). How they combine depends on whether a grant set is baked (138.19 D2):

- **No grant set baked** (no class key or `all` in `tkc.toml [capabilities]`, and no
  compile-time `--allow-*` flag; `enforce` and `--cap-enforce` alone bake none):
  the run-time flags **are** the grant. This is how scripts run during development.
- **A grant set baked:** the run-time flags **narrow** it. A class a run-time flag
  names is admitted only where both the baked grant and the flag admit it; a class
  no flag names keeps its baked grant. A run-time flag can never add a class or a
  scope. `--allow-all` at run time is refused with one line before `main` runs
  (exit 1), unless the baked set is already every class.
- **`runtime = "union"`** in `[capabilities]` restores the union of the baked set
  and the run-time flags (and `--allow-all` then switches the broker to
  `ALLOW_ALL`).

### 2.1 Runtime flags

Passed to the compiled binary itself:

```
./app --allow-read[=PATH,...] --allow-write[=PATH,...] \
      --allow-net[=connect:HOST[:PORT],listen:ADDR:PORT,...] \
      --allow-env[=KEY,PREFIX*,...] --allow-run[=CMD,...] --allow-all
```

A flag with no `=value` is the whole class; with a value, the comma-separated
scopes (§1.2). A flag may be repeated, and its scopes accumulate. Every class is
denied unless granted (§4); `--allow-all` is the deliberately broad, greppable
escape hatch. A malformed scope stops the program at start-up with one line
(`capabilities: --allow-net=api.example.com refused: ...`). A flag name matches
exactly (`--allow-read`, `--allow-read=...`, `--allow-fs-read`); consumed flags are
stripped from the program's own argv.

### 2.2 Compile-time baking

Grants declared at compile time are baked into the binary so a shipped artifact
carries its own authority (no manifest needs to travel with it):

- CLI: `tkc app.tk --allow-net=connect:api.example.com:443 --allow-read=./content -o app`
- `tkc.toml` `[capabilities]` table (read from `./tkc.toml`, or `--config=PATH`):

```toml
[capabilities]
fs_read       = ["./content", "/etc/ssl/certs"]
fs_write      = ["./cache"]
net           = ["connect:api.example.com:443", "listen:127.0.0.1:8080"]
process_spawn = ["/usr/bin/git"]
env_write     = ["TZ"]
# runtime = "union"   # let run-time flags widen the baked set (§2)
```

Keys: `fs_read`/`read`, `fs_write`/`write`, `net`, `env_write`/`env`,
`process_spawn`/`run`, `all`, `runtime` and `enforce` (below). A class value is
`true`/`1` (the whole class), `false`/`0` (not granted), a **single-line array of
quoted strings** (the scopes; `[]` grants nothing; no nesting, no trailing comma; a
double-quoted element takes no escapes), or a quoted string (below). Anything else
is a parse error naming the line. A `# comment` may follow a value. `all` takes
`true`/`false`. Precedence: compile-time `--allow-*` flags and the `tkc.toml` set
are combined into one baked set.

What the broker does with each value (`src/config.c`, `src/stdlib/capabilities.c`):

- **A quoted string is the whole class in this release, and warns.** 138.19 D4:
  a single quoted string (`fs_read = "/srv/www"`) becomes a one-element scope list
  in **toke 3.4.0**, after one release of warnings. Until then it keeps its old
  meaning — the **whole** class: `fs_read = "/srv/www"` grants `fs.read` for every
  path, the program can read `/etc/hosts` — and the compiler reports **W9012** for
  it, naming the release and, when the string is not a valid scope
  (`net = "10.255.255.1:9"`), saying the manifest will not parse then. An empty
  string `""` is read as `false`. Write `true` for the whole class, or the array
  form for a scope.
- **`enforce` does not switch enforcement.** Enforcement is the default (§4),
  so `enforce = true` and `--cap-enforce` change nothing, and `enforce = false`
  does **not** turn enforcement off: a program compiled with only
  `enforce = false` still stops with CAP001 at its first gated call. The key and
  flag are accepted for compatibility and recorded (`cap-enforce` in
  `--show-limits`); the runtime does not read the value, and neither bakes a grant
  set (§2).
- **The only way out of enforcement is `all = true`** (or `--allow-all`, at
  compile time, or at run time on a binary with no baked set or with
  `runtime = "union"`), which puts the broker in `ALLOW_ALL`. Granting each of
  the five classes one by one passes every gate as well, but the broker stays in
  `ENFORCE`.

Mechanism: the compiler emits strong `@__tk_cap_baked_{grants,present,enforce,runtime}`
globals into the entry module, and, when there are scopes,
`@__tk_cap_baked_scopes` (an array of `{class, kind, text}`) and
`@__tk_cap_baked_nscopes`, overriding the weak defaults in the runtime broker
(`capabilities.c`: no grants, not present, no scopes, legacy union). A binary built
before 138.19 has no scope table and no run-time mode, and behaves as it did.
`__tk_cap_baked_enforce` is emitted but vestigial. `--show-limits` reports the baked
set (`cap-present`, `cap-grants`, `cap-enforce`, `cap-runtime`, and one `cap-scope`
line per scope), and the `--companion` record lists each class with its scopes.

## 3. Enforcement & diagnostics [N]

At startup `tk_cap_init` (called from `tk_runtime_init`) computes the grants and
mode. Each gated sink calls its scoped gate; a denied call raises **CAP001** or
**CAP002** and exits non-zero *before* the syscall (fail-closed).

**CAP001** — the class is not granted at all (text and JSON unchanged by 138.19):

```
CAP001: capability 'fs.write' required but not granted
  grant it at run time with --allow-write, or declare it in tkc.toml [capabilities]
```

With `TK_DIAG_JSON=1` a machine-parseable line is also emitted so the
generate-compile-repair loop can repair a missing grant mechanically:

```json
{"diagnostic_id":"CAP001","severity":"error","stage":"runtime","capability":"fs.write","fix":"grant --allow-write or tkc.toml [capabilities] fs.write=true"}
```

On a binary whose baked set run-time flags may only narrow (§2), the run-time half
of that advice is wrong, so the hint names the manifest alone and the `fix` is
`tkc.toml [capabilities] fs_write = true` — left out when a run-time flag scopes
the class, which would still narrow the new grant.

**CAP002** — the class is granted, but not for this scope (138.19 D3; a new code,
so the repair loop can tell "add a grant" from "widen a grant"):

```
CAP002: capability 'fs.read' is granted, but not for '/srv/www/data.json'
  grant this scope at run time with --allow-read=/srv/www/data.json
```

```json
{"diagnostic_id":"CAP002","severity":"error","stage":"runtime","capability":"fs.read","scope":"/srv/www/data.json","fix":"--allow-read=/srv/www/data.json"}
```

`scope` is the canonical path, `connect:HOST:PORT` / `listen:ADDR:PORT`, the
resolved command, or the key. The `fix` names that **exact** scope, never the
class, in the one place that admits the call: a run-time flag when run-time flags
union with the grant (no baked set, or `runtime = "union"`) or when only the
run-time flags exclude it; the manifest (`fs_read = ["/srv/www/data.json"]`) when
only the baked grant excludes it. It is absent when both exclude it, when the
request has no canonical scope (a dangling symlink, a command not on `PATH`), or
when the scope cannot be written in that form (a comma in a run-time flag). Whether
a fix is wise is the reviewer's call; it is always sufficient. A class-only gate
reached under a scoped grant (§1.3) is CAP002 with an empty `scope` and no `fix`.

## 4. Modes & the transition [I]

The broker has two modes:

- `ALLOW_ALL` — nothing is gated. Reached only by an explicit `--allow-all`
  (baked, or at run time where §2 allows it).
- `ENFORCE` — only granted classes and scopes pass; everything else fails closed.

The default is `ENFORCE`: Epic **124.4g** flipped it (2026-07-06), together with
the first-party manifests (ooke, website) and grant-aware run harnesses, which
compile with `--allow-all` so that measurement is unaffected.

## 5. Threat model [I]

**In scope** — what scoped grants defend:

- LLM-generated or third-party toke code that over-reaches with granted authority:
  reading `~/.ssh` under a grant meant for `./content`, writing outside its cache,
  talking to a host it was not meant to, spawning a shell when granted `git`,
  listening on `0.0.0.0` when meant to serve loopback.
- The confused deputy: a server that builds a path from request input
  (`../../etc/passwd`); the check sees the final canonical path.
- Supply chain: an imported toke module that tries authority the application's
  manifest does not name.
- A launcher widening a shipped binary's authority (§2).

**Out of scope** — not defended:

- **Native code.** A user `extern` C function and any C FFI call the program makes
  do not go through the broker. Scoped grants are a **policy layer in the toke
  standard library**, not an OS sandbox; OS-level enforcement (Landlock, seccomp,
  `sandbox-exec`) is story 138.44.
- Memory-safety bugs in the program or runtime that let code skip the gate.
- The check-then-open race (§1.3).
- DNS answers: a granted name may resolve anywhere.
- Process arguments: `git` granted means `git` with any arguments.
- An attacker who controls the binary, its baked constants, or the environment it
  starts in (`PATH`, `HOME`, the working directory).
- Reads of the environment, and reads or writes on a handle acquired under a grant.

## 6. Compatibility (138.19) [I]

| Before 138.19 | Now | Breaks? |
|---|---|---|
| `--allow-read`, `fs_read = true`, `--allow-all`, `all = true` | unchanged: the whole class | no |
| `all = true` in `tkc.toml` | refused as "unknown capability 'all'" before; parses now | fixed |
| `--allow-read=/srv/www` (value ignored, whole class) | scoped to `/srv/www` | **yes**, for a program that relied on the ignored value granting more |
| `--allow-net=api.example.com` (value ignored) | refused: a net scope needs `connect:` / `listen:` | **yes** |
| `fs_read = "/srv/www"` (whole class) | still the whole class, with W9012; scoped to `/srv/www` from toke 3.4.0 | not yet |
| a launcher's `--allow-*` widening a baked set | intersected; `--allow-all` refused | **yes**, by design (§2) |
| a binary built before 138.19 | no scope table; behaves exactly as before | no |
| `enforce = true/false`, `--cap-enforce` | still accepted and vestigial; bake no grant set | no |

## References

- [ADR-0010 — Ambient authority and the toke capability model](/docs/decisions/ADR-0010/)
- [138.19 — Scoped capability grants](/docs/decisions/138.19-scoped-capability-grants/) (design note, accepted)
- [memory-model.md §6.6/§6.7](/docs/spec/memory-model/) — spatial safety, injection resistance
- Implementation: `src/stdlib/capabilities.{h,c}`, `src/config.c`, `src/main.c`, `src/llvm.c`
- Conformance: C470 (scoped gates), C1880–C1885, D615–D616
