# Changelog — toke

All notable changes to the toke language and its reference compiler `tkc`.
The version printed by `tkc --version` is the repository-root `VERSION` file,
mirrored in `src/version.h` (`TKC_VERSION`, which `src/main.c` prints);
`python3 scripts/verify_project_facts.py --check` fails when they disagree.
Those are the only places a downstream should read it from.

This project uses semantic versioning **for the language and the compiler CLI
together**. A major bump means source that compiled under the previous major
may no longer compile.

---

## Unreleased

### Breaking changes (with migration)

| # | Change | Story | Migration |
|---|---|---|---|
| 1 | **`std.tls` clients verify the server by default.** `tls.connect` with `tls.tlsconfig`, a `$tlsconfig{...}` literal without a pin, or no configuration now checks the server's chain against the system trust store (`SSL_CERT_FILE`/`SSL_CERT_DIR` override it) and checks the host name (DNS name, or IP address for an address literal). A self-signed, expired, wrongly named or untrusted certificate now makes `tls.connect` return `0`; before, any certificate was accepted (audit-120 TLS-02). Pinned configurations (`tls.pinconfig`, `tls.mutualconfig`) are unchanged. `tlsconfig` gains a fifth field, `insecure`, which a literal may omit (it is `false`). An IP-address `host` is no longer sent as SNI. | 121.64 (owner decision 2026-10-01 §3.1) | Install the CA or point `SSL_CERT_FILE` at it; to trust one self-signed peer, pin it with `tls.pinconfig("";"";peercert)`; to keep the old unverified connection, say so: `tls.insecureconfig(cert;key)` (new). No caller in toke, toke-ooke or toke-website relied on the old behaviour. |
| 2 | **Text-format log lines are logfmt.** The message is now the field `msg=`, quoted by the same rule as every value (quoted when empty or holding a space, `=`, `"`, `\` or a control byte, with `\"` `\\` `\n` `\r` `\t` `\xHH` escapes): `[INFO] msg="request received" request_id=abc123`. Before, the message was written bare, so a message holding ` user=admin` read as a `user` field. JSON-format logs and the access logs are unchanged. An unknown `log.accessformat` name is now echoed escaped. | 121.41 (3) (owner decision 2026-10-01 §3.2) | A reader of text logs that took everything after `[LEVEL] ` up to the first `key=` as the message should parse the `msg` field instead. |

---

## 3.1.0 — loke's handoff, the typed standard library, and security fixes

**Status: prepared on branch `release/3.1.0` (story 138.17). Not tagged, not
published.** Built from main as of 2026-09-30 (`b88090c3`, then Batches X1, X2,
F491, F491b, Y1, Y2 and Y3 merged in, through main `f605aec9`); more than 760 commits since `v3.0.0` (`0b0bc4e`, 2026-09-23). Every entry below names the tracker
story in `docs/progress.md` and a commit, so a consumer can bisect.

This is a minor version by the letter of semver, but read **Breaking changes**
first: most of the release is the checker learning to reject programs that
3.0.0 compiled and then ran wrongly, and several standard-library calls changed
shape. A program that 3.0.0 compiled *and ran correctly* mostly still compiles;
the migrations below cover the ones that do not.

The loke reply `docs/loke-handoff-2026-09-27-reply.md` has a per-issue verdict
for every item of loke's 2026-09-27 handoff, measured against this build.

### Breaking changes (with migration)

| # | Change | Story, commit | Migration |
|---|---|---|---|
| 1 | **HTTP client calls are separate from route registration.** `http.get/post/put/delete(path; &handler)` register routes and are type-checked as such: `http.get(client;path)` is E4031 ("the call registers a route and sends nothing"). The 1-argument `http.get(url)` and 2-argument `http.post(url;body)` client forms are retired (E4026/E4031). | 138.3 — `0d48bda`, `06a7dca`, `109db36`, merge `880b9b4b` | `http.clientget(c;path)`, `http.clientpost(c;path;bytes;ctype)`, `http.clientput(...)`, `http.clientdelete(c;path)` → `httpresp!httperr`; `http.geturl(url)` → `str!httperr` (errs on transport failure and on non-2xx, `code` = status) |
| 2 | **`http.postheaders` is typed and takes 3 arguments**: `(url:str; body:str; headers:[[str]]) -> httpresp!httperr`. The 4-argument `[abi]` form is gone (E4026). A caller's Content-Type replaces the default; CR/LF in a header is refused; the `net` capability is enforced. | 138.8 — `ce65291` | `http.postheaders(url;body;@(@("x-api-key";k);@("content-type";"application/json")))`, matched as a result |
| 3 | **The https client verifies server certificates** (peer and host name, default trust store; `SSL_CERT_FILE`/`SSL_CERT_DIR` override). A call to a host with an untrusted or mismatched certificate now takes `$err`. | 135.25 — `6b82978` | Install the CA, or point `SSL_CERT_FILE` at it; there is no opt-out |
| 4 | **Standard-library calls are checked against the typed `.tki`** — argument kinds and scalar returns. A wrong argument that used to compile (and print an address, or crash) is E4031 at `--check`: `str.tof32(12345)`, `str.isdigit(str.charat(..))`, `net.listen(8080)`, `json.i64(<str>;..)`, arithmetic on `str.nowunix()`. | 138.3, 138.11, 138.12, 138.13 — merge `880b9b4b` | Pass the declared type (`str.tof32("12345")`, `net.listen("127.0.0.1:8080")`) |
| 5 | **Retyped string and std functions.** `str.frombool/replaceall/replacere/setat/splitfirst/sub` and `json.encode/encstr/quote/stringify` return `str` (they printed an address); `str.padleft/padright` take the pad as an `i64` byte code (`48` for `'0'`); `str.compare` returns -1/0/1; `json.getval(Json;str) -> str`; `std.homedir` links without importing `std.process`. | 138.11 — `266eb17`; 138.12 — `641b129` | Interpolate the results directly; pass pad as a byte code |
| 6 | **Withdrawn modules and aliases**: `std.string`, `std.fs`, and 13 `str` aliases (`fromu16 fromu32 fromu64 fromint64 fromi32 fromf32 bytelen pad strip trimws wordcount splitfirst splitwhitespace`). The `loke://` scheme and `~/.loke/models/` are removed from the stdlib. | 136.45 — merge `07c267aa` | `std.string`→`std.str`, `std.fs`→`std.file`; `from{i32,u32,u16,int64}`→`str.fromint`; `fromf32`→`str.fromfloat`; `bytelen`→`str.len`; `strip`/`trimws`→`str.trim`; `splitwhitespace`→`str.split(s;" ")`; `splitfirst`→`str.split` then `.get(0)` |
| 7 | **`csv`, `file` and `zip` errors are real error boxes**; their `lasterr`/`lasterrkind` accessors are retired. `FileErr` widens from 3 to 10 variants (`$invalid $isdir $notregular $symlink $toolarge $nomem $badarg` appended; the first three keep their tags); `csverr` gains `kind` and a real `line`. | 127.102a/b/c — `85080971`, `6e5b9868`, `47c5ad55` | Match `$err:e` and read `e`; a `FileErr` match that listed three variants needs the new ones or a catch-all arm |
| 8 | **`mt` binds a result's arms by label**, not by position; `<$err(x)` is an error return. A program whose err arm was written first used to take it for a valid input. | 138.2 — `f4840c41`, `1def6f04` | None needed; drop any "ok arm first" convention |
| 9 | **New rejections for silent-wrong programs**: `==`/`!=` on struct and sum values (E4031; toke has no structural equality); array-literal elements of different types (E4031 — `@(arr;elem)` used to nest); a struct literal of an undeclared type (E3011); a comparison standing as a statement (**E4074**, new); a `$variant(..)` constructor with other than one payload (E4026). | 138.9 — `85570be1`, `c439fea2`, `b88f882b`; 138.10 — `4c44c669`, `17b9a7c4` | Match on the variant or compare a field; append with `x=x+@(v)`; declare the type; `x=$t{a:9;b:x.b}` for a field update |
| 10 | **Lexer/parser diagnostics**: a comment still open at end of file is **E1007** (new), naming the nested `(*` that consumed its `*)` (comments nest, spec §8.10); `x.a=9` is E2002 "a struct field cannot be assigned"; a reserved keyword as an import alias is one E2002. | 138.10 — `093e81d`, `549eb2b`, `ba91e32` | Close the comment; rebuild the struct |
| 11 | **Compiler caps are met at `--check`** and name the count, the limit and the flag (E9010: `--max-funcs`, `--max-structs`, `--max-imports`, or `tkc.toml`). A program over the struct cap that 3.0.0 compiled by silently dropping types is now E9010. **The default `max_funcs` and `max_structs` rise from 128 and 64 to 1024**; `max_imports` stays 32. The locals caps report one located E9010 per function instead of one unlocated record per local. | 138.15 — `6586136f`, merge `fd0a0e88`; 127.250 — `8e2bf425`; 127.252 — `eb24097f` | Most programs need no flag now; past 32 imports, pass the flag the diagnostic names |
| 12 | **One regex engine** for the checker and the runtime. `\s\S\d\D\w\W`, `\b\B`, `(?:…)`, a leading `(?i)` and POSIX classes are supported (on macOS `\s` etc. used to be literal letters); unsupported syntax (lazy quantifiers, backreferences, lookaround, unbalanced brackets, …) is **E4075** at `--check` for a literal pattern and traps **RT008** for a computed one; a step budget traps **RT009**. `str.matches` is whole-string. | 137.9, 121.35 — merge `49d17163` | Rewrite `.*?` as a negated class (`<[^>]*>`) |
| 13 | **Wider capability gates**: `log.openaccess`/`openerror` need `fs.write`; `toml.loadfile`, `toon.load`, `yaml.load`, `md.renderfile`, `crypto.sha256file`/`sha256verify` need `fs.read`; `router.serve`/`chart.serve`/`dashboard.serve` need `net`; `http.downloadfile` needs `fs.write`; `http.servedir`/`servepages` need `fs.read`. | 121.22 — `783e0848`; 121.42 — `9a9447ce`, merge `b88090c3` | Grant the capability in `tkc.toml [capabilities]` or with `--allow-*` |
| 14 | **`std.db`**: every `db.open` returns a fresh handle; handle-less calls use the first still-open database (a second open no longer repoints them); `db.exec` runs every statement, names the failing one, and refuses bind parameters with several statements. | 138.6 — `0deb11d` | Use `db.execon/oneon/manyon/closeon(h;…)` for a second connection |
| 15 | **Advice-only `fix` fields removed** from E2001, E2002 (generic), E2003, E2004, E2005, E4025, E4027 (the did-you-mean fix stays), W1010 and E9005. A repair loop reading `fix` finds it absent where it was never deterministically correct. | 127.233, 127.235, 127.240, 127.243, 127.237 | Read `message`/`got`/`expected` |
| 16 | **`std.llm` needs the `net` capability** for its connection, like `http.client`; RSA key generation accepts 2048 bits only, and encrypt/decrypt/sign/verify refuse larger moduli (they computed wrong results); `infer.load(path;opts)`, `infer.loadstreaming(dir;opts)` and `mdns.advertise(rec)` take their published arguments, and so do `infer.generate(h;prompt;maxtokens)`, `mlx.generate(m;prompt;maxtokens)` and `mdns.browse(type;&callback)` (the 2-argument generate forms are E4026) — none of the stub modules reports false success; `http.stream(httpclient)` takes the client its `.tki` publishes (it read a URL string) and raises `httperr`; `router.ctx`/`$ctx`, `process_set_cwd` and the SIGHUP reload stubs are removed (unreachable before). | 121.42 (2) `ff2ef9d8`; 121.51 (1) `691550fb`; 135.31 (1) `c99ad497`; 121.42 (7) `8226aa6f`, 121.51 (3) `19cd98ae`, 135.31 (4) `bfcaabff`; 137.11 — `5a5f581b`; 121.53 — `c69c18a7` | `--allow-net` / `net = true` for programs that call `llm.*`; 2048-bit RSA keys; pass a token limit to `generate`; `http.stream(http.client(url))` |
| 17 | **A one-sided result match whose value is used is E4010** — including as the value of an expression-`if` branch; `mt` on a nil sum value traps RT005 (it read the tag through a null pointer). | 127.249 (2) `50a0bd83`; 127.252 — `45a0fd28`; 121.51 (2) `d7eb72df` | Write both arms |
| 18 | Behaviour changes in other modules: `soap.fault` escapes its detail; TOON escapes `\ \| \n \r` both ways; `jwtverify` rejects at/after `exp` and before `nbf`; `http.param`/`header` take `$err` when missing; the HTTP/2 `h2c` upgrade is retired (answered as HTTP/1.1); `pkg.toml` has a closed schema, a source allowlist and a strict lock; **`tpl` `layout()` takes a layout name, not a path** (a name holding `/`, `\`, `..` or empty is refused), and template block nesting (64) and render depth (128) are bounded; `toon.str/i64/f64/bool/arr` raise a `ToonErr` payload (`$err:e` bound nil before); `file` errors name `$isdir` and `$symlink` for EISDIR/ELOOP on every call. | 121.16 `7f95de9b`; 121.40 `b1f8c0da`; 121.20 `e53bf845`; 135.28 `2e667e9`; 121.44 `b90ca884`; 121.39 `3256a412`; 121.41 `ed228353`; 127.246 `53d46b3c`, `6e45ebb1` | Pass `layout("base")`, not a path |

### Fixed for loke (the 2026-09-27 handoff, Epic 138)

- **B1 bind address** — `http.serveon(host;port)`, `serveworkerson`,
  `servetlson`, `servetlsworkerson`; `http.listen("h:p";…)` binds its host. The
  port-only forms keep binding every interface (owner-reversible decision,
  recorded in `docs/stdlib/http.md`). 138.1 — `e743005`.
- **B2 `mt` by label** (above, #8). 138.2.
- **B3 client calls** (above, #1); a client DELETE is reachable. 138.3.
- **B4 keychain** — `keychain.laststatus/lasterrkind/lasterr` tell "no item"
  from "locked/denied/failed"; `keychain.create/use/destroy` give a throwaway
  keychain file off the search list. 138.4 — `a1e0b97`.
- **B5 f64 across modules** — the importer declares the defining module's return
  type (double/float and narrow integers, zero-extended when unsigned). 138.5 —
  `ffe9b4f`.
- **B6 `std.db`** (above, #14) and zero values: `row.i64`/`row.f64` of 0, and a
  `COUNT` of 0, are `$ok 0`. 138.6 — `0deb11d`, `9d9544c`.
- **B8 typed `postheaders`** (above, #2). 138.8.
- **T-16** `http.mockreq(method;path;headers;body;params) -> $req` and
  `http.resstatus/resbody/resheader` test a handler in-process. 138.16 —
  `e2fcb3d`, `e08c2fd`.
- **`$req`/`$res` layout** — handler records are laid out in `.tki` order, so
  `req.body`, `req.headers`, `req.params` and a `$res{…}` literal read and
  write the right memory; `make check-tki-layout` gates it. 138.18 — `c662484`.
- **JSON** — `json.str` unescapes correctly; `json.at(Json;str) -> Json!JsonErr`
  reads nested objects. 138.12 — `641b129`.
- **Crashes** — `net.listen`, `str.isdigit`, a struct with a field of its own
  type (`@$self`), a `$internal` struct field. 138.13 — `3af398c`, `266eb17`,
  `5c9bd31e`, `4a62e7e`, `cfa07fa3`.
- **No silent truncation** — string constants of any size; the stdlib source
  list handed to clang is 64 KB and E9010 when it does not fit, so a long
  checkout path builds. 138.14 — `2075fa9`.
- **Diagnostics** — W1020 no longer fires on a binding named like a foreign
  keyword (`let def=…`). 138.10 — `574a491`.
- **T-11 stub modules** — `infer`/`mlx`/`mdns` take the arity their `.tki`
  publishes and never report false success: a missing model takes `$err`,
  `generate` on no model is `$err`, `isavailable` is `false`. 137.11 —
  `5a5f581b`.
- **Default caps** — `max_funcs` and `max_structs` default to 1024, so loke's
  `--max-funcs`/`--max-structs` flags can go (`--max-imports` stays: default
  32). 127.250 — `8e2bf425`.
- **Still open for loke**: T-17 `json.parse` (owner decision, 138.9), T-55
  quadratic `str.charat` (127.170, owner decision), T-59 scoped capability
  grants (not yet filed).

### Security fixes

- **https certificate verification** (above, #3). 135.25.
- **`llm.llmclient`'s `.model` read the API key**; every stdlib record now
  matches its C struct slot for slot, gated by `scripts/check_tki_layout.py`.
  135.30 — `9088da12`.
- **`crypto.sha256` of a `str` returned the digest of the empty string** (and
  two calls corrupted the heap); `str` into a `[byte]` parameter is fixed at the
  checker. 124.2f — merge `e1dcc7ca`.
- **RSA key generation never terminated** (`bi_modinv` seeded the wrong
  coefficient). 121.48 — `0a47c948`. Keys over 2048 bits, which computed wrong
  products, are now refused (121.51 — `691550fb`).
- **Map hash flooding** — the map index is keyed SipHash-1-3. 121.32 — `fc7cbfb1`.
- **HTTP** — one RFC 9112 request reader for plain and TLS (smuggling,
  pipelining, deadlines; 121.17 `32efd7b1`); `servedir` symlink escape (121.18
  `b8bd3b2d`); a single abandoned request no longer kills the server (127.144,
  merge `1ed358e1`); HTTP/2 frame limits and deadlines (121.17, 121.44); a
  lingering close after GOAWAY and after an HTTP/1.1 error response, so the
  peer reads the answer instead of a TCP reset (121.52 — `d742f29b`; its
  C391 test race fixed in `de559637`, `d572ab01`); `tls.listen` no longer
  binds beside a live listener with an unconditional `SO_REUSEADDR` (135.34 —
  `8eb44f6d`).
- **Templates** — a self-including partial or deeply nested blocks blew the
  stack; `layout()` read `templates_dir/../secret.tkt` (121.41 — `ed228353`).
  An unreachable access-log writer that did not escape its fields is deleted
  (121.41 — `00380854`).
- **Injection** — Markdown table-cell `javascript:` links (121.16 `d7875c47`);
  TOON ↔ JSON field and value escaping (121.40 `b1f8c0da`); header CR/LF in
  `postheaders` (138.8).
- **Decoders and crypto** — forbidden PNG variants refused (121.33 `013ee0a3`);
  RNG aborts rather than return zero keys, JWT `exp`/`nbf`, constant-time TOTP,
  key wiping (121.20 `0ee70589`); database TLS required by default for
  MySQL/Postgres and bound parameters for MySQL (121.21 `91691a21`).
- **Ambient authority** — a capability-sink gate follows every callable entry to
  its fs/net/spawn primitives (121.22 `783e0848`); the gaps it found are gated
  (above, #13).

### Everything else

- **Checker**: receiver dispatch — a call to a method that does not exist is a
  diagnostic, not a link failure (127.115, 127.96); an unimported alias is E4033
  (127.135); parameters shadow top-level functions (127.136); E4027 suggests a
  near member (127.122); `m.get` and `json.get*` set the error channel and can be
  matched (127.30 `3f167aea`, 127.241) — the checker does not yet force the
  match (127.244).
- **Codegen**: a later `mt` binder no longer overwrites an outer binding
  (127.234); a string `mt` binds the right arm (127.237); stdlib error payloads
  bind (127.238, 127.241); in-place appends and non-retaining callees
  (135.20, 127.171, 127.229); `as str` on a slice is the string (127.229).
- **Batches Y1–Y3**: a user function named like a std member (`get`,
  `read`) is no longer taken as the std call's error payload (127.252
  `0d086b66`); E4075 follows a pattern named by an immutable `let` of a
  literal (121.45 `6c36fea9`); the unreachable POSIX `regcomp` wrappers are
  deleted (121.45 `fa009569`); `capabilities.md` §2.2 says what the broker
  does with `enforce` and scoped values (121.42 `a035e786`); bounded
  `snprintf` in duration formatting (121.53 `ac955587`).
- **Tooling**: `--lint` exits non-zero on an error (127.137); `--fix` says when
  it stops short of its fixpoint (127.158); `--migrate` keeps v0.4 source and
  comments (127.57); `--emit-tkir` keeps every import (127.235).
- **Gates**: `make diff-codegen` fails on a crashing program instead of
  baselining it (127.139); `make lint` analyses the stdlib (135.3a); `make`
  rebuilds when flags change (136.62); the D-series `fix` checks are live
  (127.239).
- **Batches X1/X2 (merged into this release)**: user `str`/scalar error payloads
  bind, and `args.get`/`env.get` bind their payload (127.249 `cfb13887`); a
  `str` from `tpl.render`/`tpl.escape` interpolates as text (127.242
  `0c265416`); match-arm bindings release their slots, so many-arm matches in
  one function build (127.246 (4) `2b001c27`); E3011's did-you-mean offers only
  a writable name (127.246 (1)); `time.withtz`/`utcoffset` read the zone rules
  themselves and never `setenv TZ` — thread-safe, and an unknown zone is UTC
  (121.42 (6) `2688a0c8`); a db error's `e.msg` owns its text and survives the
  next call or close (121.51 (5) `7124e466`); Postgres survives allocation
  failure and `??` escapes a literal `?` for jsonb operators (121.51 (8)
  `97511f30`).
- **Runtime**: `http.servedir` mounts are a longest-prefix table (127.75); the
  per-IP rate limiter that silently returned 429 is removed (127.60); `ws.recv`
  returns a real message (127.145); `Res` bodies are owned (127.148); HTTP/2
  CONTINUATION, request bodies and response headers (merge `b88090c3`);
  `vecstore.flush` with atomic saves (136.15); `os.getcwd` returns an owned
  string (135.23); failed server starts exit 1 (135.22, 135.24).

### Publishing

Nothing here is tagged or published. For the owner: tag `v3.1.0` on this
branch's tip (the release workflow takes the version from the tag), then bump
ooke's `ooke.toml [requires]` floor to the tag and re-run its packager (ooke
branch `release/ooke-rc2` already declares `mintoke = "3.1.0"`).

---

## 3.0.0 — the v0.4 language boundary

**Status: tagged `v3.0.0` (`0b0bc4e`, 2026-09-23).** The rest of this entry,
"Publishing" included, is the record as written when the version was cut.

### Why this is a major version

`VERSION` read `2.8.0` from `f1e30b3` (2026-06-21) until this change. The
entire v0.4 language break shipped under that one unchanged number, so `2.8.0`
names two mutually incompatible compilers: the one released on 2026-06-21, and
every commit since. Released 2.8.0 **cannot build current ooke at all** — it
rejects `store.tk:148` with a hard E2002 on expression-`if`.

The practical consequence was that no downstream could pin toke by semver.
ooke had to invent three keys — `mintoke`, `mintokedate` and `mintokecommit`
(`ooke.toml [requires]`, `toke-ooke/docs/versioning.md`) — to express a floor
that a version number should have carried. loke and every other consumer are
in the same position. 3.0.0 gives them a semver to pin instead of a commit
hash.

### Breaking changes since 2.8.0 (`f1e30b3`, 2026-06-21)

Each row names the commit that introduced the break, so a consumer can still
bisect if it needs to.

| # | Change | Introduced | Effect on 2.8.0-era source |
|---|---|---|---|
| 1 | `=` is assignment, `==` is equality (Epic 116.3 / A3) | `335113d`, 2026-07-01 | v0.3 source using `=` for equality changes meaning; 2.8.0 warns W2021 on every `==` |
| 2 | Expression-oriented `if` — `if`/`el` yields a value (Epic 116.1 / A1) | `3bc1dbb`, 2026-07-01 | 2.8.0 rejects expression-`if` with E2002; new source is not backward compatible |
| 3 | `@(...)` array and map literals; `[]` removed (Epic 116 syntax decision) | Epic 116 | `[]` literals no longer parse |
| 4 | Capability broker, deny-by-default (ADR-0010) | `8c073a6`, 2026-07-06 | `stdlib/capabilities.c` becomes a link dependency; binaries are subject to `CAP001` |
| 5 | **`tkc.toml [capabilities]` manifest parsed and baked (ADR-0010)** | `b66bd8e`, 2026-07-06 | **the hard floor.** Any earlier tkc aborts before parsing any toke: `tkc: tkc.toml:9: expected key = value` |
| 6 | Escape-by-default template surface (ADR-0011) | Epic 124.3 | stdlib HTML-escape semantics changed |
| 7 | `mut.` mutable bindings | Epic 116 | reassignment now requires a `mut` binding |
| 8 | No-underscore naming for config keys and stdlib call names (113.2a) | Epic 113 | Profile-1 excludes `_`; stdlib call names renamed |
| 9 | `std.fmt` added; lint rules v1 (12 rules, 6 of them the 131.9 pattern rules) | Epic 131.9 | new diagnostics on previously silent source |
| 10 | The Epic 127.x compiler fixes | throughout | several changed observable behaviour, not only failure modes |

**Earliest toke that builds current ooke: `b66bd8e` (2026-07-06)** — verified by
building tkc at that commit and running a full ooke build against it
(`toke-ooke/docs/versioning.md`). That commit is the semantic content of the
3.0.0 floor.

### Pinning guidance for downstreams

- Require `>= 3.0.0` for anything written in v0.4 syntax.
- `2.x` means pre-v0.4 and will not compile v0.4 source. There is no forward
  compatibility and none is planned.
- The commit/date fallbacks (`mintokedate`, `mintokecommit`) can be retired
  once 3.0.0 is tagged and published; until then they remain the only precise
  floor.

### Spec

Spec level is **v0.4** (`docs/spec/toke-spec-v0.4.md`). It does not change with
this version cut; 3.0.0 is the first compiler version number that names v0.4.

### Not changed

Historical measurement records that name `toke 2.8.0` as the compiler under
test are left alone on purpose — `patterns/catalogue.json`, `bench/patterns/`
results, `docs/metrics-baseline.md` measurement rows, `docs/spec/patterns-v0.4.md`
"Measured at" lines, `docs/reference/combinator-status.md` and the reviews under
`docs/about/`. They record what was measured and on what; rewriting them would
make them false. They are re-stamped only by a re-measurement.

### Publishing

**Nothing here is published.** Cutting the number in the repository is
reversible; tagging and publishing is not. Still outstanding, for the owner:

1. Rebuild `tkc` so the binary actually prints `toke 3.0.0` (the checked-in
   binary still reports 2.8.0 until then).
2. Tag `v3.0.0` — `.github/workflows/release.yml` derives the artefact version
   from `GITHUB_REF_NAME`, so the tag name *is* the release version.
3. Fill the real SHA256s into the homebrew formulae and correct their declared
   versions (story 132.27; `homebrew-toke/Formula/tkc.rb` still declares
   `0.3.0`, `ooke.rb` still declares `0.1.0`, and both carry
   `PLACEHOLDER_SHA256_*`). Do not publish the tap before both are real.
4. Bump ooke's `ooke.toml [requires] mintoke` to `3.0.0` and retire
   `mintokedate` / `mintokecommit`.

---

## 2.8.0 — 2026-06-21 (`f1e30b3`)

Stdlib interface fidelity (113.B.1 / .2 / .3). The last release before the v0.4
break. Pre-v0.4 syntax: `=` is equality, `[]` literals, statement-only `if`, no
capability manifest.

## 2.7.0 — 2026-04-30 (`30b132a`)

`sc` / `spawn` keywords; W8001 unsafe diagnostic.

## 2.6.0 — 2026-04-30 (`f7fe774`)

FFI ownership, closure runtime, `sc`/`spawn`, collections (`std.queue`,
`std.set`, `std.stack`).

## 2.5.0 — 2026-04-29 (`f7a9d40`)

`std.task`, array higher-order functions, `extern_c`, closure parser and name
resolution.

## 2.4.0 — 2026-04-29 (`56ea6b1`)

DWARF debug metadata; option type `$none`.

## 2.2.0 — 2026-04-29 (`f159394`)

`&name` function-reference syntax, replacing `f=name`.

## 2.1.0 — 2026-04-29 (`26eadd4`)

Function-reference syntax `f=name` in call arguments.
