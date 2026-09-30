---
title: toke Standard Library — Normative Signatures
slug: stdlib-signatures
section: spec
order: 4
---

**Status:** Complete — 17 modules with C runtime backing

This file is the normative interface for the toke standard library.
Changes here require coordinated updates to the stdlib and toke-eval/benchmark.

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
f=len(s:$str):i64
f=concat(a:$str;b:$str):$str
f=slice(s:$str;start:i64;end:i64):$str
f=split(s:$str;sep:$str):@$str
f=upper(s:$str):$str
f=lower(s:$str):$str
f=trim(s:$str):$str
f=contains(s:$str;sub:$str):bool
f=replace(s:$str;old:$str;new:$str):$str
f=starts(s:$str;prefix:$str):bool
f=ends(s:$str;suffix:$str):bool
```

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
t=$json{raw:$str}
t=$jsonerr{$parse:$str;$type:$str;$missing:$str}
f=enc(s:$str):$str
f=dec(s:$str):$json!$jsonerr
f=str(j:$json;key:$str):$str!$jsonerr
f=u64(j:$json;key:$str):u64!$jsonerr
f=i64(j:$json;key:$str):i64!$jsonerr
f=f64(j:$json;key:$str):f64!$jsonerr
f=bool(j:$json;key:$str):bool!$jsonerr
f=arr(j:$json;key:$str):@$json!$jsonerr
f=at(j:$json;path:$str):$json!$jsonerr   (* 138.12: dotted path, e.g. "usage.input_tokens", "r.0" *)
f=getval(j:$json;path:$str):$str        (* 138.12: the raw JSON text at path, "" if none *)

(* 127.241: the typed get* spellings -- aliases of i64 / bool / f64 above.
   Their parameters are still the ABI's i64 (a json handle, a str key); the
   result is the declared union, so a match decides on the error slot. *)
f=getint(j:i64;key:i64):i64!$jsonerr
f=geti64(j:i64;key:i64):i64!$jsonerr
f=geti32(j:i64;key:i64):i64!$jsonerr
f=getbool(j:i64;key:i64):bool!$jsonerr
f=getf32(j:i64;key:i64):f64!$jsonerr
f=getfloat(j:i64;key:i64):f64!$jsonerr

(* Streaming API — 12.1.1.  Declared in json.tki but quarantined: no glue
   reaches the C core yet, so a call is refused at check with E4027
   (docs/reference/tki-quarantine.md; 102.16) *)
t=$jsonstream{buf:@$byte;pos:u64;depth:u64;state:u64}
t=$jsontoken{$objectstart:void;$objectend:void;$arraystart:void;$arrayend:void;$key:$str;$str:$str;$u64:u64;$i64:i64;$f64:f64;$bool:bool;$null:void;$end:void}
t=$jsonstreamerr{$truncated:$str;$invalid:$str;$overflow:$str}
t=$jsonwriter{buf:@$byte;pos:u64}
f=json.streamparser(input:@$byte):$jsonstream
f=json.streamnext(parser:$jsonstream):$jsontoken!$jsonstreamerr
f=json.streamemit(writer:$jsonwriter;val:$json):void!$jsonstreamerr
f=json.newwriter(capacity:u64):$jsonwriter
f=json.writerbytes(writer:$jsonwriter):@$byte
```

ABI helpers. `json.tki` exports these with `"gen":"abi"`: their parameter and
return types are `i64` placeholders, so only the name and arity are
established, and they are listed that way (`name/arity`):
`arrget/2`, `arrlen/1`, `decode/1`, `encarr/3`, `encbool/1`, `encf64/1`, `encnum/1`, `encode/1`, `encstr/1`, `entries/1`, `err/1`, `get/2`, `getarr/2`, `getarray/2`, `getobj/2`, `getopt/2`, `getregulations/1`, `getstr/2`, `getstring/2`, `has/2`, `haskey/2`, `isnullkey/2`, `keys/1`, `kv/2`, `len/1`, `obj/1`, `ok/1`, `parse/1`, `parsearray/1`, `parseobj/1`, `print/1`, `printarr/3`, `quote/1`, `serialize/1`, `set/3`, `setint/3`, `strarr/1`, `strarray/1`, `stringify/1`, `tryarr/2`, `trybool/2`, `tryboolkey/2`, `tryfloat/2`, `tryfloatkey/2`, `tryint/2`, `tryintkey/2`, `trystr/2`, `trystrkey/2`, `values/1`.

