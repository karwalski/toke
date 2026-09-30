---
title: std.net
slug: net
section: reference/stdlib
order: 28
---

The `std.net` module is planned for a future release. It will provide low-level TCP and UDP socket operations for use cases not covered by the higher-level `std.http` module.

**Status:** Partial. A minimal IPv4 TCP server surface exists -- `net.listen`, `net.accept`, `net.read`, `net.write`, `net.close`, `net.portavailable` -- and needs the `net` capability (`--allow-net`). Only `net.listen` is typed; the rest are untyped ABI entries in `stdlib/net.tki`.

## Functions

### net.listen(addr: $str): i64

Opens a TCP socket listening on `addr`, an IPv4 `"host:port"` (`"127.0.0.1:8080"`), and returns its file descriptor, or `0` on failure (bad address, port in use, no capability).

Before 138.13 the parameter was untyped and `net.listen(8080)` passed the port where a string is read: the program crashed (SIGSEGV). Since 138.13 the checker rejects an integer there (E4031 at `--check`), and the runtime, if it is ever handed one, takes an integer below 65536 as a **port on the loopback address** (`127.0.0.1`) -- never every interface. Write the address you mean.

`net.listen` returns `0` when another server already listens on the port --
at the same address, or at `127.0.0.1` for a `0.0.0.0:port` listen, or at all
interfaces for a `127.0.0.1:port` listen. On macOS such a listen used to
succeed and silently take that address's traffic (135.33). A port whose old
connections are still closing (TIME_WAIT) still listens.

### net.portavailable(port: int): int

`1` when `127.0.0.1:port` can be bound now, `0` when it cannot (including a
port another server listens on at all interfaces -- on macOS that used to
read as available, 135.33). A port in TIME_WAIT reads as available: a server
can bind it.

**Limit (both functions):** the "is a server already there?" check for an
all-interfaces address asks `127.0.0.1` only, so a server listening on one
other specific address alone is not detected. On Linux the bind fails either
way.

## Planned Scope

The following capabilities are under consideration:

- TCP client connections (connect, read, write, close)
- TCP server sockets (bind, listen, accept)
- UDP send and receive
- Address resolution

## See Also

- [std.http](/docs/stdlib/http/) -- for HTTP server and client functionality available today
