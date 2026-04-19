# std.secure_mem — Secure Ephemeral Memory

## Overview

The `std.secure_mem` module provides mlock'd, zero-on-free, TTL-expiring memory
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
pressure), `is_available()` returns `false` and a warning is logged, but
allocation still succeeds — secrets are stored in ordinary heap memory without
swap protection.

---

## Types

### $secure_buf

```toke
type $secure_buf {
  id:         str   (* opaque unique identifier *)
  size:       i32   (* capacity in bytes *)
  expires_at: i64   (* Unix epoch seconds; 0 = no expiry *)
}
```

The `$secure_buf` handle is a lightweight descriptor. The actual locked memory
is managed internally and is keyed by `id`.

---

## Functions

### secure_mem.alloc(size_bytes: i32; ttl_seconds: i32) -> $secure_buf

Allocates a locked memory region of `size_bytes` bytes with a TTL of
`ttl_seconds` seconds from the time of allocation.

- `ttl_seconds = 0` means no expiry (the buffer lives until explicitly wiped or
  the process exits).
- Returns a `$secure_buf` handle. The underlying memory is zeroed immediately
  after allocation.
- Locks the page with `mlock()` where available; logs a warning and continues
  if locking fails.

**Example:**
```toke
let buf = secure_mem.alloc(64; 300);   (* 64-byte buffer, 5-minute TTL *)
```

---

### secure_mem.write(buf: $secure_buf; data: str) -> bool

Copies `data` into the secure buffer identified by `buf.id`.

- Returns `true` on success.
- Returns `false` if the buffer has expired, has been wiped, or if `data` is
  longer than `buf.size` bytes.
- The write is bounds-checked; data is truncated to `buf.size - 1` bytes to
  ensure a NUL terminator fits.

**Example:**
```toke
let ok = secure_mem.write(buf; "s3cr3t-tok3n");
```

---

### secure_mem.read(buf: $secure_buf) -> ?(str)

Returns the contents of the buffer as a string, or `None` if the buffer has
expired, been wiped, or does not exist.

- The returned string is a **copy** allocated with `malloc`; the caller owns it
  and must free it (the toke runtime handles this automatically).
- Calling `read()` does not reset the expiry clock.

**Example:**
```toke
match secure_mem.read(buf) {
  Some(s) -> log.info("secret: " + s)
  None    -> log.warn("buffer expired or wiped")
}
```

---

### secure_mem.wipe(buf: $secure_buf) -> bool

Immediately zeros the buffer contents and releases the memory region.

- Returns `true` if the buffer was found and wiped.
- Returns `false` if the buffer was already wiped or never existed.
- After a successful wipe, subsequent `read()` calls return `None`.

**Example:**
```toke
let wiped = secure_mem.wipe(buf);
```

---

### secure_mem.sweep() -> i32

Walks all live allocations, zeros and frees every buffer whose TTL has elapsed,
and returns the count of buffers that were freed.

Call periodically (e.g. from a background task) to prevent expired secrets from
accumulating in memory.

**Example:**
```toke
let freed = secure_mem.sweep();
log.info("swept " + str(freed) + " expired buffers");
```

---

### secure_mem.is_available() -> bool

Returns `true` if `mlock()` succeeds on a one-page test allocation, `false`
otherwise.

Use this to detect whether the current process has the `CAP_IPC_LOCK` privilege
(Linux) or equivalent, before relying on swap-protection guarantees.

**Example:**
```toke
if !secure_mem.is_available() {
  log.warn("mlock unavailable: secrets may be paged to disk")
}
```

---

## Platform Notes

| Platform | Swap lock mechanism          | Notes                                      |
|----------|------------------------------|--------------------------------------------|
| Linux    | `mlock(2)`                   | Requires `CAP_IPC_LOCK` or `RLIMIT_MEMLOCK` headroom |
| macOS    | `mlock(2)`                   | Available without special privileges for small regions |
| Windows  | `VirtualLock()`              | Enabled via `<memoryapi.h>`                |
| Other    | None (graceful degradation)  | `is_available()` returns `false`; allocation still works |

## Security Considerations

- Buffer IDs are generated from an atomic counter and are **not** cryptographic
  nonces. Do not expose them outside the process.
- `sweep()` is not called automatically. If your application is long-running,
  ensure it is invoked regularly.
- `mlock()` prevents swap but does not protect against a privileged process
  reading `/proc/<pid>/mem` on Linux. For stronger isolation, combine with OS
  process sandboxing.
