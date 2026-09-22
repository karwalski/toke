---
title: std.mdns
slug: mdns
section: reference/stdlib
order: 47
---

> **Status: FAÇADE -- do not write against this page's API.** `src/stdlib/mdns.c` is 788 lines of Bonjour/DNS-SD implementation. `src/stdlib/mdns_glue.c` is 46 lines and calls none of it: every wrapper is `(void)arg; return 0;`. Two published functions -- `mdns.isavailable` and `mdns.resolve` -- have **no wrapper at all** and fail at link. Two more are published with one arity and implemented with another, so the call you write is not the call the interface documents. Nothing on this page has ever discovered or advertised a service from a toke program. Same class as story 136.44 (`std.tls`) and 136.16 (`std.toon`).

`std.mdns` is intended to provide mDNS (Multicast DNS) service advertisement and discovery, compatible with Apple Bonjour and the wider zero-configuration networking ecosystem (RFC 6762 / RFC 6763).

## What is actually reachable today

Measured against the built compiler, not read off the interface.

| Published call | `.tki` signature | Glue signature | Reachable | Behaviour |
|---|---|---|---|---|
| `mdns.isavailable` | `(): bool` | — | **no** | `E9003`, undefined symbol `_tk_mdns_isavailable_w` |
| `mdns.resolve` | `(str; str): ?($discovered)` | — | **no** | `E9003`, undefined symbol `_tk_mdns_resolve_w` |
| `mdns.advertise` | `($service_record): bool` | `(i64; i64; i64)` | at **3** args | returns 0 |
| `mdns.browse` | `(str; fn($discovered): void): bool` | `(i64)` | at **1** arg | **the callback parameter does not exist in the implementation**; returns an empty array |
| `mdns.stopadvertise` | `(str): bool` | `(i64)` | links | returns 0 |
| `mdns.stopbrowse` | `(str): bool` | `(i64)` | links | returns 0 |
| `mdns.servicerecord` | `(i64; i64; i64): i64` | `(i64; i64; i64)` | links | returns 0 -- it constructs nothing |

Two of these are worse than a stub that returns the wrong answer.

**`mdns.browse` has lost its callback.** The interface publishes `browse(type; cb)` where `cb` is invoked per discovered service. The wrapper takes the service type alone and returns an empty array. A program cannot register a discovery callback at all, so the module has no discovery path even in principle — and a caller writing the documented two-argument form gets `E4026` rather than a runtime surprise, which is the one mercy here.

**`mdns.servicerecord` constructs nothing.** It exists because `$service_record` cannot be written in toke — see below — so it is the only way to build the argument `advertise` wants. It returns 0.

Because `isavailable` does not link, a toke program cannot even perform the availability check this page used to open with. The only compiling example is the shape a consumer must take today: assume no mDNS, and configure the host and port by hand.

```toke
m=mdnsfallback;
i=io:std.io;
i=env:std.env;
i=str:std.str;

(* std.mdns is not imported. isavailable() has no wrapper and fails at link,
   so there is no way to ask whether discovery is possible -- a consumer has
   to take the manual path unconditionally. *)
f=main():i64{
  let host=env.getor("SERVICEHOST";"127.0.0.1");
  let port=env.getor("SERVICEPORT";"8080");
  io.println(str.concat("using configured endpoint ";str.concat(host;str.concat(":";port))));
  <0
};
```

## The underscore problem, on top of the glue problem

Toke's default 59-character profile excludes `_`, so these published names cannot be written in a toke program at all:

| Name | Kind | Why it cannot be written |
|---|---|---|
| `service_record` | type | `$service_record{...}` will not lex |
| `mdns.is_available` | call-name | as this page previously documented it; the interface already spells it `isavailable` |
| `mdns.stop_advertise`, `mdns.stop_browse` | call-name | as above; the interface spells them `stopadvertise`, `stopbrowse` |

`$discovered` is fine, and so are all eight field names on the two record types (`name`, `type`, `port`, `txt`, `host`) -- the problem is confined to `service_record` and the prose's stale call-names. That one unspellable type name is why `mdns.servicerecord` exists as a constructor, the same pattern `std.tls` uses for `TlsConfig`.

Two further things this page previously got wrong independently of the glue: the examples opened with `import std.mdns;`, which is not toke's import syntax (`i=mdns:std.mdns;` is), and built struct literals with `,` separators rather than `;`.

## The design, kept as the record

Everything below describes what a restored module should expose. **It is not callable as written.**

### Types

#### servicerecord

A service to advertise. Published today as `service_record`, which is unspellable.

| Field | Type | Meaning |
|---|---|---|
| `name` | `str` | human-readable instance name, e.g. `"My Server"` |
| `type` | `str` | DNS-SD service type, e.g. `"_http._tcp"` |
| `port` | `i32` | port the service listens on |
| `txt` | `@(str)` | TXT entries in `"key=value"` form |

#### $discovered

A service instance found on the network: `name`, `host` (resolved), `port`, and `txt` in the same form.

### Functions

#### mdns.isavailable(): bool

Whether mDNS is supported here. Intended to be `true` on macOS via the system `dns_sd` API, `false` on Linux (Avahi support planned) and Windows. Has no wrapper.

#### mdns.advertise(svc: servicerecord): bool

Registers a service for advertisement, which persists until `mdns.stopadvertise` or process exit. Intended to fail when mDNS is unavailable, when an instance of the same name is already advertised, or when the Bonjour call fails.

#### mdns.stopadvertise(name: str): bool

Withdraws a service previously advertised under `name`.

#### mdns.browse(type: str; cb: fn): bool

Starts browsing for instances of `type`, invoking `cb` for each one discovered. Browsing continues until `mdns.stopbrowse`. The wrapper has no callback parameter.

#### mdns.stopbrowse(type: str): bool

Stops a browse started for `type`.

#### mdns.resolve(name: str; type: str): ?($discovered)

Resolves one named instance to a host and port without browsing. Has no wrapper.

#### mdns.servicerecord(name; type; port): servicerecord

Constructs a `servicerecord`, which exists because the type name cannot be written as a literal. Returns 0.

### Service types used by loke

- `_loke-mcp._tcp` -- loke MCP server advertisement
- `_loke-companion._tcp` -- loke companion service advertisement

## Restoring it

A real `mdns_glue.c`: seven wrappers calling the core in `mdns.c`, at the published arities rather than the current ones; `_w` symbols for `isavailable` and `resolve`, which do not exist; a callback parameter on `browse`, which needs the same per-handle trampoline decision that blocked `std.webview`'s `registerhandler` and `onclose` (131.38); a `servicerecord` that builds something; and a rename of the `service_record` type. The core is written. The layer is not.

## See Also

- `std.net` -- the manual path: explicit host and port, which is what a consumer must use today.
- `std.webview` -- withdrawn for a related reason, and the page that records why a toke callback ABI is the blocker.
