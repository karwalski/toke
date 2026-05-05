---
title: std.sse
slug: sse
section: reference/stdlib
order: 32
---

**Status: Implemented** -- C runtime backing.

The `std.sse` module provides Server-Sent Events (SSE) support. Emit structured events or raw data to a connected client over a long-lived HTTP connection.

## Types

### $ssectx

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | SSE connection identifier |
| open | bool | Whether the connection is still open |

### $sseevent

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | Event ID for client-side tracking |
| event | $str | Event type name |
| data | $str | Event payload |
| retry | u64 | Reconnection interval in milliseconds |

### $sseerr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `sse.emit` | `ctx: $ssectx; event: $sseevent` | `void!$sseerr` | Emit a structured SSE event |
| `sse.emitdata` | `ctx: $ssectx; data: $str` | `void!$sseerr` | Emit a data-only event (no type or id) |
| `sse.close` | `ctx: $ssectx` | `void` | Close the SSE connection |
| `sse.keepalive` | `ctx: $ssectx; interval_ms: u64` | `void` | Send periodic keep-alive comments at the given interval |

## Usage

```toke
m=app;
i=sse:std.sse;
i=log:std.log;

f=streamhandler():void{
  let ctx=$ssectx{id:1;open:true};
  let ev=$sseevent{id:"1";event:"update";data:"{\"count\":42}";retry:3000};
  mt sse.emit(ctx;ev) {$ok:v v;$err:e log.warn("emit failed";@())};
  sse.keepalive(ctx;15000);
};
```

## Combined Example

```toke
m=app;
i=sse:std.sse;
i=str:std.str;
i=time:std.time;
i=log:std.log;

f=handlestream():void{
  let ctx=$ssectx{id:1;open:true};
  lp(let i=0;i<5;i=i+1){
    let ts=time.format(time.now();"%H:%M:%S");
    let ev=$sseevent{
      id:str.fromint(i);
      event:"tick";
      data:ts;
      retry:0
    };
    mt sse.emit(ctx;ev) {$ok:v v;$err:e log.warn("emit failed";@())};
  };

  mt sse.emitdata(ctx;"done") {$ok:v v;$err:e log.warn("final emit failed";@())};
  sse.close(ctx);
};
```

## Dependencies

None.
