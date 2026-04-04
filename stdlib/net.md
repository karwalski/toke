# std.net -- TCP Network Operations

## Overview

The `std.net` module provides low-level TCP socket operations for building network clients and servers. For higher-level HTTP functionality, see `std.http`. All operations that can fail return a result type with `NetErr`.

## Types

### Listener

Represents a bound TCP server socket that accepts incoming connections.

| Field | Type | Meaning |
|-------|------|---------|
| addr | Str | The address the listener is bound to (e.g., `"0.0.0.0:8080"`) |

### Conn

Represents an open TCP connection, either accepted from a listener or established via `net.connect`.

| Field | Type | Meaning |
|-------|------|---------|
| remote | Str | The remote address of the peer (e.g., `"192.168.1.5:43210"`) |
| local | Str | The local address of this end of the connection |

## Functions

### net.listen(addr: Str): Listener!NetErr

Binds a TCP listener to the given address and port. Returns `NetErr.AddrInUse` if the address is already bound, or `NetErr.Permission` if binding requires elevated privileges.

**Example:**
```toke
let ln = net.listen("0.0.0.0:8080");
(* ln = ok(Listener{addr: "0.0.0.0:8080"}) *)
```

### net.accept(ln: Listener): Conn!NetErr

Blocks until a client connects to the listener and returns the new connection. Returns `NetErr.IO` on failure.

**Example:**
```toke
let ln = net.listen("0.0.0.0:9000");
let conn = net.accept(ln!);
(* conn = ok(Conn{remote: "192.168.1.5:43210"; local: "0.0.0.0:9000"}) *)
```

### net.connect(addr: Str): Conn!NetErr

Establishes a TCP connection to the given address. Returns `NetErr.ConnRefused` if the remote host refuses the connection, `NetErr.Timeout` if the connection attempt times out, or `NetErr.IO` on other failures.

**Example:**
```toke
let conn = net.connect("127.0.0.1:8080");
(* conn = ok(Conn{remote: "127.0.0.1:8080"; local: "127.0.0.1:54321"}) *)
```

### net.send(conn: Conn; data: Str): u64!NetErr

Sends `data` over the connection and returns the number of bytes written. Returns `NetErr.IO` on failure or `NetErr.Timeout` if the send times out.

**Example:**
```toke
let n = net.send(conn; "hello");
(* n = ok(5) *)
```

### net.recv(conn: Conn; max: u64): Str!NetErr

Reads up to `max` bytes from the connection and returns the data as a string. Returns an empty string if the peer has closed the connection. Returns `NetErr.IO` on failure or `NetErr.Timeout` if the read times out.

**Example:**
```toke
let data = net.recv(conn; 1024);
(* data = ok("hello") *)
```

### net.close(conn: Conn): bool!NetErr

Closes the connection. Returns `true` on success. Returns `NetErr.IO` if the close fails.

**Example:**
```toke
let ok = net.close(conn);
(* ok = ok(true) *)
```

### net.close_listener(ln: Listener): bool!NetErr

Closes the listener and stops accepting new connections. Returns `true` on success.

**Example:**
```toke
let ok = net.close_listener(ln);
(* ok = ok(true) *)
```

## Error Types

### NetErr

A sum type representing network operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| ConnRefused | Str | The remote host refused the connection |
| Timeout | Str | The operation timed out |
| AddrInUse | Str | The address is already in use |
| Permission | Str | The operation requires elevated privileges |
| IO | Str | A general I/O or network error occurred |

## See Also

- [std.http](http.md) -- for HTTP server and client functionality
- [std.str](str.md) -- for string operations used with network data
