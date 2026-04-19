# std.ws -- WebSocket Client

## Overview

The `std.ws` module provides WebSocket client and messaging functions. It supports text and binary frames, connection lifecycle management, and one-to-many broadcast. All network operations that can fail return error unions with `wserr`.

## Types

### wsconn

An open WebSocket connection handle.

| Field | Type | Description |
|-------|------|-------------|
| `id` | `u64` | Unique connection identifier assigned at connect time. |
| `ready` | `bool` | `true` while the connection is open and usable. |

### wsmsg

A single received WebSocket message.

| Field | Type | Description |
|-------|------|-------------|
| `payload` | `[byte]` | The raw message bytes. For text frames this is UTF-8. |
| `fin` | `bool` | `true` when this frame completes the message (no continuation). |
| `opcode` | `u64` | WebSocket opcode: 1 = text, 2 = binary, 8 = close, 9 = ping, 10 = pong. |

### wserr

Error type returned by fallible WebSocket operations.

| Field | Type | Description |
|-------|------|-------------|
| `msg` | `str` | Human-readable description of the failure. |

## Functions

### ws.connect(url: str): wsconn!wserr

Opens a WebSocket connection to `url`. The URL must use the `ws://` or `wss://` scheme. Returns a `wsconn` handle on success or `wserr` if the handshake fails or the host is unreachable.

**Example:**
```toke
let conn = ws.connect("ws://localhost:9000/chat");
match conn {
  ok(c)  -> (* use c *)
  err(e) -> io.println(e.msg);
};
```

### ws.send(conn: wsconn; text: str): void!wserr

Sends a UTF-8 text frame over `conn`. Returns `wserr` if the connection is no longer ready or the write fails.

**Example:**
```toke
let conn = ws.connect("ws://localhost:9000") |> unwrap;
ws.send(conn; "hello server");
```

### ws.sendbytes(conn: wsconn; data: [byte]): void!wserr

Sends a binary frame containing `data` over `conn`. Use this for non-text payloads such as images, serialised structs, or protocol buffers. Returns `wserr` on write failure.

**Example:**
```toke
let conn = ws.connect("ws://localhost:9000") |> unwrap;
let img = fs.read_bytes("photo.png") |> unwrap;
ws.sendbytes(conn; img);
```

### ws.recv(conn: wsconn): wsmsg!wserr

Blocks until the next complete message arrives on `conn`. Returns a `wsmsg` whose `opcode` indicates the frame type. Returns `wserr` if the connection drops before a message is received.

**Example:**
```toke
let conn = ws.connect("ws://localhost:9000") |> unwrap;
let msg = ws.recv(conn) |> unwrap;
match msg.opcode {
  1 -> io.println(str.from_bytes(msg.payload));
  2 -> io.println("binary frame received");
  _ -> io.println("control frame");
};
```

### ws.close(conn: wsconn): void

Sends a close frame and releases the connection handle. After `ws.close`, the `conn.ready` field is `false` and further sends or receives will fail. This function does not return an error; closing an already-closed connection is a no-op.

**Example:**
```toke
let conn = ws.connect("ws://localhost:9000") |> unwrap;
ws.send(conn; "goodbye");
ws.close(conn);
```

### ws.broadcast(conns: [wsconn]; text: str): void

Sends the same text frame to every connection in `conns`. Connections that are no longer ready are silently skipped. This function does not return an error.

**Example:**
```toke
let clients: [wsconn] = get_active_clients();
ws.broadcast(clients; "server shutting down in 30s");
```

## Error handling

All fallible functions return `wserr` through an error union. The idiomatic approach is `|> unwrap` for quick scripts or `match` for production code that must react to failures.

```toke
let result = ws.connect("ws://bad-host:9999");
match result {
  ok(c)  -> ws.send(c; "hi");
  err(e) -> io.println(str.concat("connect failed: "; e.msg));
};
```

## Patterns

### Client: connect, send, receive, close

The basic lifecycle for a WebSocket client. Connect to a server, exchange messages, then close cleanly.

```toke
let conn = ws.connect("ws://localhost:9000/echo") |> unwrap;

(* send a text message *)
ws.send(conn; "ping");

(* wait for the reply *)
let reply = ws.recv(conn) |> unwrap;
io.println(str.from_bytes(reply.payload));  (* "ping" echoed back *)

(* tear down *)
ws.close(conn);
```

### Echo server loop

A minimal echo server that reads one message at a time and sends it back. In practice the accept loop would be provided by a listener; this example shows the per-connection handler.

```toke
(* handle_client is called once per accepted connection *)
let handle_client = fn(conn: wsconn): void {
  loop {
    let result = ws.recv(conn);
    match result {
      err(_) -> {
        ws.close(conn);
        return;
      }
      ok(msg) -> {
        match msg.opcode {
          1 -> ws.send(conn; str.from_bytes(msg.payload));
          8 -> {
            ws.close(conn);
            return;
          }
          _ -> (* ignore control frames *)
        };
      }
    };
  };
};
```

### Broadcast to all connected clients

Fan-out a message to every connection in a client list, for example a chat room announcement.

```toke
let clients: [wsconn] = @(
  ws.connect("ws://localhost:9000") |> unwrap;
  ws.connect("ws://localhost:9001") |> unwrap;
);

ws.broadcast(clients; "system: maintenance window begins now");

(* clean up *)
for c in clients {
  ws.close(c);
};
```
