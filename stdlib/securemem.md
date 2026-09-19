# std.securemem — Secure Ephemeral Memory

## Overview

The `std.securemem` module provides mlock'd, zero-on-free, TTL-expiring memory
buffers for storing short-lived secrets (tokens, passwords, private keys) safely
in memory.

Key properties:

- **Swap prevention** — each buffer is locked into RAM via `mlock()` (POSIX) or
  `VirtualLock()` (Windows) so the operating system will not page it to disk.
- **Zeroed on release** — buffers are wiped with a compiler-barrier-protected
  zero (equivalent to `explicit_bzero`) before their memory is freed, preventing
  secrets from lingering in heap memory.
- **TTL expiry** — every buffer carries an expiry timestamp. `read()` refuses to
  return data from an expired buffer. `sweep()` bulk-frees all expired buffers.
- **Thread safety** — the internal allocation list is protected by a
  `pthread_mutex_t`; all public functions are safe to call from multiple threads.

On platforms where `mlock()` is unavailable or fails (e.g. very high memory
pressure), `isavailable()` returns `false` and a warning is logged, but
allocation still succeeds — secrets are stored in ordinary heap memory without
swap protection.

---

## Types

### SecureBuf

`SecureBuf` is an **opaque handle**. Pass it whole to `write`, `read` and
`wipe`; it has no readable fields.

> **136.5 — fields withdrawn, not forgotten.** The interface used to publish
> `id`, `size` and `expires_at` on this type. None of the three worked. The glue
> hands toke a raw C `TkSecureBuf` (`char id[24]` at 0, `size` at 24,
> `expires_at` at 32, 40 bytes total), while the compiler lays a struct out as
> one i64 slot per field at 0/8/16 — so `buf.size` read bytes out of the middle
> of the id character array and returned the id's first byte. Three consecutive
> `alloc(128)`, `alloc(256)`, `alloc(512)` calls reported sizes 49, 50, 51:
> the ASCII codes of the ids "1", "2", "3". `expires_at` could not even be
> named, the lexer rejecting the underscore. The fields are withdrawn rather
> than left as a trap; restoring them needs a glue change and has its own story.

The `SecureBuf` handle is a lightweight descriptor. The actual locked memory
is managed internally and is keyed by `id`.

---

## Functions

### securemem.alloc(sizebytes: i32; ttlseconds: i32) -> SecureBuf

Allocates a locked memory region of `size_bytes` bytes with a TTL of
`ttl_seconds` seconds from the time of allocation.

- `ttl_seconds = 0` means no expiry (the buffer lives until explicitly wiped or
  the process exits).
- Returns a `SecureBuf` handle. The underlying memory is zeroed immediately
  after allocation.
- Locks the page with `mlock()` where available; logs a warning and continues
  if locking fails.

**Example:**
```toke
let buf=securemem.alloc(64;300);   (* 64-byte buffer, 5-minute TTL *)
```

---

### securemem.write(buf: SecureBuf; data: str) -> bool

Copies `data` into the secure buffer identified by `buf.id`.

- Returns `true` on success.
- Returns `false` if the buffer has expired, has been wiped, or if `data` is
  longer than `buf.size` bytes.
- The write is bounds-checked; data is truncated to `buf.size - 1` bytes to
  ensure a NUL terminator fits.

**Example:**
```toke
let ok=securemem.write(buf;"s3cr3t-tok3n");
```

---

### securemem.read(buf: SecureBuf) -> ?(str)

Returns the contents of the buffer as a string, or `None` if the buffer has
expired, been wiped, or does not exist.

- The returned string is a **copy** allocated with `malloc`; the caller owns it
  and must free it (the toke runtime handles this automatically).
- Calling `read()` does not reset the expiry clock.

**Example:**
```toke
let s=securemem.read(buf);
if(str.len(s)>0){
  log.info(str.concat("secret: ";s))
}el{
  log.warn("buffer expired or wiped")
};
```

> **Do not test the result with `==`.** `read` returns the `?(str)` none
> sentinel when the buffer has expired or been wiped, and comparing that
> sentinel with `==` segfaults the caller (story 127.83). `str.len()` is
> null-safe and is the probe to use until that is fixed. Returning `""` instead
> would be the wrong fix: it would make a wiped secret indistinguishable from
> an empty one.

---

### securemem.wipe(buf: SecureBuf) -> bool

Immediately zeros the buffer contents and releases the memory region.

- Returns `true` if the buffer was found and wiped.
- Returns `false` if the buffer was already wiped or never existed.
- After a successful wipe, subsequent `read()` calls return `None`.

**Example:**
```toke
let wiped=securemem.wipe(buf);
```

---

### securemem.sweep() -> i32

Walks all live allocations, zeros and frees every buffer whose TTL has elapsed,
and returns the count of buffers that were freed.

Call periodically (e.g. from a background task) to prevent expired secrets from
accumulating in memory.

**Example:**
```toke
let freed=securemem.sweep();
log.info(str.concat("swept ";str.concat(str.fromint(freed);" expired buffers")));
```

---

### securemem.isavailable() -> bool

Returns `true` if `mlock()` succeeds on a one-page test allocation, `false`
otherwise.

Use this to detect whether the current process has the `CAP_IPC_LOCK` privilege
(Linux) or equivalent, before relying on swap-protection guarantees.

**Example:**
```toke
if(securemem.isavailable()==0){
  log.warn("mlock unavailable: secrets may be paged to disk")
};
```

---

## Platform Notes

| Platform | Swap lock mechanism          | Notes                                      |
|----------|------------------------------|--------------------------------------------|
| Linux    | `mlock(2)`                   | Requires `CAP_IPC_LOCK` or `RLIMIT_MEMLOCK` headroom |
| macOS    | `mlock(2)`                   | Available without special privileges for small regions |
| Windows  | `VirtualLock()`              | Enabled via `<memoryapi.h>`                |
| Other    | None (graceful degradation)  | `isavailable()` returns `false`; allocation still works |

## Security Considerations

- Buffer IDs are generated from an atomic counter and are **not** cryptographic
  nonces. Do not expose them outside the process.
- `sweep()` is not called automatically. If your application is long-running,
  ensure it is invoked regularly.
- `mlock()` prevents swap but does not protect against a privileged process
  reading `/proc/<pid>/mem` on Linux. For stronger isolation, combine with OS
  process sandboxing.
