# Reply to loke — the 2026-09-27 handoff, answered by toke 3.1.0

**From:** toke
**To:** the loke project
**Date:** 2026-09-30
**Re:** `loke/docs/upstream/handoff-2026-09-27.md` (branch `foundation/reset`) and its
reproducers in `docs/upstream/repro-2026-09-27/`
**Verified against:** toke **3.1.0**, branch `release/3.1.0` (toke main as of 2026-09-30, with
Batches X1, X2, F491, F491b, Y1, Y2 and Y3; the reproducers were re-run on the final build), and ooke
**3.0.0-rc.2**, branch `release/ooke-rc2` at `3cc4998`
**Status:** every blocker (B0–B8) is fixed in 3.1.0. Three issues remain open and are named with
their stories in [Still open](#still-open). **The `v3.1.0` and `v3.0.0-rc.2` tags are not created
yet** — the owner tags after reading this; pin the tag, not the branch.

---

## The position, stated first

**Your handoff was the specification, and it held up.** Every issue came with a reproducer and a
stated EXPECTED, which is why each one below has a verdict measured on the release build rather
than a status copied from our tracker. We re-ran both of your runners unchanged, and where a
runner could no longer build a reproducer (because the call it uses is now a compile-time error)
we wrote the smallest rewrite that exercises the same property and ran that too. The rewrites are
in [Appendix A](#appendix-a--rewritten-reproducers).

**Of 77 issues (T-1…T-59 with T-47 split, O-1…O-3, N-1…N-6, R1…R8): 61 FIXED, 13 FIXED BY A
DESIGN CHANGE — the program you wrote is now a located compile error, and the table says which —
and 3 OPEN.** Separately, **11 of your runner's verdicts are wrong about the build they ran on**;
[loke's runner issues](#lokes-runner-issues) lists each with the reason.

**Three things change what loke must do, not only what it can stop doing:**

1. **The HTTP client moved.** `http.get(client;path)` never sent anything; it registered a route.
   It is now a type error, and the client is `http.clientget/clientpost/clientput/clientdelete`
   and `http.geturl`. The one-argument `http.get(url)` loke switched to as a workaround is
   **retired too** — it no longer compiles. See [1](#1-http-client-calls).
2. **`http.postheaders` takes 3 arguments, typed**, and the https client now **verifies
   certificates** (B0). Both of loke's call shapes get E4026 and must migrate.
3. **Several standard-library names are withdrawn** (`std.string`, `std.fs`, 13 `str` aliases)
   and the `csv`/`file`/`zip` `lasterr` accessors are gone. The rename list was measured over loke
   `b19ed91`. See [9](#9-withdrawn-names--the-rename-list-for-loke).

---

## How to reproduce

```sh
# toke 3.1.0 (tag v3.1.0 once published; until then the branch)
git clone https://github.com/karwalski/toke && cd toke && git checkout v3.1.0 && make

# your runners, unchanged, from docs/upstream/repro-2026-09-27
TOKE_DIR=/path/to/toke ./run.sh
./run2.sh /path/to/toke
```

The evidence column quotes our runs of `run.sh`, `run2.sh` and the rewrites in Appendix A
(plus small probes for N-1…N-5); the raw outputs are kept with the release record and are yours
on request. The https, keychain and record-layout
items were measured with toke's own conformance suites against the same build: **C279** (https
certificate verification, 3/3), **C280** (typed `postheaders`, http and https against a local TLS
server, 8/8), **C311** (keychain error kinds and a throwaway keychain, 29/29), **C277** (bind
host, 9/9) and **C360** (`$req`/`$res` layout, 5/5). No request left the machine; every server was
on `127.0.0.1` and stopped by PID.

ooke: `make test TOKE_ROOT=<toke 3.1.0>` on `release/ooke-rc2` exits 0 — every target, the suite
12/12, 165 assertions over 16 suite summaries, 0 failed — and `scripts/package_release.sh`
packaged rc.2 against that toke.

---

## Verdicts

FIXED = the EXPECTED in the reproducer's header was observed. FIXED-BY-DESIGN-CHANGE = the program
as written is now a compile-time error (named), and the property it was after is available by
another call (named). OPEN = not fixed; the story says why. RUNNER-BUG in the last column means the
runner's printed verdict is wrong for this build — see [loke's runner issues](#lokes-runner-issues).

### Blockers

| # | Verdict | Story | Commit | Evidence (3.1.0) |
|---|---|---|---|---|
| B0 https certificates (T-58) | FIXED | 135.25 | `6b82978` | C279: an untrusted self-signed certificate is `$err` and nothing is handled; the same certificate trusted via `SSL_CERT_FILE` is `$ok 200`; a trusted certificate for another name is `$err` |
| B1 bind address (R1, T-43, O-3) | FIXED | 138.1 | `e743005`; ooke `076db84` | `http.serveon("127.0.0.1";11441)`: lsof `127.0.0.1:11441`; loke's `repro_listen`: `127.0.0.1:11442` |
| B2 `mt` by label (T-18, T-31, T-51) | FIXED | 138.2 | `f4840c41`, `1def6f04` | T-18 exit 77; T-31 `valid,okfirst=ok valid,errfirst=ok invalid,okfirst=err invalid,errfirst=err` |
| B3 client calls (T-57, T-15, R7) | FIXED-BY-DESIGN-CHANGE | 138.3 | `0d48bda`, `06a7dca`, `109db36`, merge `880b9b4b` | `http.get(client;path)` is E4031; `http.clientget` ×300 → 300 `GET /health` at the recorder, 300 on the ok arm |
| B4 keychain (R4, R5) | FIXED | 138.4 | `a1e0b97` | C311 29/29: a locked keychain reports `locked`, never `notfound`, with no dialog |
| B5 f64 across modules (T-20) | FIXED | 138.5 | `ffe9b4f` | T-20 exit 55; `declare fastcc double @lib_f64abs(double)` |
| B6 `std.db` (R8, T-30, T-40) | FIXED | 138.6 | `0deb11d`, `9d9544c` | T-30 `ha=1 hb=2 ta-after-b=ok`; T-40 `x,ix_x,` |
| B7 ooke on current toke (O-1, O-2) | FIXED | 138.7 | ooke `94607ac`, `a1ad3e5`, `5d53dae`; floor `b7b5e95` | rc.2's `src/store.tk` compiles; ooke `make test` on 3.1.0 exits 0 |
| B8 typed https POST with headers (R6, T-48) | FIXED-BY-DESIGN-CHANGE | 138.8 | `ce65291` | the 4-argument form is E4026; `http.postheaders(url;body;@(@("x-api-key";"k1");…))` arrives with its headers; C280 8/8 incl. https |
| B9 scoped capability grants (T-59) | OPEN | — (not yet filed) | — | see [Still open](#still-open) |

### Every issue

| Issue | Verdict | Story | Commit | Evidence (3.1.0) | Runner |
|---|---|---|---|---|---|
| T-1 … T-6, T-19 | FIXED | (before 3.0.0) | — | run.sh OK for each | |
| T-7 | FIXED | (before 3.0.0) | — | E4036 no receiver method | |
| T-8 | FIXED | (before 3.0.0) | — | import kept, `--check` after fix rc 0 | |
| T-9 regex syntax | FIXED-BY-DESIGN-CHANGE | 137.9 | merge `49d17163` | lazy `<.*?>` is **E4075** at `--check` (the message suggests `<[^>]*>`); without that line your repro prints `s: a_b S: _ _ ci: Z` | |
| T-10 `http.post` | FIXED-BY-DESIGN-CHANGE | 138.3, 137.10 | merge `880b9b4b` | 4-arg call E4026 naming it route registration; `http.post(url;body)` E4031 "…to send a request call http.clientpost"; `clientpost` arrives (`POST /post body-p`) | runner cannot build |
| T-11 stub modules | FIXED | 137.11, 121.53 | `5a5f581b` | `.tki`-arity calls: `mlx.isavailable=false mlx.load(/nonexistent) arm=err mlx.generate arm=load-err infer.generate arm=load-err` — no `"[… not available]"` success value; your implementation-arity (2-argument `generate`) call is now E4026 "the implementation takes 3 arguments" | |
| T-12 `.tki` vs implementation | FIXED | 138.11 | `266eb17` | `padright str-arg=[ab***]`, `getregulations len=1` | |
| T-13 `$internal` field | FIXED-BY-DESIGN-CHANGE | 138.13 | `4a62e7e`, `cfa07fa3` | both forms E4033 "'$internal' is a type, not a value" | |
| T-14 `==` on sums | FIXED-BY-DESIGN-CHANGE | 138.9 | `85570be1` | E4031 "'==' does not compare struct or sum values"; match on the variant | RUNNER-BUG |
| T-15 `http.get` error channel | FIXED-BY-DESIGN-CHANGE | 138.3 | `109db36` | 1-arg `http.get(url)` E4026; `clientget` on a closed port `$err` (`TCP connect failed`); `geturl` `$err` | runner cannot build |
| T-16 handler test | FIXED | 138.16 | `e2fcb3d`, `e08c2fd` | rewritten: `status=200 body=[got POST /x body=hello]` | RUNNER-BUG |
| T-17 `json.parse` | OPEN | 138.9 | — | `decode a=[xyz] parse a=[]` | |
| T-18 | FIXED | 138.2 | `1def6f04` | exit 77 | |
| T-20 | FIXED | 138.5 | `ffe9b4f` | exit 55 | |
| T-21 `std.homedir` | FIXED | 138.11 | `266eb17` | every length 21; links without `std.process` | |
| T-22 / N-5 db zero values | FIXED | 138.6 | `9d9544c` | `x: ok 25 \| zero: ok 0 \| nosuch: err`; `COUNT(*)` of an empty table `ok 0` | |
| T-23 | FIXED | (before 3.0.0) | — | E4031 on the reversed `str.join` | |
| T-24 `@(array;elem)` | FIXED-BY-DESIGN-CHANGE | 138.9 | `c439fea2` | E4031 "array literal elements must share one type"; append with `x=x+@(v)` | |
| T-25 `max_funcs` | FIXED | 138.15, 127.250 | merge `fd0a0e88`, `8e2bf425` | 130 signatures now build with no flag (`--check` rc 0, `--emit-llvm` rc 0; default `max_funcs` 1024); over the cap, `--check` E9010 names the count, the limit and `--max-funcs` | |
| T-26 field assignment | FIXED | 138.10 | `549eb2b`, `4c44c669` | `x.a=9` E2002 showing the rebuild; `x.a==9;` **E4074** | |
| T-27 `(*` in a comment | FIXED | 138.10 | `093e81d` | **E1007** "the '(*' at 7:1 is never closed … the '(*' at 7:15 inside it opened a nested comment"; `--emit-llvm` rc 1 | RUNNER-BUG |
| T-28 `isdigit(charat)` | FIXED-BY-DESIGN-CHANGE | 138.13 | `266eb17` | E4031 `str.isdigit` takes a `str`; no crash at run time either | |
| T-29 reserved word as an import alias | FIXED | 138.10 | `ba91e32` | one E2002 "'sc' is a reserved keyword and cannot be an import alias" | |
| T-30 | FIXED | 138.6 | `0deb11d` | `ha=1 hb=2 ta-before-b=ok ta-after-b=ok` | |
| T-31 | FIXED | 138.2 | `f4840c41` | `valid,okfirst=ok valid,errfirst=ok invalid,okfirst=err invalid,errfirst=err` | RUNNER-BUG |
| T-32 nested JSON | FIXED-BY-DESIGN-CHANGE | 138.12 | `641b129`, merge `880b9b4b` | `json.i64(<str>;…)` is E4031 (no segfault); `json.at(j;"usage")` then `json.i64` reads `150` | runner cannot build |
| T-33 | FIXED | (before 3.0.0) | — | `i64(0)=ok:0 … bool(false)=ok:false … i64(missing)=err` | RUNNER-BUG |
| T-34, T-37, T-38, T-39 | FIXED | (before 3.0.0) | — | run2 FIXED | |
| T-35 IR truncation | FIXED | 138.14 | `2075fa9` | 670 constants and the 300k string: toke 0, clang 0 | |
| T-36 str typing | FIXED | 138.11 | `266eb17` | `compare(a,b)=-1 compare(b,a)=1 compare(a,a)=0 padleft=[007] tof32=12345` (from `"12345"`; `str.tof32(12345)` is E4031) | RUNNER-BUG |
| T-40 | FIXED | 138.6 | `0deb11d` | sqlite_master `x,ix_x,` | |
| T-41 | FIXED | 138.11 | `266eb17` | 0 of 14 print an address | |
| T-42 undeclared struct literal | FIXED | 138.9 | `b88f882b` | E3011 "identifier 'ghost' is not declared" | |
| T-43 bind address | FIXED | 138.1 | `e743005` | see B1; the port-only `http.serve` still binds `*` — by decision, see [2](#2-servers-choosing-the-address) | |
| T-44, T-47a `json.str` unescape | FIXED | 138.12 | `641b129` | `len=14 lastbyte=10`; `97 10 98 9 99 13 100 34 101 92 102 65` | |
| T-45 `json.enc` | FIXED | 127.229 | (Batch GS) | `34 97 92 110 98 92 116 …` | |
| T-46 | FIXED | (before 3.0.0) | — | run2 FIXED | |
| T-47b `nowunix` arithmetic | FIXED | 138.11 | merge `880b9b4b` | `a/86400` E4031 "'/' requires numeric operands: the left side is 'str'" | |
| T-48 `postheaders` | FIXED-BY-DESIGN-CHANGE | 138.8 | `ce65291` | both 4-arg shapes E4026; rewritten 3-arg call: `ok status=200`, recorder `POST /v1/messages-loke {"n":1} \| hdr:x-api-key=k1;content-type=application/json` | runner cannot build |
| T-49 `net.listen` | FIXED-BY-DESIGN-CHANGE | 138.13 | `3af398c` | `net.listen(11444)` E4031 (declared `str`); `net.listen("127.0.0.1:11444")` `fd>0=true` | runner cannot build |
| T-50 | FIXED-BY-DESIGN-CHANGE | (before 3.0.0) | — | E4031 "expected an error union 'T!E', got 'i32'" | |
| T-51 | FIXED | 138.2 | `f4840c41` | both arm orders: `dec arm=jsonerr read=dec-err` | |
| T-52 `@$self` field | FIXED | 138.13 | `5c9bd31e` | `--emit-interface --check` exit 0 | |
| T-53 | FIXED | (before 3.0.0) | — | fields unlimited (127.190) | |
| T-54 65th struct type | FIXED | 138.15, 127.250 | merge `fd0a0e88`, `8e2bf425` | default build `one=1` (default `max_structs` 1024); over the cap, E9010 names the count, the limit and `--max-structs` | |
| T-55 `str.charat` quadratic | OPEN | 127.170 | — | `ratio_x10=40` (doubling the input costs ×4) | |
| T-56 32-import cap | FIXED | 138.15 | `6586136f` | E9010 "this module has 36 imports and the limit is 32 (max_imports)", fix "pass --max-imports=36, or set max_imports = 36 in tkc.toml", at `--check` | RUNNER-BUG |
| T-57 route registration | FIXED-BY-DESIGN-CHANGE | 138.3 | merge `880b9b4b` | `http.get(hc;"/health")` E4031 "…registers a route and sends nothing; to send a request call http.clientget"; `clientget` 300/300 arrive, 300 ok | runner cannot build |
| T-58 (B0) | FIXED | 135.25 | `6b82978` | C279 3/3 | |
| T-59 (B9) | OPEN | not yet filed | — | not addressed in 3.1.0 | |
| O-1 rc.1 `store.tk` | FIXED (in rc.2) | 138.7 | ooke `94607ac` | rc.2's `store.tk` compiles on 3.1.0 (ooke `make test` exits 0); rc.1's copy still fails E4071 — rc.1 is not fixed, it is replaced | RUNNER-BUG |
| O-2 handler registry cap | FIXED | 138.7 | ooke `5d53dae` | a 40-page app: `Makefile.serve` compiles the registry with `--max-imports=41`; a 3-page app gets no flag | |
| O-3 `[server] host` | FIXED | 138.7 | ooke `076db84`, `be126df` | default `127.0.0.1`; `test-serve-host` passes on 3.1.0 | |
| N-1 long checkout path | FIXED | 138.14 | `2075fa9` | toke built at a 187-character path: a `std.http` program builds; `--emit-deps` 70 paths, 14,681 bytes | |
| N-2 `--out b a.tk b.tk` | FIXED | (before 3.0.0) | — | two-module build rc 0, exits 2 | |
| N-3 `<$ok()` | FIXED-BY-DESIGN-CHANGE | 138.10 | `17b9a7c4` | E4026 "'$ok(..)' is a variant constructor … takes exactly one payload, got 0" (was E9003 `@ok`) | |
| N-4 W1020 on `def` | FIXED | 138.10 | `574a491` | `let def=5`: no W1020 | |
| N-6 client DELETE | FIXED | 138.3 | `0d48bda` | `http.clientdelete(c;"/del")` → recorder `DELETE /del`, `$ok 200` | |
| R1 bind address | FIXED | 138.1 | `e743005` | see B1 | |
| R2 >32 imports | FIXED | 138.15 | `6586136f` | still a flag (default 32); now caught at `--check` naming it | |
| R3 >64 structs / fields | FIXED | 138.15, 127.250 | merge `fd0a0e88`, `8e2bf425` | default `max_structs` is 1024 (run2: `default max-structs=1024`); fields unlimited | |
| R4 keychain error kinds | FIXED | 138.4 | `a1e0b97` | C311 | |
| R5 throwaway keychain | FIXED | 138.4 | `a1e0b97` | `keychain.create/use/destroy`; C311 | RUNNER-BUG |
| R6 https POST with headers | FIXED | 138.8, 135.25 | `ce65291`, `6b82978` | C280 cases 7–8 (https trusted arrives; untrusted `$err`, nothing arrives) | |
| R7 client DELETE | FIXED | 138.3 | `0d48bda` | see N-6 | RUNNER-BUG |
| R8 several connections, transactions | FIXED | 138.6 | `0deb11d` | two handles 1 and 2; `execon` on each writes its own file (`A=[ta ta2] B=[tb]`); BEGIN/COMMIT/ROLLBACK kept | RUNNER-BUG |

---

## What changes in the APIs loke calls

Each item gives the call as loke writes it today and the call 3.1.0 accepts. Where the old form
now fails, the diagnostic is at `--check` with a location, so the compiler will walk you through
the migration.

### 1. HTTP client calls

```toke
(* before: registered a GET route; nothing was sent *)
let up=mt http.get(http.client("http://127.0.0.1:11441");"/health") { $ok:r true; $err:e false };
(* before: the one-argument workaround — returned "" on failure, no error channel *)
let s:str=http.get("http://127.0.0.1:11441/health");
(* before: compiled and sent nothing *)
http.post("http://127.0.0.1:11441/p";"body");
```

```toke
(* 3.1.0 *)
let c=http.client("http://127.0.0.1:11441");
let up=mt http.clientget(c;"/health") { $ok:r r.status==200; $err:e false };
let s=mt http.geturl("http://127.0.0.1:11441/health") { $ok:b b; $err:e "" };
let p=mt http.clientpost(c;"/p";str.bytes("body");"text/plain") { $ok:r r.status; $err:e 0 };
let d=mt http.clientdelete(c;"/item/7") { $ok:r r.status; $err:e 0 };
```

- `http.clientget(httpclient; path)`, `http.clientpost(httpclient; path; [byte]; contenttype)`,
  `http.clientput(…same…)`, `http.clientdelete(httpclient; path)` → `httpresp!httperr`.
  `httpresp` is `{status:u64; headers:[[str]]; body:[byte]}` — the headers are handed to you now
  (they used to be leaked); read the body as text with `str.frombytes(r.body)`
  (`str!EncodingErr`). `httperr` is `{msg:str; code:u64}`; `$err` means transport failure.
- `http.geturl(url) -> str!httperr` — also `$err` on a non-2xx response, with `code` = the status.
- **Retired:** the 1-argument `http.get(url)` (E4026) and the 2-argument `http.post(url;body)`
  (E4031). `http.get/post/put/delete(path; &handler)` are route registration only, and a client
  value in the path position is E4031 naming the client call to use.

### 2. Servers: choosing the address

```toke
http.serve(11441);                     (* before and after: every interface *)
http.serveon("127.0.0.1";11441);       (* 3.1.0: loopback only *)
```

- New: `http.serveon(host; port)`, `http.serveworkerson(host; port; workers)`,
  `http.servetlson(host; port; cert; key)`, `http.servetlsworkerson(host; port; cert; key; workers)`.
  `host` is an IPv4 literal, `localhost`, or `""`/`*`/`0.0.0.0` for every interface; anything
  else fails at start with stage `host`. `http.listen("127.0.0.1:11442";…)` now binds its host.
- **The port-only forms keep binding every interface.** Moving them to loopback would have taken
  every deployed server off the network with no diagnostic; the decision is recorded in
  `docs/stdlib/http.md` and is the owner's to reverse. loke, being local-first, should call the
  `…on` forms with `"127.0.0.1"`.
- A bind that would shadow a live listener on the same port fails at start instead of silently
  taking its traffic (macOS; 135.32, 135.33).

### 3. `http.postheaders` and https

```toke
(* before — both of loke's shapes; now E4026 (the implementation takes 3) *)
http.postheaders(hc;"";body;headersjson);
http.postheaders(url;body;headersjson;"");
```

```toke
(* 3.1.0 *)
let r=mt http.postheaders("https://api.example/v1/messages";body;
  @(@("x-api-key";key);@("content-type";"application/json"))) {
  $ok:v v.status; $err:e 0 };
```

- `http.postheaders(url:str; body:str; headers:[[str]]) -> httpresp!httperr`. Your Content-Type
  replaces the default (it was sent twice); `Host` and `Content-Length` are reserved; a CR or LF
  in a header name or value is refused (`$err`, nothing sent); the `net` capability is enforced.
- **https certificates are verified** — peer and host name, against the default trust store;
  `SSL_CERT_FILE`/`SSL_CERT_DIR` override it. A bad certificate is `$err`, distinguishable from a
  4xx/5xx (which is `$ok` with its status). **This is B0; loke can lift its "refuse every
  non-loopback send" guard.** It applies to every https client call, not only `postheaders`.

### 4. Testing handlers: `http.mockreq` and the response readers

```toke
f=h(req:$req):$res{ <http.res.ok("got \(req.method) \(req.path) body=\(req.body)") };
let r=h(http.mockreq("POST";"/x";@(@("content-type";"text/plain"));"hello";@()));
io.println("\(http.resstatus(r)) \(http.resbody(r)) \(http.resheader(r;"content-type"))");
```

- `http.mockreq(method; path; headers:[[str]]; body; params:[[str]]) -> $req` builds the same
  request record the server passes, so `http.reqbody/reqparam/reqheader/param/header` behave as
  under the server. `http.resstatus(res) -> u64`, `http.resbody(res) -> str`,
  `http.resheader(res; name) -> str`.
- **`$req`/`$res` fields are read from the right memory now.** On 3.0.0, `req.body` read the
  headers pointer and `req.headers.len` segfaulted, and a `$res{…}` literal went out with a
  garbage body (138.18). `req.body` is the recommended read again; `http.reqbody(req)` still
  works. `http.param`/`http.header` take `$err` when the name is missing.

### 5. `std.db`: handles and multi-statement exec

```toke
(* before: the second open returned handle 1 again and repointed every later call *)
let a=mt db.open("a.db") { $ok:h h; $err:e 0 };
let b=mt db.open("b.db") { $ok:h h; $err:e 0 };
```

```toke
(* 3.1.0: a=1, b=2 *)
let x=mt db.execon(a;"CREATE TABLE t(v INTEGER); CREATE INDEX ix ON t(v);";@()) { $ok:n n; $err:e 0 };
let r=mt db.oneon(b;"SELECT COUNT(*) AS n FROM t";@()) { $ok:row row; $err:e … };
db.closeon(a);
```

- Every `db.open` returns a fresh, never-reused handle. `db.execon/oneon/manyon(h; sql; params)`
  and `db.closeon(h)` address one connection. The handle-less `db.exec/one/many` use the first
  database still open — a second `db.open` no longer repoints them.
- `db.exec` runs **every** statement, names the failing statement's number, and refuses bind
  parameters with more than one statement. `BEGIN`/`COMMIT`/`ROLLBACK` through `exec` still work.
- `row.i64`/`row.f64` of a stored 0 and a `COUNT` of 0 are `$ok 0` (they were `$err`).

### 6. Keychain

- `keychain.get/exists/delete` keep their shapes. After a `false` or none, read
  `keychain.lasterrkind()`: `notfound` is the only kind that means "absent"; `locked`, `denied`,
  `nokeychain`, `badarg`, `unavailable` and `failed` mean the query failed and say nothing about
  whether an item exists. `keychain.laststatus()` is the `OSStatus`; `keychain.lasterr()` a
  description that never contains a secret. A locked keychain reports `locked` with no dialog.
- `keychain.create(path; password)`, `keychain.use(path)` (`""` returns to the default keychain),
  `keychain.destroy(path)` give a throwaway keychain file that is not on the search list;
  `destroy` refuses the default keychain and anything on the search list. **This retires loke's
  tests writing to the login keychain (X8.7).**

### 7. JSON, maps and strings

- `json.at(Json; path) -> Json!JsonErr` reads a nested object (`json.at(j;"usage")`, then
  `json.i64(u;"input_tokens")`). `json.getval(Json; str) -> str`. `json.str` unescapes
  correctly (`\n` is byte 10; a backslash is kept).
- `json.get*` (`getint`, `geti64`, `getbool`, `getfloat`, …) are declared `T!JsonErr` and set the
  error channel: `mt json.getint(j;"a") { $ok:v …; $err:e … }` tells a stored 0 from a missing key
  (measured: `a=ok 0 b=err`). **The checker does not yet force the match** — a plain
  `json.getint(j;"a")+1` still compiles and reads the value (127.244).
- `m.get(k)` sets the error channel: `mt m.get("z") { $ok:v …; $err:e … }` takes `$err` on a miss
  (measured: `a=ok 0 b=miss`). Same 127.244 caveat.
- Retyped (138.11): `str.frombool`, `replaceall`, `replacere`, `setat`, `splitfirst`, `sub`,
  `json.encode`, `encstr`, `quote`, `stringify` return `str` (they printed an address);
  `str.compare` returns -1/0/1; `str.isdigit(str) -> bool`; `str.tof32(str) -> f64!ParseErr`;
  `str.nowunix() -> str` (arithmetic on it is E4031 — convert with `str.toint`);
  **`str.padleft/padright(s; width; pad:i64)` take the pad as a byte code**: `str.padleft("7";3;48)`
  is `"007"`. A string pad pads with its first byte, but pass the code.
- `std.homedir()` returns a `str` and links with only `i=std:std;`.
- **`llm.*` needs the `net` capability** (`--allow-net`, or `net = true` in `tkc.toml
  [capabilities]`), like `http.client`: its connection used to bypass the grant (121.42).
- **RSA**: key generation accepts 2048 bits only, and encrypt/decrypt/sign/verify refuse larger
  moduli — they computed wrong results (121.51). 2048-bit key generation works (121.48).
- `time.withtz`/`utcoffset` no longer `setenv TZ` (thread-safe); an unknown zone name is UTC.
- A `std.db` error's `e.msg` is owned and stays valid after the next call or `db.close`.
- `infer.load(path; opts)`, `infer.loadstreaming(dir; opts)`, `mdns.advertise(rec)`,
  `infer.generate(h; prompt; maxtokens)`, `mlx.generate(m; prompt; maxtokens)` and
  `mdns.browse(type; &callback)` take the arguments their `.tki` publishes; the 2-argument
  `generate` is E4026. **None of them reports false success any more (T-11):** `infer` has no
  backend compiled in and `mlx` is a bridge client, so a missing model takes `$err`, `generate`
  with no model takes `$err`, and `isavailable()` is `false` — never the old
  `"[infer: not available]"` text as an `$ok` value.
- `http.stream(c)` takes the `httpclient` that `http.client(url)` returns (it read its argument
  as a URL string, so the published call did not compile) and raises `httperr` on failure;
  `http.streamnext` raises too. It GETs the client's base URL and does not send extra headers.
- `tpl` layouts: `layout("base")` takes a layout **name**, not a path — a name holding `/`, `\`,
  `..` or empty is refused (it read `templates_dir/../…`). Block nesting (64) and render depth
  (128) are bounded, so a self-including partial is `$tmplerr` instead of a stack overflow.
- `toon.str/i64/f64/bool/arr` raise a `ToonErr` you can bind (`$err:e` was nil); `file` errors
  are `$isdir`/`$symlink` for a directory or a symlink on every call, as the byte calls were.
- `net.listen(addr:str)`: `net.listen(8080)` is E4031 — write `net.listen("127.0.0.1:8080")`.
  (Handed an integer at run time, it binds loopback only, never every interface.)

### 8. Errors from `csv`, `file` and `zip`

The `lasterr`/`lasterrkind` accessors of `csv`, `file` and `zip` are **retired** (E4027). The
error arrives in the box:

```toke
(* before *)
let t=file.read(p); if(str.len(file.lasterr())>0){ … };
(* 3.1.0 *)
let t=mt file.read(p) {
  $ok:s s;
  $err:e mt e {
    $notfound:m "";$permission:m "";$io:m "";$invalid:m "";$isdir:m "";
    $notregular:m "";$symlink:m "";$toolarge:m "";$nomem:m "";$badarg:m ""
  }
};
```

`FileErr` widens from 3 to 10 variants — `$invalid $isdir $notregular $symlink $toolarge $nomem
$badarg` appended, the first three keeping their tags — so a match that listed three needs the new
ones or a catch-all. `csverr` gains `kind` (`"eof"` at end of input) and a real `line`.

### 9. Withdrawn names — the rename list for loke

Measured over loke `b19ed91` (928 files):

| Withdrawn | Use | loke files |
|---|---|---|
| `std.string` | `std.str` | 43 |
| `std.fs` | `std.file` | 6 |
| `str.fromi32`, `fromu32`, `fromu16`, `fromint64` | `str.fromint` | |
| `str.fromf32` | `str.fromfloat` | |
| `str.bytelen` | `str.len` | |
| `str.strip`, `str.trimws` | `str.trim` | |
| `str.splitwhitespace` | `str.split(s;" ")` | |
| `str.splitfirst` | `str.split` then `.get(0)` | |
| `str.wordcount` | none | |
| `string.mockhttpreq*` | `http.mockreq` | |

`str.fromu64`, `str.pad` are withdrawn too. Kept: `frombool`, `replaceall`, `compare`, `isdigit`,
`tof32`, `tof64`, `parsefloat`, `lastindexof`, and the `toi32/toi64/toint64/parseint` aliases of
`str.toint`. The `loke://` URL scheme and the `~/.loke/models/` convention are removed from the
standard library (136.45): another project's identity does not belong in it.

### 10. Regex

One non-backtracking engine for the checker and the runtime (137.9). Supported: `\s \S \d \D \w
\W` (also inside brackets), `\b \B`, `(?:…)`, a leading `(?i)`, POSIX `[:classes:]`, bounds up to
255. **On macOS `\s` and friends used to be literal letters** — patterns that "worked" by matching
nothing now match. Unsupported syntax — lazy quantifiers, backreferences, lookaround, unknown
escapes, unbalanced brackets, a bare `{`, empty alternatives — is **E4075** at `--check` for a
literal pattern (with a byte offset) and traps **RT008** for a computed one; a runaway search traps
**RT009**. Matching is POSIX leftmost-longest. `str.matches` is whole-string (unchanged in
effect, now applied by the matcher). **Caveat:** an invalid pattern from untrusted input traps the
whole process; an error-as-value call is 121.45.

### 11. Language and compiler

- **`mt` binds a result's arms by label.** Write the arms in any order; loke's "`$ok` first"
  convention can go. A user-declared error type is discriminated across a module boundary, so
  loke's sentinel structs + predicates for `!$userErr` can go too (C400 covers exactly that).
- **An f64 (or a narrow integer) returned across modules is correct.** Drop the
  format-to-string-before-crossing workaround.
- **E4074** (new): a comparison standing as a statement (`x.a==9;`) is an error.
- **E1007** (new): a comment still open at end of file, naming the nested `(*` that took its
  `*)`. Comments nest (spec §8.10), so `COUNT(*)` inside a comment opens one — write `COUNT( * )`.
- **W1020** no longer fires on a binding named like a foreign keyword (`let def=…`); a real
  foreign-syntax use still warns.
- **Compiler caps are caught at `--check`** and the diagnostic names the count, the limit and the
  flag: `--max-funcs`, `--max-structs`, `--max-imports` (or `max_funcs`/`max_structs`/`max_imports`
  in `tkc.toml`). **The defaults for functions and struct types rise to 1024** (from 128 and 64,
  127.250), so loke's `--max-funcs`/`--max-structs` flags can go; the import cap stays 32.
  The locals caps (E9010) report once per function, located, naming the function.
- New rejections that catch real loke defects rather than change working code: `==`/`!=` on struct
  or sum values (E4031), mixed array-literal element types (E4031), a struct literal of an
  undeclared type (E3011), a `$variant(..)` constructor without exactly one payload (E4026), and
  standard-library arguments of the wrong kind (E4031, checked against the typed `.tki`).
- A result match whose value is used must have both arms (E4010) — also when it is the value of
  an expression-`if` branch (`let a=if(c){ mt g() {$ok:v v} }el{5}` is E4010) — and `mt` on a nil sum value traps
  RT005 instead of reading through a null pointer. User error payloads of type `str` or a scalar
  bind in the err arm, as do `args.get`/`env.get` payloads.

### 12. ooke 3.0.0-rc.2

- **Pin ooke rc.2** (tag `v3.0.0-rc.2` once published; branch `release/ooke-rc2` until then). Its
  floor is **toke 3.1.0** (`ooke.toml [requires] mintoke = "3.1.0"`). rc.1 does not build on toke
  after 3.0.0 (O-1) and should not be used.
- **`[server] host` defaults to `127.0.0.1`.** A deployment that must be reachable from other
  machines sets `host = "0.0.0.0"`; the start log says when a server is loopback-only and names the
  setting. `ooke new` writes it explicitly.
- **Breaking for code that builds `$ookecfg` or calls `serve` directly:** `$ookecfg` gains
  `serverhost` (16 fields), and `serve.serverun` / `serve.serverlisten` take a `host:$str` after
  `projectdir`.
- An app with more than 31 handler pages compiles: the generated registry is built with
  `--max-imports=<count>` (O-2).
- Also in rc.2: `[server] admin = true` refuses to start (ooke has no authentication to put an
  admin interface behind); `[server] corsorigins` is validated before it is applied; table and
  column names are checked as SQL identifiers.

---

## Which of loke's §9 workarounds this retires

| loke's workaround (§9) | Retired by | What to do |
|---|---|---|
| Stay pinned at `0b0bc4e` until O-1 is fixed | O-1, B7 | Pin toke `v3.1.0` and ooke `v3.0.0-rc.2`; fix loke's §5 diagnostics |
| `--max-imports` for moke's handler registry (MK6.5) | O-2 — for an ooke-generated registry only | ooke passes the flag itself; loke's own large modules still need it (default 32), and `--check` now names it |
| `--max-funcs` / `--max-structs=256` on large builds (T-25, T-54, R3) | 127.250 (default caps 1024) | Drop both flags; `--check` names the flag if a build ever passes 1024 |
| `http.postheaders` URL-first, pending B8 | B8 + B0 | Migrate to the 3-argument typed call ([3](#3-httppostheaders-and-https)); lift the non-loopback refusal |
| Avoid `!$userErr` results across modules (sentinel structs + predicates) | B2 | Use user error types; drop the quality-gate check |
| Format f64 to str before it crosses a module | B5 | Return the f64 |
| One-argument `http.get` only | B3 | **Mandatory:** it no longer compiles — use `http.geturl` or `http.clientget` |
| `$ok` first in every match | B2 | Drop the convention and its quality-gate check |
| Refuse every non-loopback send (B0) | B0 | Lift it |
| `w2tools/tkbuild.sh` for long checkout paths | N-1 | Plain `toke --out` works |
| Keychain tests in the login keychain | B4 / R5 | `keychain.create/use/destroy` |

---

## Still open

| Issue | Story | Why it is open |
|---|---|---|
| **T-17** `json.parse` | 138.9 (blocked) | Not a mis-binding: `json.parse` is a separate, deliberately public parser for scalars and integer arrays (`tk_json_parse`). Given an object it returns a value `json.getstr` reads as empty. **Owner decision pending**: alias `parse` to `decode` (breaks its documented array/string use), or type it so an object handle is rejected. **Use `json.dec` / `json.decode` for objects.** |
| **T-55** `str.charat` quadratic | 127.170 (blocked) | toke strings carry no length, so every offset check is O(N). The plan (`docs/runtime-abi.md` §3.1, a length-carrying string heap) is written and prototyped; it is an ABI change awaiting an owner decision. Measured on 3.1.0: doubling the input still costs ×4. |
| **T-59 / B9** scoped capability grants | **not yet filed** | Not addressed in 3.1.0: a quoted path or host in `[capabilities]` still grants the whole class, listen and connect are one `net` grant, and a launcher can add `--allow-*`. (3.1.0 does close several gaps where a stdlib call reached the file system or network with no grant at all — 121.22, 121.42 — which is the other direction.) A story will be filed from your `toke-issues.md` T-59. |
| `m.get` / `json.get*` / `str.toint` not forced to be matched | 127.244 | The error channel is set and matchable; the checker still types them as plain values. Enforcing it is breaking by design and awaits the owner. |
| RSA keys over 2048 bits | 121.51 | Refused in 3.1.0 (they computed wrong products); RSA-2048 works (121.48). Larger keys need a wider bignum. |

## loke's runner issues

These are defects in `run.sh` / `run2.sh`, not in toke. Each was checked against the observed
output above.

| Line | What the runner prints on 3.1.0 | Why it is wrong |
|---|---|---|
| T-31 | REPRODUCES | Its pattern `*"valid,errfirst=err"*` matches inside `invalid,errfirst=err`. The output is exactly the FIXED string the next case tests for. |
| T-27 | DEFECT | It only counts `survived` in the emitted IR. Your header allows "an unclosed-comment error at EOF" — that is E1007, and with an error there is no IR. |
| T-14 | DEFECT | It only accepts exit 11. `==` on sums is now a compile error (E4031) by design; there is no binary to exit. |
| T-33 | CHANGED | It expects `bool(false)=ok:0`; booleans now print as `false`. |
| T-56 | REPRODUCES | It calls any E9010 a reproduction. The ask was that the diagnostic name the flag, and it does (`pass --max-imports=36, or set max_imports = 36 in tkc.toml`). |
| T-16 | OK | OK for the wrong reason: the 3-argument `mockreq` call is E4026 (it takes 5), which no longer contains "no member 'mockreq'". Use the rewritten T-16. |
| T-36 | FIXED | FIXED by default: `repro_b.tk` no longer builds (`str.tof32(12345)` is E4031), so the compare check sees an empty string. The real values are in the table. |
| R5 | absent | It greps `keychain.tki` for `keychainpath`, `createkeychain` or `"path"`; the calls are `keychain.create/use/destroy`. |
| R7 | absent | `w2probes/r7_delete.tk` is not in the reproducer set, so nothing is built or sent. |
| R8 | single-conn | `w2probes/r8_twodb.tk` and `r8_tx.tk` are missing, and the verdict word is hard-coded. |
| O-1 | DEFECT | It compiles rc.1's `store.tk` (a byte copy of the tag), which stays broken; rc.2 is the fix. Point it at rc.2's `src/store.tk`. |

The runner also cannot build the reproducers whose call is now a compile error — T-10, T-15, T-32,
T-48, T-49, T-57 (it prints `note`/`CHANGED` with "No such file or directory"). That is the design
change working; Appendix A has a rewrite for each.

---

## Appendix A — rewritten reproducers

Each exercises the property of the original through the 3.1.0 call. Recorders as in `w2tools`
(`rec.sh start PORT LOG`), loopback only.

```toke
(* T-57: 300 client GETs; recorder on 127.0.0.1:11445. Observed: calls=300 ok_arm=300, 300 GET /health *)
m=p57;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let hc=http.client("http://127.0.0.1:11445");
  let oks=mut.0;
  lp(let k=0;k<300;k=k+1){
    let up=mt http.clientget(hc;"/health") { $ok:r true; $err:e false };
    if(up){ oks=oks+1 }el{}
  };
  io.println("clientget calls=300 ok_arm=\(oks)");
  <0
};
```

```toke
(* T-15: closed port. Observed: clientget=err code=0 msg=TCP connect failed geturl=err code=0 *)
m=p15;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let c=http.client("http://127.0.0.1:1");
  io.println(mt http.clientget(c;"/health") { $ok:r "clientget=ok"; $err:e "clientget=err code=\(e.code) msg=\(e.msg)" });
  io.println(mt http.geturl("http://127.0.0.1:1/health") { $ok:s "geturl=ok"; $err:e "geturl=err code=\(e.code)" });
  <0
};
```

```toke
(* T-10 / R7 / N-6: recorder on 11446. Observed: POST /post body-p, PUT /put body-u, DELETE /del; each $ok 200 *)
m=p10;
i=io:std.io;
i=http:std.http;
i=str:std.str;
f=main():i64{
  let c=http.client("http://127.0.0.1:11446");
  io.println(mt http.clientpost(c;"/post";str.bytes("body-p");"text/plain") { $ok:r "post=ok status=\(r.status)"; $err:e "post=err" });
  io.println(mt http.clientput(c;"/put";str.bytes("body-u");"text/plain") { $ok:r "put=ok status=\(r.status)"; $err:e "put=err" });
  io.println(mt http.clientdelete(c;"/del") { $ok:r "delete=ok status=\(r.status)"; $err:e "delete=err" });
  <0
};
```

```toke
(* T-48 / R6 (http; https is toke's C280). Observed: ok status=200; x-api-key=k1, one content-type *)
m=p48;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let r=mt http.postheaders("http://127.0.0.1:11443/v1/messages-loke";"{\"n\":1}";@(@("x-api-key";"k1");@("content-type";"application/json"))) { $ok:v "ok status=\(v.status)"; $err:e "err \(e.code)" };
  io.println("postheaders=\(r)");
  <0
};
```

```toke
(* T-32: nested read. Observed: usage.input_tokens=150 *)
m=p32;
i=io:std.io;
i=json:std.json;
f=main():i64{
  let body="{\"usage\":{\"input_tokens\":150,\"output_tokens\":7}}";
  let n=mt json.dec(body) {
    $ok:j (mt json.at(j;"usage") { $ok:u (mt json.i64(u;"input_tokens") { $ok:v v; $err:e 0-2 }); $err:e 0-3 });
    $err:e 0-1
  };
  io.println("usage.input_tokens=\(n)");
  <0
};
```

```toke
(* T-49: Observed: fd>0=true portavailable-after=0 *)
m=p49;
i=io:std.io;
i=net:std.net;
f=main():i64{
  let fd=net.listen("127.0.0.1:11444");
  io.println("fd>0=\(fd>0) portavailable-after=\(net.portavailable(11444))");
  <0
};
```

```toke
(* R8 / T-30: two connections. Observed: ha=1 hb=2, A.db has ta and ta2, B.db has tb *)
m=pr8;
i=io:std.io;
i=db:std.db;
f=main():i64{
  let a=mt db.open("A.db") { $ok:h h; $err:e 0 };
  let b=mt db.open("B.db") { $ok:h h; $err:e 0 };
  io.println("ha=\(a) hb=\(b)");
  let ra=mt db.execon(a;"CREATE TABLE ta(x INTEGER); CREATE TABLE ta2(y INTEGER);";@()) { $ok:n "ok"; $err:e "err" };
  let rb=mt db.execon(b;"CREATE TABLE tb(x INTEGER)";@()) { $ok:n "ok"; $err:e "err" };
  io.println("execon a=\(ra) b=\(rb) closea=\(db.closeon(a)) closeb=\(db.closeon(b))");
  <0
};
```

```toke
(* T-16: Observed: status=200 body=[got POST /x body=hello] *)
m=p16;
i=io:std.io;
i=http:std.http;
f=h(req:$req):$res{
  <http.res.ok("got \(req.method) \(req.path) body=\(req.body)")
};
f=main():i64{
  let r=h(http.mockreq("POST";"/x";@(@("content-type";"text/plain"));"hello";@()));
  io.println("status=\(http.resstatus(r)) body=[\(http.resbody(r))]");
  <0
};
```

## Where this is tracked on our side

`docs/progress.md` Epic 138 (138.1–138.18); the release is 138.17. The full list of changes since
3.0.0, breaking changes first, is `CHANGELOG.md` § 3.1.0.