### std.toon

```text
f=enc(data:$str;schema:$str):$str
f=dec(s:$str):$str
f=str(s:$str;key:$str):$str
f=i64(s:$str;key:$str):i64
f=f64(s:$str;key:$str):f64
f=bool(s:$str;key:$str):bool
f=arr(s:$str):@$str
f=fromjson(s:$str;name:$str):$str
f=tojson(s:$str):$str
```

### std.yaml

```text
f=enc(data:$str):$str
f=dec(s:$str):$str
f=str(s:$str;key:$str):$str
f=i64(s:$str;key:$str):i64
f=f64(s:$str;key:$str):f64
f=bool(s:$str;key:$str):bool
f=arr(s:$str;key:$str):@$str
f=fromjson(s:$str):$str
f=tojson(s:$str):$str
```

### std.i18n

```text
f=load(path:$str):$str
f=get(bundle:$str;key:$str):$str
f=fmt(bundle:$str;key:$str;args:$str):$str
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
(* client-side: types *)
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
f=http.clientget(c:$httpclient;path:$str):$httpresp!$httperr
f=http.clientpost(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.clientput(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.clientdelete(c:$httpclient;path:$str):$httpresp!$httperr
f=http.geturl(url:$str):$str!$httperr
f=http.postheaders(url:$str;body:$str;headers:@(@($str))):$httpresp!$httperr
f=http.stream(c:$httpclient):$httpstream!$httperr
(* 121.53 (2), 2026-09-30: the implemented and published (http.tki) form is
   the client alone -- a GET of its base URL.  The (c;req) form this file
   used to list was never implemented: the glue read a URL, and the core
   (http_stream in http.c) would send req's method, path and body but not its
   headers, so restoring it needs an http.c change first. *)
f=http.streamnext(s:$httpstream):@$byte!$httperr
```

### std.db

```text
f=open(path:$str):u64!$dberr
f=close():i64
f=exec(sql:$str;params:@($str)):u64!$dberr
f=one(sql:$str;params:@($str)):$row!$dberr
f=many(sql:$str;params:@($str)):@($row)!$dberr
f=execon(h:u64;sql:$str;params:@($str)):u64!$dberr
f=oneon(h:u64;sql:$str;params:@($str)):$row!$dberr
f=manyon(h:u64;sql:$str;params:@($str)):@($row)!$dberr
f=closeon(h:u64):bool
```

Every `open` returns a new handle; the `*on` forms name one, and the
handle-less forms use the default connection -- the lowest-numbered one still
open (138.6). `exec` runs every `;`-separated statement of its string or
fails naming the one that failed; bind parameters with more than one
statement are refused. Row values are read with `row.str/u64/i64/f64/bool`.

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
f=read(path:$str):$str
f=write(path:$str;data:$str):void
f=append(path:$str;data:$str):void
f=list(dir:$str):@$str
f=delete(path:$str):void
f=readbytes(path:$str):@(byte)!$fileerr
f=writebytes(path:$str;data:@(byte)):bool!$fileerr
f=readrange(path:$str;offset:i64;len:i64):@(byte)!$fileerr
f=size(path:$str):i64!$fileerr
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
it. See [std.file](/docs/stdlib/file).

### std.env

```text
f=get(key:$str):$str
f=set(key:$str;val:$str):void
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
f=spawn(cmd:$str):i64
f=wait(pid:i64):i64
f=kill(pid:i64):void
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
f=now():i64
f=fmt(ts:i64;layout:$str):$str
f=since(ts:i64):i64
```

### std.log

```text
f=info(msg:$str):void
f=warn(msg:$str):void
f=error(msg:$str):void
f=debug(msg:$str):void
```

### std.test

```text
f=eq(a:i64;b:i64):void
f=neq(a:i64;b:i64):void
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
