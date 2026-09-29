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
| id | u64 | Connection identifier: 1 for the process's first `ws.connect`, then 2, ... |
| ready | bool | `true` until `ws.close` is called on this handle |

The two fields are readable (135.30; before, they read the runtime's socket
state as numbers). The handle is still only valid as `ws.connect` issued it:
the runtime recognises the handles it issued, not their field values.

> **Never build a `$wsconn` yourself.** The runtime accepts only a handle
> `ws.connect` issued, whatever its fields say. A hand-written
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

`ws.recv` returns one of these for every frame it receives, text or binary.
`payload` holds the raw bytes — binary-safe, so an embedded `0x00` is data and
not a terminator — `opcode` is 1 for a text frame and 2 for a binary one, and
`fin` says whether this frame completes the message.

> **This was a defect until story 127.145, and the fix is worth knowing
> about.** `ws.recv` was declared to return `$wsmsg!$wserr` and the runtime
> handed back a bare string pointer instead, so reading `msg.payload` off the
> result trapped — SIGBUS, exit 138, verified against a live WebSocket server
> with a clean handshake and `"hello"` delivered. The Combined Example below
> did exactly that, so the documented way to use the module crashed. It also
> meant every non-text frame came back as the `$err` arm, indistinguishable
> from a real failure: `ws.sendbytes` existed with no way to receive what it
> sends. Both are fixed; the fields below are now real and safe to read.

### $wserr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `ws.connect` | `url: $str` | `$wsconn!$wserr` | Open a WebSocket connection to the given `ws://` URL; a URL with a space or control character, or a port outside 1 to 65535, is `$err` |
| `ws.send` | `conn: $wsconn; msg: $str` | `void!$wserr` | Send a text message |
| `ws.sendbytes` | `conn: $wsconn; data: @($byte)` | `void!$wserr` | Send a binary message |
| `ws.recv` | `conn: $wsconn` | `$wsmsg!$wserr` | Block until a message is received |
| `ws.close` | `conn: $wsconn` | `void` | Close the connection; the handle stays valid and every later call on it is `$err` |
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
i=io:std.io;
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
i=io:std.io;
i=log:std.log;

f=fail(msg:$str):i64{
  log.error(msg;@());
  <1
};

f=main():i64{
  let conn=mt ws.connect("ws://localhost:8080/chat"){$ok:c c;$err:e <fail("ws.connect failed")};
  mt ws.send(conn;"hello"){$ok:v v;$err:e log.warn("send failed";@())};
  let n=mt ws.recv(conn){$ok:m m.payload.len;$err:e 0-1};
  io.println(n as $str);
  ws.close(conn);
  <0
};
```

`m.payload` is the frame's bytes and `m.payload.len` its length; `m.opcode`
distinguishes a text frame (1) from a binary one (2), and `m.fin` says whether
the message is complete.

## Combined Example

```toke
m=app;
i=ws:std.ws;
i=io:std.io;
i=log:std.log;

f=fail(msg:$str):i64{
  log.error(msg;@());
  <1
};

f=main():i64{
  let conn=mt ws.connect("ws://localhost:8080/echo"){$ok:c c;$err:e <fail("ws.connect failed")};

  mt ws.send(conn;"ping"){$ok:v v;$err:e log.warn("send failed";@())};
  let echoed=mt ws.recv(conn){$ok:m m.payload.len;$err:e 0-1};
  io.println(echoed as $str);

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
