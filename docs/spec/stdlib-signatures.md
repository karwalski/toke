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

toke uses a **TOON-first serialization strategy**: TOON for tabular data, YAML and JSON as secondary formats. The TOON project reports 30-60% fewer tokens than equivalent JSON for uniform arrays (their published benchmark; not reproduced here). String externalisation for internationalisation via `std.i18n`. See [ADR-0003](../docs/architecture/ADR-0003.md).

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

### std.json

```text
f=enc(val:i64):$str
f=dec(s:$str):i64
f=str(s:$str;key:$str):$str
f=i64(s:$str;key:$str):i64
f=f64(s:$str;key:$str):f64
f=bool(s:$str;key:$str):bool
f=arr(s:$str;key:$str):@$str
f=parse(s:$str):i64
f=print(val:i64):void

(* Streaming API — 12.1.1 *)
t=$jsonstream{buf:@$byte;pos:u64;depth:u64;state:u64}
t=$jsontoken{$objectstart:void;$objectend:void;$arraystart:void;$arrayend:void;$key:$str;$str:$str;$u64:u64;$i64:i64;$f64:f64;$bool:bool;$null:void;$end:void}
t=$jsonstreamerr{$truncated:$str;$invalid:$str;$overflow:$str}
t=$writer{buf:@$byte;pos:u64}
f=json.streamparser(input:@$byte):$jsonstream
f=json.streamnext(parser:$jsonstream):$jsontoken!$jsonstreamerr
f=json.streamemit(writer:$writer;val:$json):void!$jsonstreamerr
f=json.newwriter(capacity:u64):$writer
f=json.writerbytes(writer:$writer):@$byte
```

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
f=param(req:$req;key:$str):$str!$httperr
f=header(req:$req;key:$str):$str!$httperr
f=res.ok(body:$str):$res
f=res.json(status:u16;body:$str):$res
f=res.bad(msg:$str):$res
f=res.err(msg:$str):$res
http.get(path:$str;handler:func)
http.post(path:$str;handler:func)
http.put(path:$str;handler:func)
http.delete(path:$str;handler:func)
http.patch(path:$str;handler:func)
(* client-side: types *)
t=$httpclient{baseurl:$str;poolsize:u64;timeoutms:u64}
t=$httpreq{method:$str;url:$str;headers:@($str:$str);body:@$byte}
t=$httpresp{status:u64;headers:@($str:$str);body:@$byte}
t=$httperr{msg:$str;code:u64}
t=$httpstream{id:u64;open:bool}
(* client-side: functions *)
f=http.client(baseurl:$str):$httpclient
f=http.get(c:$httpclient;path:$str):$httpresp!$httperr
f=http.post(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.put(c:$httpclient;path:$str;body:@$byte;ctype:$str):$httpresp!$httperr
f=http.delete(c:$httpclient;path:$str):$httpresp!$httperr
f=http.stream(c:$httpclient;req:$httpreq):$httpstream!$httperr
f=http.streamnext(s:$httpstream):@$byte!$httperr
```

### std.db

```text
f=open(path:$str):i64
f=exec(db:i64;sql:$str):i64
f=query(db:i64;sql:$str):$str
f=close(db:i64):void
```

### std.file

```text
f=read(path:$str):$str
f=write(path:$str;data:$str):void
f=append(path:$str;data:$str):void
f=list(dir:$str):@$str
f=delete(path:$str):void
f=readbytes(path:$str):@(byte)!$fileerr
f=writebytes(path:$str;data:@(byte)):bool!$fileerr
f=lasterr():$str
f=lasterrkind():$str
```

`read` and `write` carry a `$str`, which is NUL-terminated: they truncate at
the first zero byte and are therefore **text-only**. `readbytes`/`writebytes`
(135.10) are the byte-exact pair and are what every binary format — archives,
PDFs, images, spreadsheets — must use. An empty `@(byte)` means an empty file,
never a failure; the `$err` arm carries no payload (the compiled `T!E` ABI has
none), so `file.lasterrkind` reports which of `notfound permission isdir
notregular symlink toolarge nomem io badarg` occurred and `file.lasterr` gives
the message. `readbytes` refuses a file over 64 MiB rather than attempting it,
because a `@(byte)` costs one i64 per byte. See [std.file](/docs/stdlib/file).

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
f=zip.lasterr():$str
```

Read-only; Store and Deflate only. `$ziparchive` is an opaque handle with no readable fields. Opening validates the whole central directory first, so a single bad entry rejects the archive: absolute or `..`-bearing or backslash-bearing names, a cumulative uncompressed size over 64 MiB, a ratio over 200:1, more than 65536 entries, an archive over 128 MiB, an encrypted entry, or any other compression method. `zip.openfile` and `zip.lasterr` are additions to the requested interface — the first because toke has no binary file read (`file.read` truncates at the first NUL), the second because the compiled `T!E` ABI carries no error payload, so the rule that rejected an archive would otherwise be unobservable. See [std.zip](/docs/stdlib/zip).
