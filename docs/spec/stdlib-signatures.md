---
title: toke Standard Library — Normative Signatures
slug: stdlib-signatures
section: spec
order: 4
---

**Status:** held to the interfaces (story 127.456).

**The normative signature of each module is its interface file, `stdlib/<module>.tki`**
— the file `tkc` type-checks every `std` call against (as toke-spec-v0.4 §F says of
`str`: "Normative signature lives in `stdlib/str.tki`"). This page restates those
signatures for readers and is checked against them: `scripts/check_stdlib_signatures.py`
(run by `make check-tki`) fails when a hand-written entry below names a function the
interface does not export, or states a parameter count or a type that differs from it.
The last section is generated from the interfaces: every module with no hand-written
block, and every export a hand-written block leaves out. A signature changes in the `.tki`
first, with its spec amendment, module doc, tests and toke-eval/benchmark updates
together; this page follows.

## Modules

- `std.str` — string operations (len, concat, slice, split, case, encoding)
- `std.json` — JSON encoding, decoding, and typed field extraction
- `std.toon` — TOON (Token-Oriented Object Notation) — default serialization format
- `std.yaml` — YAML encoding, decoding, and typed field extraction
- `std.i18n` — internationalisation — locale-aware string bundles with placeholder substitution
- `std.http` — HTTP request/response handling, routing, and HTTP client with connection pooling
- `std.ws` — WebSocket client/server: connect, send, recv, broadcast
- `std.sse` — Server-Sent Events: emit, keepalive, connection lifecycle
- `std.db` — database queries (SQLite3 backend)
- `std.file` — file I/O (read, write, append, list, delete) and byte-exact binary access (`readbytes`/`writebytes`)
- `std.env` — environment variable access
- `std.fmt` — value formatting for print (bool, i64/str arrays, fixed-decimal f64, padding)
- `std.process` — subprocess spawning and control
- `std.crypto` — SHA-256, SHA-512, HMAC-SHA-256, HMAC-SHA-512, constant-time compare, random bytes, hex encoding
- `std.time` — time operations (now, format, since)
- `std.log` — structured logging
- `std.test` — test assertions

## Serialization Strategy

toke uses a **TOON-first serialization strategy**: TOON for tabular data, YAML and JSON as secondary formats. The TOON project reports 30-60% fewer tokens than equivalent JSON for uniform arrays (their published benchmark; not reproduced here). String externalisation for internationalisation via `std.i18n`. See [ADR-0003](/docs/decisions/ADR-0003/).

## Function Signatures

### std.str

```text
f=len(s:$str):u64
f=concat(a:$str;b:$str):$str
f=slice(s:$str;start:u64;end:u64):$str!$sliceerr
f=substr(s:$str;start:u64;end:u64):$str!$sliceerr
f=split(s:$str;sep:$str):@$str
f=upper(s:$str):$str
f=lower(s:$str):$str
f=trim(s:$str):$str
f=contains(s:$str;sub:$str):bool
f=replace(s:$str;old:$str;new:$str):$str
f=starts(s:$str;prefix:$str):bool
f=ends(s:$str;suffix:$str):bool
```

`substr` is `slice` under a second name: the same glue, the same arguments and
the same `$sliceerr`, as are the receiver spellings `s.slice(a;b)`,
`s.substr(a;b)` and `s.substring(a;b)` (story 127.493 typed its interface
record, which had been an untyped `"gen":"abi"` placeholder).

