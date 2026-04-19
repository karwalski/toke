# Cookbook: WebSocket Chat Server

## Overview

Build a WebSocket server that broadcasts JSON-formatted messages to all connected clients, with structured logging for every event. Combines `std.ws`, `std.json`, and `std.log` to demonstrate real-time communication in toke.

## Modules Used

- `std.ws` -- WebSocket server and connection management
- `std.json` -- JSON parsing and serialization
- `std.log` -- Structured logging with levels and fields

## Complete Program

```toke
I std.ws;
I std.json;
I std.log;
I std.str;

(* Configure the logger *)
let logger = log.new(log.Config{
    level: log.Info;
    format: log.Json;
    output: log.Stdout;
});

(* Track connected clients *)
let clients = ws.new_pool();

(* Called when a new client connects *)
fn on_connect(conn: ws.Conn) {
    ws.pool_add(clients; conn);
    let id = ws.conn_id(conn);
    log.info(logger; "client connected"; log.Fields{
        "conn_id": id;
        "pool_size": str.from_int(ws.pool_size(clients));
    });
};

(* Called when a client disconnects *)
fn on_disconnect(conn: ws.Conn) {
    let id = ws.conn_id(conn);
    ws.pool_remove(clients; conn);
    log.info(logger; "client disconnected"; log.Fields{
        "conn_id": id;
        "pool_size": str.from_int(ws.pool_size(clients));
    });

    (* Broadcast a leave notice *)
    let notice = json.new_object();
    json.set(notice; "type"; json.from_str("system"));
    json.set(notice; "text"; json.from_str(str.concat("user "; str.concat(id; " left"))));
    ws.pool_broadcast(clients; json.encode(notice));
};

(* Called when a message arrives from a client *)
fn on_message(conn: ws.Conn; raw: Str) {
    let id = ws.conn_id(conn);

    (* Parse the incoming JSON *)
    let msg = json.decode(raw) catch |err| {
        log.warn(logger; "invalid json from client"; log.Fields{
            "conn_id": id;
            "error": str.from_int(0);  (* placeholder *)
        });
        ret;
    };

    let text = json.get_str(msg; "text") catch |_| { ret; };

    log.info(logger; "message received"; log.Fields{
        "conn_id": id;
        "length": str.from_int(str.len(text));
    });

    (* Build the broadcast envelope *)
    let envelope = json.new_object();
    json.set(envelope; "type"; json.from_str("chat"));
    json.set(envelope; "from"; json.from_str(id));
    json.set(envelope; "text"; json.from_str(text));

    (* Broadcast to all connected clients *)
    let payload = json.encode(envelope);
    ws.pool_broadcast(clients; payload);

    log.debug(logger; "broadcast complete"; log.Fields{
        "recipients": str.from_int(ws.pool_size(clients));
    });
};

(* Start the WebSocket server *)
let server = ws.listen(9000)!;
ws.on_connect(server; on_connect);
ws.on_disconnect(server; on_disconnect);
ws.on_message(server; on_message);

log.info(logger; "websocket server started"; log.Fields{
    "port": "9000";
});

ws.serve(server);
```

## Step-by-Step Explanation

1. **Logger setup** -- `log.new` creates a structured logger that outputs JSON to stdout. The level filter means debug messages only appear when the level is lowered.

2. **Connection pool** -- `ws.new_pool` manages the set of active connections. It provides broadcast and size-tracking without manual bookkeeping.

3. **Connect handler** -- When a client connects, it is added to the pool. A structured log entry records the connection id and current pool size.

4. **Disconnect handler** -- The client is removed from the pool and a system message is broadcast to remaining clients so they know someone left.

5. **Message handler** -- Incoming JSON is parsed with error handling via `catch`. The text field is extracted, wrapped in a broadcast envelope with sender info, and sent to all clients.

6. **Server startup** -- `ws.listen` binds the port. The three event handlers are registered. `ws.serve` blocks and runs the event loop.
