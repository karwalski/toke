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

An **opaque handle** returned by `ws.connect`. Pass it to the other `ws`
functions; do not construct one and do not rely on its fields.

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | Connection identifier |
| ready | bool | Whether the connection is open and ready |

> **Never build a `$wsconn` yourself.** The value that crosses into the
> runtime is a live socket handle, not the two fields above. A hand-written
> `$wsconn{id:0;ready:false}` is a zeroed block, and until story 127.112 the
> runtime read it as an *open connection on file descriptor 0* — `ws.send`
> wrote a real RFC 6455 frame to the caller's terminal and returned success,
> and `ws.close` then closed the caller's standard input. The runtime now
> rejects any handle it did not issue, so such a call fails honestly; the
> fabrication is still wrong, and there is never a reason to write one.
> See [Handling a failed connect](#handling-a-failed-connect).

### $wsmsg

| Field | Type | Meaning |
|-------|------|---------|
| payload | @($byte) | Message payload bytes |
| fin | bool | Whether this is the final fragment |
| opcode | u64 | WebSocket frame opcode (1=text, 2=binary) |

> **Do not read these fields yet.** `ws.recv` is declared to return
> `$wsmsg!$wserr`, but the runtime does not currently return a `$wsmsg` —
> reading `msg.payload` off the result traps (SIGBUS, exit 138), verified
> against a live WebSocket server. Until that is resolved, match on `ws.recv`
> to learn whether a message arrived; do not dereference the value. The
> examples below are written that way. This is a defect, not a design.

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

## Handling a failed connect

`ws.connect` returns `$wsconn!$wserr`, so every caller has to say what happens
when the connect fails. There are exactly two correct answers: **leave**, or
**retry**. What you must not do is manufacture a substitute handle to satisfy
the type — there is no such thing as an unconnected `$wsconn`, and every
subsequent `ws.*` call on the fabrication is a call on a connection that was
never opened.

Leave, reporting the failure:

```toke
m=app;
i=ws:std.ws;
i=log:std.log;

f=fail(msg:$str):i64{
  log.error(msg;@());
  <1
};

f=main():i64{
  let conn=mt ws.connect("ws://localhost:8080/chat"){$ok:c c;$err:e <fail("ws.connect failed")};
  mt ws.send(conn;"hello"){$ok:v v;$err:e log.warn("send failed";@())};
  ws.close(conn);
  <0
};
```

A match arm is a single expression, so the "log it and go" step is a small
helper whose `<1` leaves `main` from inside the arm. If the caller is itself
fallible, propagate instead — `$err:e <err("connect failed")` —
and let the decision belong to whoever can actually make it.

## Usage

```toke
m=app;
i=ws:std.ws;
i=log:std.log;

f=fail(msg:$str):i64{
  log.error(msg;@());
  <1
};

f=main():i64{
  let conn=mt ws.connect("ws://localhost:8080/chat"){$ok:c c;$err:e <fail("ws.connect failed")};
  mt ws.send(conn;"hello"){$ok:v v;$err:e log.warn("send failed";@())};
  mt ws.recv(conn){$ok:m log.info("got a message";@());$err:e log.warn("recv failed";@())};
  ws.close(conn);
  <0
};
```

## Combined Example

```toke
m=app;
i=ws:std.ws;
i=log:std.log;

f=fail(msg:$str):i64{
  log.error(msg;@());
  <1
};

f=main():i64{
  let conn=mt ws.connect("ws://localhost:8080/echo"){$ok:c c;$err:e <fail("ws.connect failed")};

  mt ws.send(conn;"ping"){$ok:v v;$err:e log.warn("send failed";@())};
  mt ws.recv(conn){$ok:m log.info("echo received";@());$err:e log.warn("recv failed";@())};

  let c2=mt ws.connect("ws://localhost:8080/echo"){$ok:c c;$err:e <fail("second ws.connect failed")};
  ws.broadcast(@(conn;c2);"bye");

  ws.close(conn);
  ws.close(c2);
  <0
};
```

Both connects leave on failure rather than substituting a handle, so
`ws.broadcast` is only ever reached with two connections that really opened.

## Dependencies

None.
