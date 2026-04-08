# toke Standard Library — Normative Signatures

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
- `std.file` — file I/O (read, write, append, list, delete)
- `std.env` — environment variable access
- `std.process` — subprocess spawning and control
- `std.crypto` — SHA-256, SHA-512, HMAC-SHA-256, HMAC-SHA-512, constant-time compare, random bytes, hex encoding
- `std.time` — time operations (now, format, since)
- `std.log` — structured logging
- `std.test` — test assertions

## Serialization Strategy

toke uses a **TOON-first serialization strategy**: TOON for tabular data (30-60% fewer tokens than JSON), YAML and JSON as secondary formats. String externalisation for internationalisation via `std.i18n`. See [ADR-0003](../docs/architecture/ADR-0003.md).

## Function Signatures

### std.str

```
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

```
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
t=$jsonstream{buf:@byte;pos:u64;depth:u64;state:u64}
t=$jsontoken{ObjectStart:void;ObjectEnd:void;ArrayStart:void;ArrayEnd:void;Key:$str;Str:$str;U64:u64;I64:i64;F64:f64;Bool:bool;Null:void;End:void}
t=$jsonstreamerr{Truncated:$str;Invalid:$str;Overflow:$str}
t=$writer{buf:@byte;pos:u64}
f=json.streamparser(input:@byte):$jsonstream
f=json.streamnext(parser:$jsonstream):$jsontoken!$jsonstreamerr
f=json.streamemit(writer:$writer;val:$json):void!$jsonstreamerr
f=json.newwriter(capacity:u64):$writer
f=json.writerbytes(writer:$writer):@byte
```

### std.toon

```
f=enc(data:$str;schema:$str):$str
f=dec(s:$str):$str
f=str(s:$str;key:$str):$str
f=i64(s:$str;key:$str):i64
f=f64(s:$str;key:$str):f64
f=bool(s:$str;key:$str):bool
f=arr(s:$str):@$str
f=from_json(s:$str;name:$str):$str
f=to_json(s:$str):$str
```

### std.yaml

```
f=enc(data:$str):$str
f=dec(s:$str):$str
f=str(s:$str;key:$str):$str
f=i64(s:$str;key:$str):i64
f=f64(s:$str;key:$str):f64
f=bool(s:$str;key:$str):bool
f=arr(s:$str;key:$str):@$str
f=from_json(s:$str):$str
f=to_json(s:$str):$str
```

### std.i18n

```
f=load(path:$str):$str
f=get(bundle:$str;key:$str):$str
f=fmt(bundle:$str;key:$str;args:$str):$str
f=locale():$str
```

### std.http

```
(* server-side routing *)
f=param(req:$Req;key:$str):$str!$HttpErr
f=header(req:$Req;key:$str):$str!$HttpErr
f=Res.ok(body:$str):$Res
f=Res.json(status:u16;body:$str):$Res
f=Res.bad(msg:$str):$Res
f=Res.err(msg:$str):$Res
http.GET(path:$str;handler:FuncDecl)
http.POST(path:$str;handler:FuncDecl)
http.PUT(path:$str;handler:FuncDecl)
http.DELETE(path:$str;handler:FuncDecl)
http.PATCH(path:$str;handler:FuncDecl)
(* client-side: types *)
t=$httpclient{baseurl:$str;pool_size:u64;timeout_ms:u64}
t=$httpreq{method:$str;url:$str;headers:@($str:$str);body:@byte}
t=$httpresp{status:u64;headers:@($str:$str);body:@byte}
t=$httperr{msg:$str;code:u64}
t=$httpstream{id:u64;open:bool}
(* client-side: functions *)
f=http.client(baseurl:$str):$httpclient
f=http.get(c:$httpclient;path:$str):$httpresp!$httperr
f=http.post(c:$httpclient;path:$str;body:@byte;ctype:$str):$httpresp!$httperr
f=http.put(c:$httpclient;path:$str;body:@byte;ctype:$str):$httpresp!$httperr
f=http.delete(c:$httpclient;path:$str):$httpresp!$httperr
f=http.stream(c:$httpclient;req:$httpreq):$httpstream!$httperr
f=http.streamnext(s:$httpstream):@byte!$httperr
```

### std.db

```
f=open(path:$str):i64
f=exec(db:i64;sql:$str):i64
f=query(db:i64;sql:$str):$str
f=close(db:i64):void
```

### std.file

```
f=read(path:$str):$str
f=write(path:$str;data:$str):void
f=append(path:$str;data:$str):void
f=list(dir:$str):@$str
f=delete(path:$str):void
```

### std.env

```
f=get(key:$str):$str
f=set(key:$str;val:$str):void
```

### std.process

```
f=exec(cmd:$str):$str
f=spawn(cmd:$str):i64
f=wait(pid:i64):i64
f=kill(pid:i64):void
```

### std.crypto

```
f=sha256(data:@byte):@byte
f=sha512(data:@byte):@byte
f=hmacsha256(key:@byte;data:@byte):@byte
f=hmacsha512(key:@byte;data:@byte):@byte
f=constanteq(a:@byte;b:@byte):bool
f=randombytes(n:u64):@byte
f=to_hex(data:@byte):$str
```

### std.time

```
f=now():i64
f=fmt(ts:i64;layout:$str):$str
f=since(ts:i64):i64
```

### std.log

```
f=info(msg:$str):void
f=warn(msg:$str):void
f=error(msg:$str):void
f=debug(msg:$str):void
```

### std.test

```
f=eq(a:i64;b:i64):void
f=neq(a:i64;b:i64):void
f=ok(cond:bool):void
f=fail(msg:$str):void
```

### std.ws

```
t=$wsconn{id:u64;ready:bool}
t=$wsmsg{payload:@byte;fin:bool;opcode:u64}
t=$wserr{msg:$str}
f=ws.connect(url:$str):$wsconn!$wserr
f=ws.send(conn:$wsconn;msg:$str):void!$wserr
f=ws.sendbytes(conn:$wsconn;data:@byte):void!$wserr
f=ws.recv(conn:$wsconn):$wsmsg!$wserr
f=ws.close(conn:$wsconn):void
f=ws.broadcast(conns:@$wsconn;msg:$str):void
```

### std.sse

```
t=$ssectx{id:u64;open:bool}
t=$sseevent{id:$str;event:$str;data:$str;retry:u64}
t=$sseerr{msg:$str}
f=sse.emit(ctx:$ssectx;event:$sseevent):void!$sseerr
f=sse.emitdata(ctx:$ssectx;data:$str):void!$sseerr
f=sse.close(ctx:$ssectx):void
f=sse.keepalive(ctx:$ssectx;interval:u64):void
```
