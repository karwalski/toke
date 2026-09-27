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

Before 138.13 the parameter was untyped and `net.listen(8080)` passed the port where a string is read: the program crashed (SIGSEGV). The checker does not yet reject an integer there, so the runtime now takes an integer below 65536 as a **port on the loopback address** (`127.0.0.1`) -- never every interface. Write the address you mean.

## Planned Scope

The following capabilities are under consideration:

- TCP client connections (connect, read, write, close)
- TCP server sockets (bind, listen, accept)
- UDP send and receive
- Address resolution

## See Also

- [std.http](/docs/stdlib/http/) -- for HTTP server and client functionality available today
