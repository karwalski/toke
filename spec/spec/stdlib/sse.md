# std.sse

Server-Sent Events: emit structured events, send raw data, manage keepalive, and close connections.

## Types

```
type ssectx   { id: u64, open: bool }
type sseevent { id: str, event: str, data: str, retry: u64 }
type sseerr   { msg: str }
```

## Functions

```
f=emit(ctx:ssectx;event:sseevent):void!sseerr
f=emitdata(ctx:ssectx;data:str):void!sseerr
f=close(ctx:ssectx):void
f=keepalive(ctx:ssectx;interval_ms:u64):void
```

## Semantics

- `ssectx` is obtained from an HTTP handler context when the client requests `text/event-stream`.
- `emit` writes a full SSE event with optional `id:`, `event:`, `data:`, and `retry:` fields. Returns `sseerr` if the connection is closed.
- `emitdata` is a convenience that writes only a `data:` field (unnamed event).
- `close` terminates the SSE stream and releases the connection.
- `keepalive` starts a background comment ping (`:keepalive`) at the given interval in milliseconds. Prevents proxy timeouts.

## Dependencies

None (operates on an HTTP response stream from `std.http`).
