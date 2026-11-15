---
title: std.net
slug: net
section: reference/stdlib
order: 28
---

The `std.net` module provides low-level network operations for use cases not covered by the higher-level `std.http` module: a resolver that returns every address a name has, and a minimal TCP server surface. UDP and a TCP client are not part of it yet.

**Status:** Partial. A minimal IPv4 TCP server surface exists -- `net.listen`, `net.accept`, `net.read`, `net.write`, `net.close`, `net.portavailable` -- and a resolver, `net.resolve`; all need the `net` capability (`--allow-net`). `net.listen` and `net.resolve` are typed; the rest are untyped ABI entries in `stdlib/net.tki`.

## Types

### $neterr

The `$err` of `net.resolve` (story 138.25). It is a struct with the shape of std.http's `$httperr`, so code that handles one network error can handle the other.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | What failed, e.g. `net.resolve: example.invalid: nodename nor servname provided, or not known` |
| code | u64 | `0`: a failure with no number of its own |

## Functions

### net.resolve(name: $str): @($str)!$neterr

Returns every address `name` resolves to, A and AAAA records alike, each as a numeric string (`"192.0.2.10"`, `"2001:db8::10"`). Each address appears once, in the order the system resolver returned them. An address literal resolves to itself. This is one `getaddrinfo` call of any family, without `AI_ADDRCONFIG`: the caller gets the addresses the name has, not only those this host can reach today. A name that does not resolve, and an empty name, is `$err` (a `$neterr`, `code` 0).

A lookup is network traffic, so the call needs the `net` capability, as a connection does. Without the grant the program stops with `CAP001` (story 138.25, owner answer 2.2).

It exists so that a program fetching a URL it does not control can check where the request would go before it connects:

1. resolve the name;
2. refuse the fetch if any address is private, loopback, link-local or a metadata address;
3. connect to the address it checked with [`http.withconnectaddr`](/docs/stdlib/http/#httpwithconnectaddrclient-httpclient-addr-str-httpclient).

The certificate is still verified against the URL's name. Without step 3 the HTTP client would resolve the name again when it connects, and a name can answer differently the second time (DNS rebinding).

```toke
m=resolvedemo;
i=net:std.net;
i=io:std.io;
f=main():i64{
  let addrs=mt net.resolve("localhost") {$ok:a a; $err:e @()};
  lp(let i=0;i<addrs.len;i=i+1){
    io.println(addrs.get(i));
  };
  <0
};
```

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
- Reverse resolution (address to name); forward resolution is `net.resolve`

## See Also

- [std.http](/docs/stdlib/http/) -- for HTTP server and client functionality available today
