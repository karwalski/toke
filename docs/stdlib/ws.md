---
title: std.ws
slug: ws
section: reference/stdlib
order: 40
---

**Status: Implemented** -- C runtime backing.

The `std.ws` module provides a WebSocket client. Connect to a server, send and receive text or binary messages, and broadcast to multiple connections.

## Types

### $wsconn

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | Connection identifier |
| ready | bool | Whether the connection is open and ready |

### $wsmsg

| Field | Type | Meaning |
|-------|------|---------|
| payload | @($byte) | Message payload bytes |
| fin | bool | Whether this is the final fragment |
| opcode | u64 | WebSocket frame opcode (1=text, 2=binary) |

### $wserr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `ws.connect` | `url: $str` | `$wsconn!$wserr` | Open a WebSocket connection to the given URL |
| `ws.send` | `conn: $wsconn; msg: $str` | `void!$wserr` | Send a text message |
| `ws.sendbytes` | `conn: $wsconn; data: @($byte)` | `void!$wserr` | Send a binary message |
| `ws.recv` | `conn: $wsconn` | `$wsmsg!$wserr` | Block until a message is received |
| `ws.close` | `conn: $wsconn` | `void` | Close the connection |
| `ws.broadcast` | `conns: @($wsconn); msg: $str` | `void` | Send a text message to all connections |

## Usage

```toke
m=app;
i=ws:std.ws;
i=log:std.log;

f=main():i64{
  let conn=ws.connect("ws://localhost:8080/chat")|{$ok:c c;$err:e $wsconn{id:0;ready:false}};
  ws.send(conn;"hello")|{$ok:v v;$err:e log.warn("send failed";@())};
  let msg=ws.recv(conn)|{$ok:m m;$err:e $wsmsg{payload:@();fin:false;opcode:0}};
  ws.close(conn);
  <0
};
```

## Combined Example

```toke
m=app;
i=ws:std.ws;
i=enc:std.encoding;
i=log:std.log;

f=main():i64{
  let conn=ws.connect("ws://localhost:8080/echo")|{
    $ok:c c;
    $err:e $wsconn{id:0;ready:false}
  };

  ws.send(conn;"ping")|{$ok:v v;$err:e log.warn("send failed";@())};

  let msg=ws.recv(conn)|{$ok:m m;$err:e $wsmsg{payload:@();fin:false;opcode:0}};
  let text=enc.b64encode(msg.payload);
  log.info(text;@());

  let c2=ws.connect("ws://localhost:8080/echo")|{$ok:c c;$err:e $wsconn{id:0;ready:false}};
  ws.broadcast(@(conn;c2);"bye");

  ws.close(conn);
  ws.close(c2);
  <0
};
```

## Dependencies

None.