Regular expressions (one pattern language, `docs/stdlib/str.md` "Regular
expressions"; amended 2026-09-30, story 121.45: `checkre` and `$regexerr`):

```text
t=$regexerr{msg:$str;pos:i64}
f=checkre(pattern:$str):$str!$regexerr
f=containsre(s:$str;pattern:$str):i64
f=matches(s:$str;pattern:$str):i64
f=countre(s:$str;pattern:$str):i64
f=findall(s:$str;pattern:$str):@$str
f=replacere(s:$str;pattern:$str;replacement:$str):$str
```

`checkre` returns the pattern unchanged, or the reason it is not in the
language and the byte offset (`-1` when not at a byte). A pattern it accepts
never traps RT008 in the five matching calls, which is what they do with an
invalid pattern computed at run time; a literal invalid pattern passed to one
of them is E4075 at compile time.

### std.json

Mirrors `stdlib/json.tki` (135.27; conform C353 holds the two together).

```text
t=$jsonval{raw:$str}
t=$jsonerr{$parse:$str;$type:$str;$missing:$str}
f=enc(s:$str):$str
f=dec(s:$str):$jsonval!$jsonerr
f=str(j:$jsonval;key:$str):$str!$jsonerr
f=u64(j:$jsonval;key:$str):u64!$jsonerr
f=i64(j:$jsonval;key:$str):i64!$jsonerr
f=f64(j:$jsonval;key:$str):f64!$jsonerr
f=bool(j:$jsonval;key:$str):bool!$jsonerr
f=arr(j:$jsonval;key:$str):@$jsonval!$jsonerr
f=at(j:$jsonval;path:$str):$jsonval!$jsonerr   (* 138.12: dotted path, e.g. "usage.input_tokens", "r.0" *)
f=getval(j:$jsonval;path:$str):$str        (* 138.12: the raw JSON text at path, "" if none *)

(* 113.B.5: the object surface.  get names one member exactly (no path
   split: a key may hold '.'), at walks a dotted path; both return the value,
   of any JSON type, as a $jsonval.  The scalar readers above take the key ""
   on a non-object $jsonval to mean the value itself (as arr does for a
   top-level array), so a value from get/at/arr reads as str/i64/... too. *)
f=get(j:$jsonval;key:$str):$jsonval!$jsonerr
f=keys(j:$jsonval):@$str!$jsonerr          (* names in document order; {} -> @() *)
f=entries(j:$jsonval):@$str!$jsonerr       (* "key=<value as JSON text>" per member *)
f=has(j:$jsonval;key:$str):bool
f=haskey(j:$jsonval;key:$str):bool         (* alias of has *)

(* 113.B.5: building output.  $jsonval is immutable: every set* returns a NEW
   object with the member set (replaced in place, or appended), and $type if
   j is not an object.  The key is always JSON-encoded. *)
f=newobj():$jsonval                        (* {} *)
f=set(j:$jsonval;key:$str;v:$str):$jsonval!$jsonerr      (* v as a JSON string *)
f=setint(j:$jsonval;key:$str;v:i64):$jsonval!$jsonerr
f=setf64(j:$jsonval;key:$str;v:f64):$jsonval!$jsonerr    (* NaN/inf: $type *)
f=setbool(j:$jsonval;key:$str;v:bool):$jsonval!$jsonerr
f=setval(j:$jsonval;key:$str;v:$jsonval):$jsonval!$jsonerr (* nests v; $parse if v is not one JSON value *)
f=serialize(j:$jsonval):$str               (* the JSON text, for output *)

(* 127.241: the typed get* spellings -- aliases of i64 / bool / f64 above.
   Their document parameter is still the ABI's i64 (a json handle); the key
   is a str (127.304: it was spelled i64, so a str key went unjudged and an
   i64 key was accepted).  The result is the declared union, so a match
   decides on the error slot. *)
f=getint(j:i64;key:$str):i64!$jsonerr
f=geti64(j:i64;key:$str):i64!$jsonerr
f=geti32(j:i64;key:$str):i64!$jsonerr
f=getbool(j:i64;key:$str):bool!$jsonerr
f=getf32(j:i64;key:$str):f64!$jsonerr
f=getfloat(j:i64;key:$str):f64!$jsonerr

(* Streaming API — 12.1.1.  Declared in json.tki but quarantined: no glue
   reaches the C core yet, so a call is refused at check with E4027
   (docs/reference/tki-quarantine.md; 102.16) *)
t=$jsonstream{buf:@$byte;pos:u64;depth:u64;state:u64}
t=$jsontoken{$objectstart:void;$objectend:void;$arraystart:void;$arrayend:void;$key:$str;$str:$str;$u64:u64;$i64:i64;$f64:f64;$bool:bool;$null:void;$end:void}
t=$jsonstreamerr{$truncated:$str;$invalid:$str;$overflow:$str}
t=$jsonwriter{buf:@$byte;pos:u64}
f=json.streamparser(input:@$byte):$jsonstream
f=json.streamnext(parser:$jsonstream):$jsontoken!$jsonstreamerr
f=json.streamemit(writer:$jsonwriter;val:$jsonval):void!$jsonstreamerr
f=json.newwriter(capacity:u64):$jsonwriter
f=json.writerbytes(writer:$jsonwriter):@$byte
```

ABI helpers. `json.tki` exports these with `"gen":"abi"`: their parameter and
return types are `i64` placeholders, so only the name and arity are
established, and they are listed that way (`name/arity`):
`arrget/2`, `arrlen/1`, `decode/1`, `encarr/3`, `encbool/1`, `encf64/1`, `encnum/1`, `encode/1`, `encstr/1`, `encval/2`, `err/1`, `getarr/2`, `getarray/2`, `getobj/2`, `getopt/2`, `getregulations/1`, `getstr/2`, `getstring/2`, `isnullkey/2`, `kv/2`, `len/1`, `obj/1`, `ok/1`, `parse/1`, `parsearray/1`, `parseobj/1`, `print/1`, `printarr/3`, `printval/2`, `quote/1`, `strarr/1`, `strarray/1`, `stringify/1`, `tryarr/2`, `trybool/2`, `tryboolkey/2`, `tryfloat/2`, `tryfloatkey/2`, `tryint/2`, `tryintkey/2`, `trystr/2`, `trystrkey/2`, `values/1`.

### std.toon

```text
f=enc(data:$str):$str
f=dec(s:$str):$toonval!$toonerr
f=str(t:$toonval;key:$str):$str!$toonerr
f=i64(t:$toonval;key:$str):i64!$toonerr
f=f64(t:$toonval;key:$str):f64!$toonerr
f=bool(t:$toonval;key:$str):bool!$toonerr
f=arr(t:$toonval;key:$str):@$toonval!$toonerr
f=fromjson(s:$str):$str
f=tojson(s:$str):$str
```

### std.yaml

```text
f=enc(data:$str):$str
f=dec(s:$str):$yamlval!$yamlerr
f=str(y:$yamlval;key:$str):$str!$yamlerr
f=i64(y:$yamlval;key:$str):i64!$yamlerr
f=f64(y:$yamlval;key:$str):f64!$yamlerr
f=bool(y:$yamlval;key:$str):bool!$yamlerr
f=arr(y:$yamlval;key:$str):@$yamlval!$yamlerr
f=fromjson(s:$str):$str
f=tojson(s:$str):$str
```

### std.i18n

```text
f=load(dir:$str;locale:$str):$i18nbundle!$i18nerr
f=get(bundle:$i18nbundle;key:$str):$str
f=fmt(bundle:$i18nbundle;key:$str;arg:$str):$str
f=locale():$str
```

### std.http

```text
(* server-side routing *)
f=param(req:$req;key:$str):$str!$httprouteerr
f=header(req:$req;key:$str):$str!$httprouteerr
f=res.ok(body:$str):$res
f=res.json(status:u16;body:$str):$res
f=res.bad(msg:$str):$res
f=res.err(msg:$str):$res
http.get(path:$str;handler:func)
http.post(path:$str;handler:func)
http.put(path:$str;handler:func)
http.delete(path:$str;handler:func)
http.patch(path:$str;handler:func)
(* testing a handler in-process (138.16): mockreq builds the request a
   handler receives; the res* functions read the response it returned *)
f=http.mockreq(method:$str;path:$str;headers:@(@($str));body:$str;params:@(@($str))):$req
f=http.resstatus(res:$res):u64
f=http.resbody(res:$res):$str
f=http.resheader(res:$res;name:$str):$str
(* server start: block until shut down (0); -1 and exit status 1 if the
   server cannot start.  Port 0 = any free port, reported on stderr.  The
   port-only forms bind all interfaces; the ...on forms bind host (an IPv4
   literal, "localhost", or ""/"*"/"0.0.0.0" for all) -- 135.24, 138.1 *)
f=http.serve(port:u64):i64
f=http.serveworkers(port:u64;workers:u64):i64
f=http.servetls(port:u64;cert:$str;key:$str):i64
f=http.servetlsworkers(port:u64;cert:$str;key:$str;workers:u64):i64
f=http.serveon(host:$str;port:u64):i64
f=http.serveworkerson(host:$str;port:u64;workers:u64):i64
f=http.servetlson(host:$str;port:u64;cert:$str;key:$str):i64
f=http.servetlsworkerson(host:$str;port:u64;cert:$str;key:$str;workers:u64):i64
(* client-side: types.  timeoutms is a PER-READ timeout (SO_RCVTIMEO,
   restarting whenever data arrives), not a per-request one (138.34);
   http.withdeadline bounds a request as a whole. *)
t=$httpclient{baseurl:$str;poolsize:u64;timeoutms:u64}
t=$httpreq{method:$str;url:$str;headers:@($str:$str);body:@$byte}
t=$httpresp{status:u64;headers:@($str:$str);body:@$byte}
t=$httperr{msg:$str;code:u64}
t=$httpstream{id:u64;open:bool}
(* client-side: functions.  138.3: the client calls have names of their own;
   http.get/post/put/delete/patch are route registration only (above).  This
   block used to overload them by argument type, which no implementation
   ever resolved: a client call on a route name registered a route. *)
f=http.client(baseurl:$str):$httpclient
f=http.withproxy(c:$httpclient;proxyurl:$str):$httpclient
(* request controls (spec amendment 138.24, owner answers 2026-11-15 2.1 and
   2.2; docs/decisions/138.24-http-client-controls.md).  Each returns a copy
   of c with one setting changed; a client from http.client has none.  They
   apply to clientget/post/put/delete; http.stream and http.downloadfile
   refuse a client carrying one.
   withdeadline: the whole request (connect, TLS handshake, send, response
     headers and body) within ms of the call, else $err "request deadline
     exceeded (<ms> ms) <stage>"; 0 has already passed.
   withmaxbody: a response body over bytes is $err "response body exceeds
     the cap (<bytes> bytes): ..." and the read stops; never a cut body $ok.
   withconnectaddr: connect to addr, an IPv4/IPv6 literal, with no lookup;
     Host, SNI and certificate verification use the URL's host; a
     non-literal addr, or one with withproxy, is $err at request time. *)
f=http.withdeadline(c:$httpclient;ms:u64):$httpclient
f=http.withmaxbody(c:$httpclient;bytes:u64):$httpclient
f=http.withconnectaddr(c:$httpclient;addr:$str):$httpclient
f=http.clientget(c:$httpclient;path:$str):$httpresp!$httperr
f=http.clientpost(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.clientput(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.clientdelete(c:$httpclient;path:$str):$httpresp!$httperr
f=http.geturl(url:$str):$str!$httperr
f=http.postheaders(url:$str;body:$str;headers:@(@($str))):$httpresp!$httperr
(* 138.27: postheaders without a body, as GET; 138.8's rules: Host and
   Content-Length are the client's, CR/LF in a name or value is $err *)
f=http.getheaders(url:$str;headers:@(@($str))):$httpresp!$httperr
f=http.stream(c:$httpclient):$httpstream!$httperr
(* 121.53 (2), 2026-09-30: the implemented and published (http.tki) form is
   the client alone -- a GET of its base URL.  The (c;req) form this file
   used to list was never implemented: the glue read a URL, and the core
   (http_stream in http.c) would send req's method, path and body but not its
   headers, so restoring it needs an http.c change first. *)
f=http.streamnext(s:$httpstream):@$byte!$httperr
```

### std.net

```text
(* 138.25 (spec amendment 138.24, owner answer 2026-11-15 2.2): every
   address name resolves to, A and AAAA, numeric strings, each once, in the
   resolver's order; needs the net capability (CAP001 without it).
   neterr has httperr's shape; code 0 = no number of its own. *)
t=$neterr{msg:$str;code:u64}
f=net.resolve(name:$str):@($str)!$neterr
(* the IPv4 server surface (net.listen and the untyped ABI entries beside
   it) is described in docs/stdlib/net.md and stdlib/net.tki *)
```

### std.db

```text
f=open(path:$str):u64!$dberr
f=close():i64
f=exec(sql:$str;params:@($str)):u64!$dberr
f=one(sql:$str;params:@($str)):$dbrow!$dberr
f=many(sql:$str;params:@($str)):@($dbrow)!$dberr
f=execon(h:u64;sql:$str;params:@($str)):u64!$dberr
f=oneon(h:u64;sql:$str;params:@($str)):$dbrow!$dberr
f=manyon(h:u64;sql:$str;params:@($str)):@($dbrow)!$dberr
f=closeon(h:u64):bool
```

Every `open` returns a new handle; the `*on` forms name one, and the
handle-less forms use the default connection -- the lowest-numbered one still
open (138.6). `exec` runs every `;`-separated statement of its string or
fails naming the one that failed; bind parameters with more than one
statement are refused. `$dbrow` values are read with `row.str/u64/i64/f64/bool`.

### std.keychain

```text
f=isavailable():bool
f=set(service:$str;account:$str;secret:$str):bool
f=get(service:$str;account:$str):?($str)
f=delete(service:$str;account:$str):bool
f=exists(service:$str;account:$str):bool
f=laststatus():i64
f=lasterrkind():$str
f=lasterr():$str
f=create(path:$str;password:$str):bool
f=use(path:$str):bool
f=destroy(path:$str):bool
```

After a `false` or empty result, `lasterrkind` is `"notfound"` exactly when the
item is absent; any other non-empty kind (`locked denied nokeychain badarg
unavailable failed`) means the call failed. `create`/`use`/`destroy` name a
throwaway keychain file (macOS); `use("")` returns to the default; `destroy`
refuses the default keychain and any on the user's search list (138.4).

### std.file

```text
f=read(path:$str):$str!$fileerr
f=write(path:$str;data:$str):bool!$fileerr
f=append(path:$str;data:$str):bool!$fileerr
f=list(dir:$str):@$str!$fileerr
f=delete(path:$str):bool!$fileerr
f=readbytes(path:$str):@(byte)!$fileerr
f=writebytes(path:$str;data:@(byte)):bool!$fileerr
f=readrange(path:$str;offset:i64;len:i64):@(byte)!$fileerr
f=size(path:$str):i64!$fileerr
f=realpath(path:$str):$str!$fileerr
```

`read` and `write` carry a `$str`, which is NUL-terminated: they truncate at
the first zero byte and are therefore **text-only**. `readbytes`/`writebytes`
(135.10) are the byte-exact pair and are what every binary format — archives,
PDFs, images, spreadsheets — must use. An empty `@(byte)` means an empty file,
never a failure; the `$err` arm binds a `$fileerr` whose variant says which of
`$notfound $permission $io $invalid $isdir $notregular $symlink $toolarge
$nomem $badarg` occurred and whose payload is the message (127.102b retired
`file.lasterr`/`file.lasterrkind`, which carried the same two facts). `readbytes` refuses a file over 64 MiB rather than attempting it,
because a `@(byte)` costs one i64 per byte.

`readrange`/`size` (135.12) are how a file bigger than that is read. The cap is
not raised — the memory cost is the reason it exists — instead the two limits
that were one number are separated: `readrange` caps the **window**, because
the window is what is materialised, and does not check the **file's** size at
all. A short result is an end rather than a failure, and an offset at or beyond
the end is a real zero-length `@(byte)` marked ok. `size` is the call a parser
makes first, because a PDF's xref table and a zip's end-of-central-directory
record both live at the *end* of the file. These two are also the first calls
in the module to report failure through the error slot rather than a value
sentinel — an empty file's `size` **is** `0`, so no value could have carried
it.

`realpath` (121.60) is the absolute path with every `.`, `..` and symlink
resolved (realpath(3)), or the `$fileerr` saying why not. It is the only
`std.file` call that follows symlinks, because resolving them is its purpose;
it needs the `fs.read` grant. See [std.file](/docs/stdlib/file).

### std.io

```text
f=print(s:$str):i64
f=println(s:$str):i64
f=eprint(s:$str):void
f=eprintln(s:$str):i64
f=printf(fmt:$str;val:$str):i64
f=readln():$str
f=eof():i64
```

Console text: `print`/`println` to stdout, `eprint`/`eprintln` to stderr, the
`ln` forms adding a newline. `eprint` (121.70) is the stderr write with no
newline added, so a program no longer reaches into `std.os` for
`os.write(os.stderrfd(); s)`. Every export but `eprint` is `"gen":"abi"` in
io.tki (only its arity is established). `readln` returns `""` both for a blank
line and at end of input; `eof` tells them apart. See [std.io](/docs/stdlib/io).

### std.os

```text
t=$oserr{msg:$str;code:i64}
f=open(path:$str;flags:i64;mode:i64):i64
f=close(fd:i64):i64
f=read(fd:i64;count:i64):$str
f=write(fd:i64;data:$str):i64!$oserr
f=lseek(fd:i64;offset:i64;whence:i64):i64
f=stat(path:$str):i64
f=unlink(path:$str):i64
f=rename(from:$str;to:$str):i64
f=mkdir(path:$str;mode:i64):i64
f=rmdir(path:$str):i64
f=access(path:$str;mode:i64):i64
f=getcwd():$str
f=getpid():i64
f=exit(status:i64):i64
f=getenv(name:$str):$str
f=setenv(name:$str;value:$str):i64
f=errno():i64
f=strerror(code:i64):$str
f=ordonly():i64
f=owronly():i64
f=ordwr():i64
f=ocreat():i64
f=otrunc():i64
f=oappend():i64
f=onofollow():i64
f=stdinfd():i64
f=stdoutfd():i64
f=stderrfd():i64
```

A thin POSIX bridge: each call returns what the syscall returns (`-1` on
failure, with the reason in `errno()`), except `write`. `write` (121.70)
writes every byte of `data`, retrying `EINTR` and continuing after a short
write, and returns the byte count; a failure is the `$err` arm, whose `code`
is the errno and whose `msg` names it and the bytes written before it. **This
is a behaviour change:** until 121.70 `write` made one write(2) and returned
`0` on error -- the same value a write of `""` returns. See
[std.os](/docs/stdlib/os).

### std.toml

```text
t=$tomlval{}
t=$tomlerr{msg:$str}
f=load(src:$str):$tomlval!$tomlerr
f=loadfile(path:$str):$tomlval!$tomlerr
f=str(v:$tomlval;key:$str):$str!$tomlerr
f=i64(v:$tomlval;key:$str):i64!$tomlerr
f=f64(v:$tomlval;key:$str):f64!$tomlerr
f=bool(v:$tomlval;key:$str):bool!$tomlerr
f=has(v:$tomlval;key:$str):bool
f=section(v:$tomlval;key:$str):$tomlval!$tomlerr
f=strarr(v:$tomlval;key:$str):@$str!$tomlerr
f=i64arr(v:$tomlval;key:$str):@i64!$tomlerr
f=f64arr(v:$tomlval;key:$str):@f64!$tomlerr
f=boolarr(v:$tomlval;key:$str):@bool!$tomlerr
f=sectionarr(v:$tomlval;key:$str):@$tomlval!$tomlerr
f=keys(v:$tomlval):@$str
f=len(v:$tomlval):u64
f=free(v:$tomlval):bool
```

`$tomlval` is an opaque table handle. Each accessor reads one key of the
table it is given (not a dotted path); `section` descends. `f64` and `has`
(121.68): `f64` reads a float, or a decimal integer as the nearest `f64`, and
its `$err` says `key not found` or `key is not a float`; `has` is `true` for a
key present with any value, arrays and tables included. `loadfile` needs the
`fs.read` grant.

113.B.4: every accessor's `$err` says `key not found` for an absent key and
`key is not a <type>` (`string`, `integer`, `boolean`, `table`) for a key of
another type.  `strarr`/`i64arr`/`f64arr`/`boolarr`/`sectionarr` read the array at
`key` whole, each element as its scalar accessor would (`sectionarr`: the
`[[x]]` form, as views); an empty array is `$ok` with length 0, a mixed one
`array element N is not a <type>`.  `keys` lists the table's own keys
(values, then arrays, then sub-tables, document order within each; copies),
`len` counts them.  `free` releases a document from `load`/`loadfile` and is
`true`; on a view, or a document already freed, it does nothing and is
`false`.  `f64arr` (113.B.4) reads each element as `f64` does: a float, or
a decimal integer as the nearest `f64`. See [std.toml](/docs/stdlib/toml).

### std.env

```text
f=get(key:$str):$str!$enverr
f=set(key:$str;val:$str):bool
```

### std.fmt

```text
f=bool(b:bool):$str
f=arr(xs:@i64;sep:$str):$str
f=strs(xs:@$str;sep:$str):$str
f=f64(x:f64;prec:i64):$str
f=pad(s:$str;width:i64;left:bool):$str
```

Module style only (`i=fmt:std.fmt;`). Every function returns a fresh string; `fmt.f64` renders exactly `prec` digits after the point (0..20, clamped); `fmt.pad` pads with spaces to `width` code points, before `s` when `left` is true.

### std.process

```text
f=exec(cmd:$str):$str
f=spawn(argv:@$str):$handle!$processerr
f=wait(h:$handle):i32
f=kill(h:$handle):bool
```

### std.crypto

```text
f=sha256(data:@$byte):@$byte
f=sha512(data:@$byte):@$byte
f=hmacsha256(key:@$byte;data:@$byte):@$byte
f=hmacsha512(key:@$byte;data:@$byte):@$byte
f=constanteq(a:@$byte;b:@$byte):bool
f=randombytes(n:u64):@$byte
f=tohex(data:@$byte):$str
```

### std.time

```text
f=now():u64
f=format(ts:u64;layout:$str):$str
f=since(ts:u64):u64
```

### std.log

```text
f=info(msg:$str;fields:@(@$str)):bool
f=warn(msg:$str;fields:@(@$str)):bool
f=error(msg:$str;fields:@(@$str)):bool
f=debug(msg:$str;fields:@(@$str)):bool   (* "gen":"abi" in log.tki: only the arity is declared *)
f=closeerror():bool
```

`closeerror` (136.77) flushes, closes and unregisters the error log
`log.openerror` opened; `true` if one was open. See [std.log](/docs/stdlib/log).

### std.test

```text
f=eq(a:i64;b:i64):bool
f=ok(cond:bool):void
f=fail(msg:$str):void
```

### std.ws

```text
t=$wsconn{id:u64;ready:bool}
t=$wsmsg{payload:@$byte;fin:bool;opcode:u64}
t=$wserr{msg:$str}
f=ws.connect(url:$str):$wsconn!$wserr
f=ws.send(conn:$wsconn;msg:$str):void!$wserr
f=ws.sendbytes(conn:$wsconn;data:@$byte):void!$wserr
f=ws.recv(conn:$wsconn):$wsmsg!$wserr
f=ws.close(conn:$wsconn):void
f=ws.broadcast(conns:@$wsconn;msg:$str):void
```

### std.sse

```text
t=$ssectx{id:u64;open:bool}
t=$sseevent{id:$str;event:$str;data:$str;retry:u64}
t=$sseerr{msg:$str}
f=sse.emit(ctx:$ssectx;event:$sseevent):void!$sseerr
f=sse.emitdata(ctx:$ssectx;data:$str):void!$sseerr
f=sse.close(ctx:$ssectx):void
f=sse.keepalive(ctx:$ssectx;interval:u64):void
```

### std.tls

Amended 2026-10-06, story 121.64 (owner decision 2026-10-01 §3.1): the
`insecure` field and `tls.insecureconfig`, and client verification by default.

```text
t=$tlsconfig{certpem:$str;keypem:$str;peercertpem:$str;requiremutual:bool;insecure:bool}
t=$tlsconn{id:$str}
t=$tlskeypair{certpem:$str;keypem:$str}
f=tls.genselfsigned(commonname:$str;validdays:i32):$tlskeypair!$str
f=tls.genselfsignedalg(commonname:$str;validdays:i32;keyalg:$str):$tlskeypair!$str
f=tls.certof(kp:$tlskeypair):$str
f=tls.keyof(kp:$tlskeypair):$str
f=tls.freekeypair(kp:$tlskeypair):bool
f=tls.tlsconfig(certpem:$str;keypem:$str):$tlsconfig
f=tls.pinconfig(certpem:$str;keypem:$str;peercertpem:$str):$tlsconfig
f=tls.mutualconfig(certpem:$str;keypem:$str;peercertpem:$str):$tlsconfig
f=tls.insecureconfig(certpem:$str;keypem:$str):$tlsconfig
f=tls.freeconfig(cfg:$tlsconfig):bool
f=tls.listen(port:i32;cfg:$tlsconfig;cb:fn):bool
f=tls.connect(host:$str;port:i32;cfg:$tlsconfig):?($tlsconn)
f=tls.read(conn:$tlsconn):?($str)
f=tls.write(conn:$tlsconn;data:$str):bool
f=tls.close(conn:$tlsconn):bool
f=tls.protocol(conn:$tlsconn):$str
f=tls.group(conn:$tlsconn):$str
f=tls.peercert(conn:$tlsconn):?($str)
f=tls.fingerprint(pem:$str):$str
f=tls.pairingcode(conn:$tlsconn):$str
```

TLS 1.3 only. A client authenticates the server by exactly one rule, chosen
by the configuration: a non-empty `peercertpem` accepts only that certificate
(pinning; no chain or host-name check); `insecure` checks nothing; otherwise
the chain must verify against the default trust store (`SSL_CERT_FILE` /
`SSL_CERT_DIR` override it) and the certificate must name `host`. A literal
that omits `insecure` gets `false`. On a server, `insecure` has no effect. See
[std.tls](/docs/stdlib/tls).

### std.zip

```text
t=$ziparchive{}
t=$zipentry{name:$str;size:i64;compressedsize:i64;isdir:bool}
t=$ziperr{$badarchive:$str;$badentry:$str;$notfound:$str;$toolarge:$str;$unsupported:$str;$io:$str}
f=zip.open(data:@$byte):$ziparchive!$ziperr
f=zip.openfile(path:$str):$ziparchive!$ziperr
f=zip.entries(a:$ziparchive):@$zipentry
f=zip.read(a:$ziparchive;name:$str):@$byte!$ziperr
f=zip.close(a:$ziparchive):void
```

Read-only; Store and Deflate only. `$ziparchive` is an opaque handle with no readable fields. Opening validates the whole central directory first, so a single bad entry rejects the archive: absolute or `..`-bearing or backslash-bearing names, a cumulative uncompressed size over 64 MiB, a ratio over 200:1, more than 65536 entries, an archive over 128 MiB, an encrypted entry, or any other compression method. `zip.openfile` is an addition to the requested interface, because toke has no binary file read (`file.read` truncates at the first NUL). Every failure raises a `$ziperr` whose variant names the class of rule that rejected the input and whose payload is the reason (`$err:e mt e {$toolarge:m …}`); `zip.lasterr` was retired by 127.102c. See [std.zip](/docs/stdlib/zip).

### std.xml

Mirrors `stdlib/xml.tki` (131.48).

```text
f=element(tag:$str;content:$str;attrs:$str):$str
f=elementns(ns:$str;tag:$str;content:$str;attrs:$str):$str
f=escape(s:$str):$str
f=cdata(s:$str):$str
f=comment(text:$str):$str
f=declaration(version:$str;encoding:$str):$str
f=get(xml:$str;path:$str):$str
f=attr(attrs:$str;name:$str):$str
```

Builders take `attrs` as raw attribute text (`href="/" id='7'`), inserted
verbatim after the tag name; `content` is inserted verbatim too, so text
that may hold markup goes through `escape` (or `cdata`) first. `get` parses
`xml` and returns the text content of the first element at the dotted tag
path (`"root.name"`, `"a.c"`) whose text is non-empty;
`attr` reads one attribute from a raw attribute string. Both return `""`
when nothing matches -- as `json.getval` does -- so a present-but-empty
element and a missing one read the same. `elementns` with an empty `ns`
emits an unprefixed tag.

ABI helper. `xml.tki` exports `parse/1` with `"gen":"abi"`: it returns an
opaque node-array handle, and no export reads or frees one, so it has no
use from toke yet.

### std.soap

Mirrors `stdlib/soap.tki` (131.48).

```text
f=envelope(body:$str;header:$str):$str
f=fault(code:$str;string:$str;detail:$str):$str
f=extractbody(envelope:$str):$str
f=extractfault(envelope:$str):$str
f=wsseusernametoken(user:$str;pass:$str):$str
f=wssetimestamp(created:$str;expires:$str):$str
```

`envelope` wraps `body` (and a non-empty `header`) in a SOAP 1.1
`soap:Envelope`. `extractbody` returns the inner XML of the first `soap:Body`,
`SOAP-ENV:Body` or unprefixed `Body`, and `extractfault` the whole first
`Fault` element in the same prefix order; each returns `""` when there is
none. `fault` XML-escapes its three fields, and the `wsse*` calls build
WS-Security header fragments from their escaped arguments (a PasswordText
`UsernameToken`, a `Timestamp`); they do no hashing or clock reads.

## Generated from the interfaces

Generated from `stdlib/*.tki`, so that every public type and every function whose types
the interface declares appears on this page. A module with no hand-written block above is
listed whole as `std.<module> (generated)`; a module with one gets a
`std.<module>: exports not listed above` block for whatever that block leaves out, so a
new export in any module shows up here (and `make check-tki` fails until this section is
re-rendered with `make render-stdlib-signatures`). Parameter names are positional (`a`,
`b`, ...) because an interface records types, not names. `"gen":"abi"` exports are
counted, not listed: their types are ABI placeholders.

<!-- BEGIN GENERATED by scripts/check_stdlib_signatures.py --write from stdlib/*.tki (story 127.456). Do not edit by hand. -->

### std.analytics (generated)

```text
t=$groupstat
t=$statsrow
t=$tspoint
f=anomalies(a:$dataframe;b:$str;c:f64):@(u64)!$dferr
f=corr(a:$dataframe;b:$str;c:$str):f64!$dferr
f=describe(a:$dataframe):@($statsrow)!$dferr
f=groupstats(a:$dataframe;b:$str;c:$str):@($groupstat)!$dferr
f=pivot(a:$dataframe;b:$str;c:$str;d:$str):$dataframe!$dferr
f=timeseries(a:$dataframe;b:$str;c:$str;d:u64):@($tspoint)!$dferr
```

### std.args (generated)

```text
t=$argserr
f=all():@($str)
f=count():u64
f=get(a:u64):$str!$argserr
```

### std.array (generated)

Not listed: 11 `"gen":"abi"` exports of `stdlib/array.tki`, whose types are ABI placeholders (only the arity is declared).

### std.auth (generated)

```text
t=$autherr
t=$jwtclaims
f=apikeygenerate():$str
f=apikeyvalidate(a:$str;b:$str):bool
f=bearerextract(a:$str):$str!$autherr
f=jwtexpired(a:$str):bool
f=jwtsign(a:$jwtclaims;b:@(byte)):$str!$autherr
f=jwtverify(a:$str;b:@(byte)):$jwtclaims!$autherr
f=passwordhash(a:$str):$str!$autherr
f=passwordverify(a:$str;b:$str):bool
```

### std.canvas (generated)

```text
t=$canvas
t=$canvasop
f=arc(a:$canvas;b:f64;c:f64;d:f64;e:f64;f:f64):$canvas
f=beginpath(a:$canvas):$canvas
f=clearrect(a:$canvas;b:f64;c:f64;d:f64;e:f64):$canvas
f=closepath(a:$canvas):$canvas
f=drawimage(a:$canvas;b:$str;c:f64;d:f64;e:f64;f:f64):$canvas
f=fill(a:$canvas;b:$str):$canvas
f=fillrect(a:$canvas;b:f64;c:f64;d:f64;e:f64;f:$str):$canvas
f=filltext(a:$canvas;b:$str;c:f64;d:f64;e:$str;f:$str):$canvas
f=lineto(a:$canvas;b:f64;c:f64):$canvas
f=moveto(a:$canvas;b:f64;c:f64):$canvas
f=new(a:$str;b:u32;c:u32):$canvas
f=setalpha(a:$canvas;b:f64):$canvas
f=stroke(a:$canvas;b:$str;c:f64):$canvas
f=strokerect(a:$canvas;b:f64;c:f64;d:f64;e:f64;f:$str;g:f64):$canvas
f=tohtml(a:$canvas):$str
f=tojs(a:$canvas):$str
```

### std.chart (generated)

```text
t=$charterr
t=$chartspec
t=$dataset
f=bar(a:@($str);b:@(f64);c:$str):$chartspec
f=line(a:@($str);b:@($dataset);c:$str):$chartspec
f=pie(a:@($str);b:@(f64);c:$str):$chartspec
f=scatter(a:@(f64);b:@(f64);c:$str):$chartspec
f=tojson(a:$chartspec):@(byte)
f=tovega(a:$chartspec):@(byte)
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/chart.tki`, whose types are ABI placeholders (only the arity is declared).

### std.clipboard (generated)

Not listed: 2 `"gen":"abi"` exports of `stdlib/clipboard.tki`, whose types are ABI placeholders (only the arity is declared).

### std.collections (generated)

Not listed: 3 `"gen":"abi"` exports of `stdlib/collections.tki`, whose types are ABI placeholders (only the arity is declared).

### std.crypto: exports not listed above (generated)

```text
f=constanttimeequal(a:@(byte);b:@(byte)):bool
f=sha256file(a:$str):$str
f=sha256verify(a:$str;b:$str):bool
```

Not listed: 2 further `"gen":"abi"` exports of `stdlib/crypto.tki`, whose types are ABI placeholders (only the arity is declared).

### std.csv (generated)

```text
t=$csvdialect
t=$csverr
t=$csvopts
t=$csvreader
t=$csvrow
t=$csvwriter
f=atend(a:$csvreader):bool
f=close(a:$csvreader):void
f=dialect(a:$csvreader):$csvdialect
f=flush(a:$csvwriter):@(byte)
f=header(a:$csvreader):@($str)!$csverr
f=next(a:$csvreader):$csvrow!$csverr
f=opts(a:$str;b:$str;c:$str;d:$str;e:$str;f:bool):$csvopts
f=parse(a:@(byte)):@($csvrow)!$csverr
f=raggedreport(a:$csvreader):@($str)
f=reader(a:@(byte);b:u8):$csvreader
f=readeropts(a:@(byte);b:$csvopts):$csvreader!$csverr
f=serialize(a:@(@($str))):$str
f=sniff(a:@(byte)):$csvdialect!$csverr
f=writer(a:u8):$csvwriter
f=writerow(a:$csvwriter;b:@($str)):void
```

### std.dashboard (generated)

```text
t=$dashboard
t=$dasherr
t=$dashwidget
f=addchart(a:$dashboard;b:$str;c:$chartspec):$dashwidget
f=addtable(a:$dashboard;b:$str;c:@($str)):$dashwidget
f=new(a:$str;b:u64):$dashboard
f=serve(a:$dashboard;b:i64):void!$dasherr
f=update(a:$dashboard;b:$dashwidget;c:@(byte)):void
```

### std.dataframe (generated)

```text
t=$dataframe
t=$dferr
t=$dfshape
t=$series
f=column(a:$dataframe;b:$str):@(f64)!$dferr
f=columnstr(a:$dataframe;b:$str):@($str)!$dferr
f=filter(a:$dataframe;b:$str;c:$str;d:$str):$dataframe!$dferr
f=fromcsv(a:$str):$dataframe!$dferr
f=fromrows(a:@($str);b:@($csvrow)):$dataframe
f=groupby(a:$dataframe;b:$str):@($dataframe)!$dferr
f=head(a:$dataframe;b:u64):$dataframe
f=join(a:$dataframe;b:$dataframe;c:$str):$dataframe!$dferr
f=schema(a:$dataframe):@($series)
f=shape(a:$dataframe):$dfshape
f=tocsv(a:$dataframe):$str
f=tojson(a:$dataframe):@(byte)!$dferr
```

### std.db: exports not listed above (generated)

```text
t=$dberr
t=$dbrow
f=bool(a:$dbrow;b:$str):bool!$dberr
f=f64(a:$dbrow;b:$str):f64!$dberr
f=i64(a:$dbrow;b:$str):i64!$dberr
f=str(a:$dbrow;b:$str):$str!$dberr
f=u64(a:$dbrow;b:$str):u64!$dberr
```

Not listed: 17 further `"gen":"abi"` exports of `stdlib/db.tki`, whose types are ABI placeholders (only the arity is declared).

### std.encoding (generated)

```text
t=$encodingerr
f=b64decode(a:$str):@(byte)!$encodingerr
f=b64encode(a:@(byte)):$str
f=b64urldecode(a:$str):@(byte)!$encodingerr
f=b64urlencode(a:@(byte)):$str
f=base64encode(a:@(byte)):$str
f=base64urlencodenopad(a:@(byte)):$str
f=frombytes(a:@(byte)):$str
f=hexdecode(a:$str):@(byte)!$encodingerr
f=hexencode(a:@(byte)):$str
f=urldecode(a:$str):$str!$encodingerr
f=urlencode(a:$str):$str
```

Not listed: 6 further `"gen":"abi"` exports of `stdlib/encoding.tki`, whose types are ABI placeholders (only the arity is declared).

### std.encrypt (generated)

```text
t=$decryptresult
t=$keypair
f=aes256gcmdecrypt(a:@(byte);b:@(byte);c:@(byte);d:@(byte)):$decryptresult
f=aes256gcmencrypt(a:@(byte);b:@(byte);c:@(byte);d:@(byte)):@(byte)
f=aes256gcmkeygen():@(byte)
f=aes256gcmnoncegen():@(byte)
f=ed25519keypair():$keypair
f=ed25519sign(a:@(byte);b:@(byte)):@(byte)
f=ed25519verify(a:@(byte);b:@(byte);c:@(byte)):bool
f=hkdfsha256(a:@(byte);b:@(byte);c:@(byte);d:u64):@(byte)
f=tlscertfingerprint(a:$str):@(byte)
f=x25519dh(a:@(byte);b:@(byte)):@(byte)
f=x25519keypair():$keypair
```

### std.env: exports not listed above (generated)

```text
t=$enverr
f=getint(a:$str;b:i64):i64
f=getor(a:$str;b:$str):$str
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/env.tki`, whose types are ABI placeholders (only the arity is declared).

### std.file: exports not listed above (generated)

```text
t=$fileerr
f=copy(a:$str;b:$str):bool!$fileerr
f=exists(a:$str):bool
f=isdir(a:$str):bool
f=listall(a:$str):@($str)!$fileerr
f=mkdir(a:$str):bool!$fileerr
```

Not listed: 12 further `"gen":"abi"` exports of `stdlib/file.tki`, whose types are ABI placeholders (only the arity is declared).

### std.html (generated)

```text
t=$htmldoc
t=$htmlnode
f=append(a:$htmldoc;b:$htmlnode):void
f=div(a:@(@($str));b:@($htmlnode)):$htmlnode
f=doc():$htmldoc
f=escape(a:$str):$str
f=h1(a:$str):$htmlnode
f=h2(a:$str):$htmlnode
f=p(a:$str):$htmlnode
f=render(a:$htmldoc):$str
f=script(a:$htmldoc;b:$str):void
f=style(a:$htmldoc;b:$str):void
f=table(a:@($str);b:@(@($str))):$htmlnode
f=title(a:$htmldoc;b:$str):void
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/html.tki`, whose types are ABI placeholders (only the arity is declared).

### std.http: exports not listed above (generated)

```text
t=$httprouteerr
t=$req
t=$res
f=downloadfile(a:$httpclient;b:$str;c:$str):i64
f=getstatic(a:$str;b:$str):i64
f=getstaticmime(a:$str;b:$str;c:$str):i64
f=postecho(a:$str):i64
f=postjson(a:$str;b:$str):i64
f=reqbody(a:i64):$str
f=reqheader(a:i64;b:$str):$str
f=reqmethod(a:i64):$str
f=reqparam(a:i64;b:$str):$str
f=reqpath(a:i64):$str
f=resbad(a:$str):i64
f=reserr(a:$str):i64
f=resjson(a:i64;b:$str):i64
f=resok(a:$str):i64
f=servedir(a:$str;b:$str):i64
f=servepages(a:$str;b:$str):i64
f=setcors(a:$str):i64
f=setnotfound(a:$str):i64
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/http.tki`, whose types are ABI placeholders (only the arity is declared).

### std.i18n: exports not listed above (generated)

```text
t=$i18nbundle
t=$i18nerr
```

Not listed: 8 further `"gen":"abi"` exports of `stdlib/i18n.tki`, whose types are ABI placeholders (only the arity is declared).

### std.image (generated)

```text
t=$imgbuf
t=$imgfmt
f=adaptivethreshold(a:$imgbuf;b:u32;c:f64):$imgbuf!$str
f=blur(a:$imgbuf;b:u32):$imgbuf
f=brightness(a:$imgbuf;b:f64):$imgbuf
f=contrast(a:$imgbuf;b:f64):$imgbuf
f=convolve(a:$imgbuf;b:@(f64);c:u32;d:f64;e:f64):$imgbuf!$str
f=crop(a:$imgbuf;b:u32;c:u32;d:u32;e:u32):$imgbuf!$str
f=decode(a:@(byte)):$imgbuf!$str
f=encode(a:$imgbuf;b:$imgfmt;c:u8):@(byte)!$str
f=fliph(a:$imgbuf):$imgbuf
f=flipv(a:$imgbuf):$imgbuf
f=fromraw(a:@(byte);b:u32;c:u32;d:u8):$imgbuf
f=histogram(a:$imgbuf):@(i64)
f=paste(a:$imgbuf;b:$imgbuf;c:i64;d:i64):$imgbuf
f=pixelat(a:$imgbuf;b:u32;c:u32):@(byte)!$str
f=quantize(a:$imgbuf;b:u32):$imgbuf
f=resize(a:$imgbuf;b:u32;c:u32):$imgbuf
f=rotate(a:$imgbuf;b:f64):$imgbuf
f=sharpen(a:$imgbuf):$imgbuf
f=textdraw(a:$imgbuf;b:$str;c:i64;d:i64;e:u32;f:u32):$imgbuf
f=tiffdecode(a:@(byte);b:u32):$imgbuf!$str
f=tiffpages(a:@(byte)):u32!$str
f=tograyscale(a:$imgbuf):$imgbuf
```

### std.infer (generated)

```text
t=$infererr
t=$inferopts
t=$modelhandle
t=$streamopts
f=embed(a:$modelhandle;b:$str):@(f32)!infererr
f=generate(a:$modelhandle;b:$str;c:i32):$str!$infererr
f=isloaded(a:$modelhandle):bool
f=load(a:$str;b:$inferopts):$modelhandle!$infererr
f=loadstreaming(a:$str;b:$streamopts):$modelhandle!$infererr
f=unload(a:$modelhandle):bool
```

### std.json: exports not listed above (generated)

Not listed: 41 further `"gen":"abi"` exports of `stdlib/json.tki`, whose types are ABI placeholders (only the arity is declared).

### std.llm (generated)

```text
t=$llmclient
t=$llmerr
t=$llmmsg
t=$llmresp
t=$llmstream
f=chat(a:$llmclient;b:@($llmmsg)):$llmresp!$llmerr
f=chatstream(a:$llmclient;b:@($llmmsg)):$llmstream!$llmerr
f=client(a:$str;b:$str;c:$str):$llmclient
f=complete(a:$llmclient;b:$str):$str!$llmerr
f=countokens(a:$llmclient;b:$str):u64
f=streamnext(a:$llmstream):$str!$llmerr
```

### std.llmtool (generated)

```text
t=$toolcall
t=$tooldecl
t=$toolparam
t=$toolresult
f=chatwithtools(a:$llm.llmclient;b:@($llm.llmmsg)):$toolcall!$llm.llmerr
f=parsetoolcalls(a:$str):$toolcall!$llm.llmerr
f=resultmsgs(a:@($toolresult)):@($llm.llmmsg)
f=submitresult(a:$llm.llmclient;b:@($llm.llmmsg);c:$toolresult):$llm.llmresp!$llm.llmerr
f=withtools(a:$llm.llmclient;b:@($tooldecl)):$llm.llmclient
```

### std.log: exports not listed above (generated)

```text
f=accessformat(a:$str):i64
f=openerror(a:$str;b:i64;c:i64;d:i64):i64
```

Not listed: 3 further `"gen":"abi"` exports of `stdlib/log.tki`, whose types are ABI placeholders (only the arity is declared).

### std.math (generated)

```text
t=$linregresult
f=abs(a:f64):f64
f=acos(a:f64):f64
f=asin(a:f64):f64
f=atan(a:f64):f64
f=atan2(a:f64;b:f64):f64
f=ceil(a:f64):f64
f=cos(a:f64):f64
f=erf(a:f64):f64
f=exp(a:f64):f64
f=fabs(a:f64):f64
f=floor(a:f64):f64
f=fmod(a:f64;b:f64):f64
f=frombits(a:i64):f64
f=hypot(a:f64;b:f64):f64
f=linreg(a:@(f64);b:@(f64)):$linregresult
f=ln(a:f64):f64
f=log(a:f64):f64
f=log10(a:f64):f64
f=log2(a:f64):f64
f=max(a:@(f64)):f64
f=mean(a:@(f64)):f64
f=median(a:@(f64)):f64
f=min(a:@(f64)):f64
f=percentile(a:@(f64);b:f64):f64
f=pow(a:f64;b:f64):f64
f=round(a:f64):f64
f=sin(a:f64):f64
f=sqrt(a:f64):f64
f=stddev(a:@(f64)):f64
f=sum(a:@(f64)):f64
f=tan(a:f64):f64
f=tobits(a:f64):i64
f=variance(a:@(f64)):f64
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/math.tki`, whose types are ABI placeholders (only the arity is declared).

### std.md (generated)

```text
t=$mderr
f=render(a:$str):$str
f=renderfile(a:$str):$str!$mderr
```

### std.mdns (generated)

```text
t=$discovered
t=$mdnserr
t=$servicerecord
f=advertise(a:$servicerecord):bool
f=browse(a:$str;b:fn($discovered):void):bool
f=isavailable():bool
f=resolve(a:$str;b:$str):?($discovered)
f=stopadvertise(a:$str):bool
f=stopbrowse(a:$str):bool
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/mdns.tki`, whose types are ABI placeholders (only the arity is declared).

### std.mem (generated)

```text
f=alloc(a:u64):u64
f=cmp(a:u64;b:u64;c:u64):i64
f=copy(a:u64;b:u64;c:u64):void
f=free(a:u64):void
f=load8(a:u64;b:u64):u64
f=realloc(a:u64;b:u64):u64
f=set(a:u64;b:u64;c:u64):void
f=store8(a:u64;b:u64;c:u64):void
```

### std.ml (generated)

```text
t=$centroid
t=$dtreemodel
t=$kmeansmodel
t=$linearmodel
t=$mlerr
t=$row
f=accuracy(a:@(u64);b:@(u64)):f64
f=dtreefit(a:@($row);b:@($str);c:u64):$dtreemodel!$mlerr
f=dtreepredict(a:$dtreemodel;b:@(f64)):$str
f=kmeansassign(a:$kmeansmodel;b:@(f64)):u64
f=kmeanstrain(a:@($row);b:u64;c:u64):$kmeansmodel!$mlerr
f=knnpredict(a:@($row);b:@($str);c:@(f64);d:u64):$str
f=linregfit(a:@($row);b:@(f64)):$linearmodel!$mlerr
f=linregpredict(a:$linearmodel;b:@(f64)):f64
```

### std.mlx (generated)

```text
t=$mlxerr
t=$mlxmodel
f=embed(a:$mlxmodel;b:$str):@(f32)!mlxerr
f=generate(a:$mlxmodel;b:$str;c:i32):$str!$mlxerr
f=isavailable():bool
f=load(a:$str):$mlxmodel!$mlxerr
f=unload(a:$mlxmodel):bool
```

### std.net: exports not listed above (generated)

```text
f=listen(a:$str):i64
f=portavailable(a:$int):$int
```

Not listed: 4 further `"gen":"abi"` exports of `stdlib/net.tki`, whose types are ABI placeholders (only the arity is declared).

### std.ocr (generated)

```text
t=$ocrerr
t=$ocrrun
f=engine():$str
f=isavailable():bool
f=languages():$str
f=runs(a:@(byte)):@(ocrrun)!ocrerr
f=runsfile(a:$str):@(ocrrun)!ocrerr
f=runsraw(a:@(byte);b:i64;c:i64;d:i64):@(ocrrun)!ocrerr
f=setfast(a:bool):void
f=setlanguages(a:$str):bool
f=text(a:@(byte)):$str!$ocrerr
f=textfile(a:$str):$str!$ocrerr
```

### std.option (generated)

```text
t=$none
```

### std.path (generated)

```text
f=base(a:$str):$str
f=dir(a:$str):$str
f=ext(a:$str):$str
f=isabs(a:$str):bool
f=join(a:$str;b:$str):$str
f=normalize(a:$str):$str
f=stem(a:$str):$str
```

### std.pdf (generated)

```text
t=$pdfdoc
t=$pdferr
t=$textrun
f=close(a:$pdfdoc):void
f=hastext(a:$pdfdoc;b:i64):bool
f=open(a:@(byte)):$pdfdoc!$pdferr
f=openfile(a:$str):$pdfdoc!$pdferr
f=pagecount(a:$pdfdoc):i64
f=pageheight(a:$pdfdoc;b:i64):f64
f=pagetext(a:$pdfdoc;b:i64):$str!$pdferr
f=pagewidth(a:$pdfdoc;b:i64):f64
f=runs(a:$pdfdoc;b:i64):@(textrun)!pdferr
```

### std.process: exports not listed above (generated)

```text
t=$handle
t=$processerr
f=stderr(a:$handle):$str!$processerr
f=stdout(a:$handle):$str!$processerr
```

Not listed: 12 further `"gen":"abi"` exports of `stdlib/process.tki`, whose types are ABI placeholders (only the arity is declared).

### std.queue (generated)

```text
f=len(a:i64):i64
f=new():i64
f=peek(a:i64):i64
f=pop(a:i64):i64
f=push(a:i64;b:i64):i64
```

### std.router (generated)

```text
t=$handler
t=$middleware
t=$router
t=$routererr
f=bad(a:$str):i64
f=css(a:$str):i64
f=delete(a:$router;b:$str;c:$handler):void
f=get(a:$router;b:$str;c:$handler):void
f=html(a:$str):i64
f=json(a:$str):i64
f=new():$router
f=notfound(a:$str):i64
f=ok(a:$str):i64
f=param(a:i64;b:$str):$str
f=post(a:$router;b:$str;c:$handler):void
f=put(a:$router;b:$str;c:$handler):void
f=reqbody(a:i64):$str
f=reqmethod(a:i64):$str
f=reqpath(a:i64):$str
f=reqquery(a:i64):$str
f=respond(a:u64;b:$str;c:$str):i64
f=serve(a:$router;b:$str;c:u64):void!$routererr
f=status(a:u64;b:$str):i64
f=text(a:$str):i64
f=use(a:$router;b:$middleware):void
```

### std.securemem (generated)

```text
t=$securebuf
f=alloc(a:i32;b:i32):$securebuf
f=isavailable():bool
f=read(a:$securebuf):?($str)
f=sweep():i32
f=wipe(a:$securebuf):bool
f=write(a:$securebuf;b:$str):bool
```

### std.set (generated)

```text
f=add(a:i64;b:i64):i64
f=has(a:i64;b:i64):i64
f=len(a:i64):i64
f=new():i64
f=remove(a:i64;b:i64):i64
```

### std.stack (generated)

```text
f=empty(a:i64):i64
f=len(a:i64):i64
f=new():i64
f=peek(a:i64):i64
f=pop(a:i64):i64
f=push(a:i64;b:i64):i64
```

### std.std (generated)

```text
f=homedir():$str
```

### std.str: exports not listed above (generated)

```text
t=$bracketerr
t=$encodingerr
t=$parseerr
t=$sliceerr
t=$strbuf
f=add(a:$strbuf;b:$str):void
f=addbyte(a:$strbuf;b:byte):void
f=buf():$strbuf
f=build(a:$strbuf):$str
f=builder():$strbuf
f=bytes(a:$str):@(byte)
f=charat(a:$str;b:i64):i64
f=charcode(a:$str;b:i64):i64
f=compare(a:$str;b:$str):i64
f=done(a:$strbuf):$str
f=endswith(a:$str;b:$str):bool
f=eq(a:$str;b:$str):bool
f=fields(a:$str):@($str)
f=frombytes(a:@(byte)):$str!$encodingerr
f=fromfloat(a:f64):$str
f=fromint(a:i64):$str
f=ge(a:$str;b:$str):bool
f=gt(a:$str;b:$str):bool
f=indexfrom(a:$str;b:$str;c:i64):i64
f=indexof(a:$str;b:$str):i64
f=interpolate(a:@($str)):$str
f=isdigit(a:$str):bool
f=join(a:$str;b:@($str)):$str
f=lastindex(a:$str;b:$str):i64
f=le(a:$str;b:$str):bool
f=lt(a:$str;b:$str):bool
f=matchbracket(a:$str):$str!$bracketerr
f=nowunix():$str
f=padleft(a:$str;b:i64;c:i64):$str
f=padright(a:$str;b:i64;c:i64):$str
f=parsefloat(a:$str):f64!$parseerr
f=repeat(a:$str;b:u64):$str
f=startswith(a:$str;b:$str):bool
f=tof32(a:$str):f64!$parseerr
f=tof64(a:$str):f64!$parseerr
f=tofloat(a:$str):f64!$parseerr
f=toint(a:$str):i64!$parseerr
f=trimprefix(a:$str;b:$str):$str
f=trimsuffix(a:$str;b:$str):$str
```

Not listed: 48 further `"gen":"abi"` exports of `stdlib/str.tki`, whose types are ABI placeholders (only the arity is declared).

### std.svg (generated)

```text
t=$svgdoc
t=$svgelem
t=$svgstyle
f=append(a:$svgdoc;b:$svgelem):$svgdoc
f=arrow(a:f64;b:f64;c:f64;d:f64;e:$svgstyle):$svgelem
f=circle(a:f64;b:f64;c:f64;d:$svgstyle):$svgelem
f=doc(a:f64;b:f64):$svgdoc
f=group(a:@($svgelem);b:$str):$svgelem
f=line(a:f64;b:f64;c:f64;d:f64;e:$svgstyle):$svgelem
f=path(a:$str;b:$svgstyle):$svgelem
f=polygon(a:@(@(f64));b:$svgstyle):$svgelem
f=polyline(a:@(@(f64));b:$svgstyle):$svgelem
f=rect(a:f64;b:f64;c:f64;d:f64;e:$svgstyle):$svgelem
f=render(a:$svgdoc):$str
f=style(a:$str;b:$str;c:f64):$svgstyle
f=text(a:f64;b:f64;c:$str;d:$svgstyle):$svgelem
```

### std.sys (generated)

```text
f=configdir(a:$str):$str
f=datadir(a:$str):$str
```

Not listed: 6 further `"gen":"abi"` exports of `stdlib/sys.tki`, whose types are ABI placeholders (only the arity is declared).

### std.task (generated)

```text
f=awaitall(a:i64):i64
f=cancel(a:i64):i64
f=result(a:i64):i64
f=scope():i64
f=spawn(a:i64;b:i64):i64
```

### std.template (generated)

```text
t=$tmpl
t=$tmplerr
t=$tmplvars
f=compile(a:$str):$tmpl!$tmplerr
f=escape(a:$str):$str
f=html(a:$str;b:@($str:$str);c:@$str):$str
f=render(a:$tmpl;b:$tmplvars):$str!$tmplerr
f=renderfile(a:$str;b:$tmplvars):$str!$tmplerr
f=vars(a:@($str:$str)):$tmplvars
```

Not listed: 1 further `"gen":"abi"` export of `stdlib/template.tki`, whose types are ABI placeholders (only the arity is declared).

### std.test: exports not listed above (generated)

```text
f=assert(a:bool;b:$str):bool
f=asserteq(a:$str;b:$str;c:$str):bool
f=assertne(a:$str;b:$str;c:$str):bool
f=eqstr(a:$str;b:$str):bool
```

Not listed: 3 further `"gen":"abi"` exports of `stdlib/test.tki`, whose types are ABI placeholders (only the arity is declared).

### std.time: exports not listed above (generated)

```text
t=$timeduration
t=$timeerr
t=$timeparts
f=add(a:u64;b:i64):u64
f=adddays(a:u64;b:i64):u64
f=addmonths(a:u64;b:i64):u64
f=addyears(a:u64;b:i64):u64
f=convert(a:u64;b:$str;c:$str):$str
f=daysinmonth(a:i64;b:i64):u64
f=diff(a:u64;b:u64):i64
f=duration(a:u64;b:u64):$timeduration
f=formatduration(a:$timeduration):$str
f=formatmars(a:f64;b:$str):$str
f=fromparts(a:$timeparts):u64
f=isleapyear(a:i64):bool
f=juliandate(a:u64):f64
f=lightdelay(a:$str;b:$str;c:u64):f64
f=marssol(a:u64):f64
f=parse(a:$str;b:$str):u64!$timeerr
f=parseduration(a:$str):$timeduration!$timeerr
f=startofday(a:u64):u64
f=startofmonth(a:u64):u64
f=startofyear(a:u64):u64
f=toparts(a:u64):$timeparts
f=utcoffset(a:$str):i64
f=weekday(a:u64):u64
f=withtz(a:u64;b:$str):$str
```

Not listed: 8 further `"gen":"abi"` exports of `stdlib/time.tki`, whose types are ABI placeholders (only the arity is declared).

### std.toon: exports not listed above (generated)

```text
t=$toonerr
t=$toonval
f=empty():$toonval
```

Not listed: 9 further `"gen":"abi"` exports of `stdlib/toon.tki`, whose types are ABI placeholders (only the arity is declared).

### std.vec (generated)

```text
t=$vechandle
f=get(a:$vechandle;b:i64):i64
f=len(a:$vechandle):i64
f=new():$vechandle
f=pop(a:$vechandle):i64
f=push(a:$vechandle;b:i64):$vechandle
f=set(a:$vechandle;b:i64;c:i64):$vechandle
f=toarray(a:$vechandle):@$i64
f=tovec(a:@$i64):$vechandle
```

### std.vecstore (generated)

```text
t=$searchresult
t=$veccollection
t=$vecentry
t=$vecerr
t=$vecstorehandle
f=close(a:$vecstorehandle):void
f=collection(a:$vecstorehandle;b:$str):$veccollection!$vecerr
f=count(a:$veccollection):i32
f=delete(a:$veccollection;b:$str):bool
f=deletebefore(a:$veccollection;b:i64):i32
f=flush(a:$vecstorehandle):bool
f=open(a:$str):$vecstorehandle!$vecerr
f=search(a:$veccollection;b:@(f64);c:i32;d:f64):@($searchresult)
f=upsert(a:$veccollection;b:$str;c:@(f64);d:$str):bool
```

### std.xlsx (generated)

```text
t=$xlsxbook
t=$xlsxcell
t=$xlsxerr
f=close(a:$xlsxbook):void
f=datefromserial(a:$str;b:$str):$str
f=datesystem(a:$xlsxbook):$str
f=open(a:@(byte)):$xlsxbook!$xlsxerr
f=openfile(a:$str):$xlsxbook!$xlsxerr
f=rows(a:$xlsxbook;b:$str):@(@(xlsxcell))!xlsxerr
f=sheets(a:$xlsxbook):@($str)
```

### std.xml: exports not listed above (generated)

Not listed: 1 further `"gen":"abi"` export of `stdlib/xml.tki`, whose types are ABI placeholders (only the arity is declared).

### std.yaml: exports not listed above (generated)

```text
t=$yamlerr
t=$yamlval
f=empty():$yamlval
```

Not listed: 9 further `"gen":"abi"` exports of `stdlib/yaml.tki`, whose types are ABI placeholders (only the arity is declared).

<!-- END GENERATED -->
