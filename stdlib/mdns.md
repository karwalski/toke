# std.mdns -- mDNS/Bonjour Service Advertisement and Discovery

## Overview

The `std.mdns` module provides mDNS (Multicast DNS) service advertisement and
discovery, compatible with Apple Bonjour and the wider zero-configuration
networking ecosystem (RFC 6762 / RFC 6763).

**Platform support:**

| Platform | Status |
|----------|--------|
| macOS    | Full — uses the system `dns_sd` Bonjour API |
| Linux    | Stub — Avahi support planned; `mdns.is_available()` returns `false` |
| Windows  | Stub — future implementation; `mdns.is_available()` returns `false` |

Always call `mdns.is_available()` before using other functions when writing
cross-platform toke programs.

**Service types used by loke:**

- `_loke-mcp._tcp` — loke MCP server advertisement
- `_loke-companion._tcp` — loke companion service advertisement

## Types

### $service_record

Describes a service to advertise on the local network.

| Field | Type    | Meaning |
|-------|---------|---------|
| name  | str     | Human-readable service instance name (e.g. `"My Server"`) |
| type  | str     | DNS-SD service type (e.g. `"_http._tcp"`) |
| port  | i32     | TCP or UDP port the service listens on |
| txt   | @(str)  | TXT record entries in `"key=value"` format |

### $discovered

Describes a service instance discovered on the network.

| Field | Type   | Meaning |
|-------|--------|---------|
| name  | str    | Service instance name |
| host  | str    | Resolved hostname |
| port  | i32    | Port the service is listening on |
| txt   | @(str) | TXT record entries in `"key=value"` format |

## Functions

### mdns.is_available(): bool

Returns `true` if mDNS is supported on the current platform. On macOS this
always returns `true`. On Linux and Windows it returns `false` until native
support is implemented.

**Example:**
```toke
import std.mdns;

if !mdns.is_available() {
  (* fall back to manual host/port configuration *)
}
```

### mdns.advertise(svc: $service_record): bool

Registers a service for advertisement on the local network. Returns `true` on
success. The service remains advertised until `mdns.stop_advertise` is called
or the process exits.

TXT record entries must be in `"key=value"` format. Empty `txt` arrays are
allowed.

Returns `false` if:
- mDNS is not available on this platform
- A service with the same `name` is already being advertised
- The underlying Bonjour call fails

**Example:**
```toke
import std.mdns;

let svc = $service_record {
  name: "My MCP Server",
  type: "_loke-mcp._tcp",
  port: 8080,
  txt: @("version=1", "proto=toke")
};

let ok = mdns.advertise(svc);
```

### mdns.stop_advertise(name: str): bool

Stops advertising the service with the given instance `name`. Returns `true` if
the registration was found and removed, `false` otherwise.

**Example:**
```toke
import std.mdns;

let stopped = mdns.stop_advertise("My MCP Server");
```

### mdns.browse(service_type: str, cb: fn($discovered): void): bool

Starts an asynchronous background browse for services of the given
`service_type`. Each time a service is found or re-confirmed, `cb` is called
with a `$discovered` value.

The browse runs on a background thread; `cb` may be called from that thread.
Returns `true` if the browse was started, `false` on error or if browsing for
`service_type` is already active.

Call `mdns.stop_browse` to cancel.

**Example:**
```toke
import std.mdns;

let found = mdns.browse("_loke-mcp._tcp", fn(d: $discovered): void {
  (* called each time a service is found *)
  let info = d.name ++ " at " ++ d.host;
});
```

### mdns.stop_browse(service_type: str): bool

Stops the background browse for the given `service_type`. Returns `true` if a
browse for that type was active and has been stopped, `false` otherwise.

**Example:**
```toke
import std.mdns;

let stopped = mdns.stop_browse("_loke-mcp._tcp");
```

### mdns.resolve(name: str, service_type: str): ?($discovered)

Synchronously resolves a named service instance. Blocks until the host and port
are resolved or a 5-second timeout expires. Returns the resolved `$discovered`
on success, or `none` on timeout or error.

**Example:**
```toke
import std.mdns;

let result = mdns.resolve("My MCP Server", "_loke-mcp._tcp");
match result {
  some(d) => (* connect to d.host:d.port *),
  none    => (* service not found or timed out *)
}
```

## Usage pattern — advertise and browse

```toke
import std.mdns;

(* Only proceed on supported platforms *)
if mdns.is_available() {

  (* Advertise this process as an MCP server *)
  let svc = $service_record {
    name: "loke-mcp-local",
    type: "_loke-mcp._tcp",
    port: 9000,
    txt: @("v=1")
  };
  mdns.advertise(svc);

  (* Browse for companion services *)
  mdns.browse("_loke-companion._tcp", fn(d: $discovered): void {
    let addr = d.host ++ ":" ++ str(d.port);
    (* connect to companion *)
  });

  (* ... run event loop ... *)

  (* Clean up *)
  mdns.stop_browse("_loke-companion._tcp");
  mdns.stop_advertise("loke-mcp-local");
}
```

## Notes

- Service instance names must be unique per type on the local network. If a
  collision occurs, Bonjour may rename the service automatically (appending a
  number). The C layer does not surface the renamed instance name; check
  `mdns.stop_advertise` return values to confirm teardown.
- TXT records are limited to 255 bytes per entry by the DNS-SD specification.
- `mdns.resolve()` performs a one-shot lookup and does not keep a persistent
  connection to Bonjour. For continuous monitoring, prefer `mdns.browse`.
