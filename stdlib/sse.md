# std.sse -- Server-Sent Events

## Overview

The `std.sse` module provides server-sent event (SSE) streaming over HTTP. An SSE connection is a long-lived, unidirectional channel from server to client that follows the W3C EventSource specification. The server writes structured text frames; the client reconnects automatically on disconnect.

A handler receives an `ssectx` representing the open connection. Call `sse.emit` or `sse.emitdata` to push frames, `sse.keepalive` to schedule heartbeat comments, and `sse.close` when the stream is finished. All emit functions return `void!sseerr` -- the error fires when the client has disconnected.

## Types

### ssectx

Connection context for an active SSE stream.

| Field  | Type   | Description                                      |
|--------|--------|--------------------------------------------------|
| `id`   | `u64`  | Unique connection identifier assigned by runtime  |
| `open` | `bool` | `true` while the client connection is alive       |

### sseevent

A structured event frame sent to the client.

| Field   | Type  | Description                                         |
|---------|-------|-----------------------------------------------------|
| `id`    | `str` | Optional event ID for client `Last-Event-ID` replay |
| `event` | `str` | Event type name (empty string = unnamed message)     |
| `data`  | `str` | Payload body (may contain newlines)                  |
| `retry` | `u64` | Suggested reconnect interval in milliseconds (0 = omit) |

### sseerr

Returned when a write fails, typically because the client closed the connection.

| Field | Type  | Description        |
|-------|-------|--------------------|
| `msg` | `str` | Human-readable reason |

## Functions

### sse.emit(ctx: ssectx; ev: sseevent): void!sseerr

Sends a full event frame to the client. All four `sseevent` fields are serialised into the SSE text protocol. Fields set to their zero value (`""` for strings, `0` for retry) are omitted from the wire frame.

**Example:**
```toke
let ev = sseevent{
    id    = "1";
    event = "price";
    data  = "{\"sym\":\"BTC\",\"px\":67321.50}";
    retry = 5000;
};
sse.emit(ctx; ev);  (* sends id, event, data, retry fields *)
```

### sse.emitdata(ctx: ssectx; data: str): void!sseerr

Convenience function that sends a data-only frame with no event name, no ID, and no retry directive. Equivalent to calling `sse.emit` with an `sseevent` whose only non-zero field is `data`.

**Example:**
```toke
sse.emitdata(ctx; "heartbeat ok");
sse.emitdata(ctx; "{\"count\":42}");
```

### sse.close(ctx: ssectx): void

Closes the SSE connection and releases runtime resources. After this call, `ctx.open` is `false` and further emits will return `sseerr`. Closing an already-closed context is a no-op.

**Example:**
```toke
sse.close(ctx);  (* client sees stream end *)
```

### sse.keepalive(ctx: ssectx; interval: u64): void

Schedules automatic SSE comment frames (`: keepalive\n\n`) at the given interval in milliseconds. This prevents proxies and load balancers from closing idle connections. Set `interval` to `0` to cancel a previously scheduled keepalive.

**Example:**
```toke
sse.keepalive(ctx; 15000);  (* ping every 15 seconds *)
```

## Example -- Real-Time Event Stream with std.router

A complete handler that streams stock-price updates to the browser. The route is registered with `std.router`, a keepalive is set, and events are emitted in a loop until the client disconnects.

```toke
use std.router;
use std.sse;
use std.time;
use std.str;

(* price_feed streams SSE events on GET /prices *)
fn price_feed(ctx: ssectx): void {
    sse.keepalive(ctx; 15000);

    let seq: u64 = 0;
    while ctx.open {
        seq = seq + 1;

        let payload = str.concat(
            str.concat("{\"seq\":"; str.from_int(seq));
            ",\"px\":67321.50}";
        );

        let ev = sseevent{
            id    = str.from_int(seq);
            event = "price";
            data  = payload;
            retry = 3000;
        };

        let res = sse.emit(ctx; ev);
        if res is err {
            (* client disconnected -- exit loop *)
            return;
        };

        time.sleep(1000);  (* 1 event per second *)
    };
};

(* register the SSE endpoint *)
router.get("/prices"; price_feed);
```

**Client HTML (for reference):**
```html
<script>
const src = new EventSource("/prices");
src.addEventListener("price", (e) => {
    console.log(JSON.parse(e.data));
});
</script>
```

### Wire format produced by the example

Each iteration emits a frame like:

```
id: 1
event: price
data: {"seq":1,"px":67321.50}
retry: 3000

```

The blank line terminates the frame per the SSE specification.
