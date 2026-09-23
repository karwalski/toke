---
title: std.securemem
slug: securemem
section: reference/stdlib
order: 49
---

**Status: Implemented** -- C runtime backing. Unit-tested by `test/stdlib/test_securemem.c`; the null-return rule below is pinned by conformance test `C011_err_payload_and_null.sh`.

`std.securemem` holds a short-lived secret in memory with three properties ordinary heap memory does not have: it is locked out of swap, it is zeroed before it is freed, and it expires.

- **Swap prevention** -- each buffer is locked into RAM with `mlock()` (POSIX) or `VirtualLock()` (Windows), so the OS will not page it to disk where it outlives the process.
- **Zeroed on release** -- buffers are wiped with a compiler-barrier-protected zero (the `explicit_bzero` guarantee) before the memory is freed, so the secret does not linger in the heap for the next allocation to read.
- **TTL expiry** -- every buffer carries an expiry timestamp. `read` refuses to return data from an expired buffer, and `sweep` bulk-frees everything that has elapsed.
- **Thread safety** -- the internal allocation list is under a `pthread_mutex_t`; all six functions are safe to call from several threads.

Where `mlock()` is unavailable or fails -- no `CAP_IPC_LOCK`, no `RLIMIT_MEMLOCK` headroom -- `securemem.isavailable` returns `false` and allocation still succeeds, without swap protection. That is a deliberate graceful degradation, and it is why you should check `isavailable` rather than assume the guarantee.

## Types

### SecureBuf

An **opaque handle**. Pass it whole to `write`, `read` and `wipe`; it has no readable fields.

> **136.5 -- fields withdrawn, not forgotten.** The interface used to publish `id`, `size` and `expires_at` on this type. None of the three worked. The glue hands toke a raw C `TkSecureBuf` (`char id[24]` at 0, `size` at 24, `expires_at` at 32, 40 bytes total), while the compiler lays a struct out as one i64 slot per field at 0/8/16 -- so `buf.size` read bytes out of the middle of the id character array and returned the id's first byte. Three consecutive `alloc(128)`, `alloc(256)`, `alloc(512)` calls reported sizes 49, 50, 51: the ASCII codes of the ids `"1"`, `"2"`, `"3"`. `expires_at` could not even be named, the lexer rejecting the underscore. The fields are withdrawn rather than left as a trap; restoring them needs a glue change and has its own story.

The handle is a lightweight descriptor; the locked memory is managed internally and keyed by id.

## Functions

### securemem.alloc(sizebytes: i32; ttlseconds: i32): SecureBuf

Allocates a locked region of `sizebytes` bytes with a TTL of `ttlseconds` seconds from the moment of allocation.

- `ttlseconds` of `0` means no expiry: the buffer lives until it is wiped or the process exits.
- The memory is zeroed immediately after allocation.
- The page is locked with `mlock()` where available; a failure to lock logs a warning and allocation continues.

### securemem.write(buf: SecureBuf; data: str): bool

Copies `data` into the buffer. Returns `true` on success; `false` if the buffer has expired, has been wiped, or `data` is longer than the buffer. The write is bounds-checked and truncated to `sizebytes - 1` so a NUL terminator fits.

### securemem.read(buf: SecureBuf): str

Returns the buffer contents. The interface declares this as `?(str)`; it yields the none sentinel when the buffer has expired, been wiped, or never existed. Reading does not reset the expiry clock.

> **Do not test the result with `==`.** Comparing the `?(str)` none sentinel with `==` segfaults the caller (story 127.83). `str.len()` is null-safe and is the probe to use until that is fixed. Returning `""` instead would be the wrong fix: it would make a wiped secret indistinguishable from an empty one.

### securemem.wipe(buf: SecureBuf): bool

Zeros the contents and releases the region immediately. Returns `true` if the buffer was found and wiped, `false` if it was already wiped or never existed. After a successful wipe, `read` returns none.

### securemem.sweep(): i32

Walks every live allocation, zeros and frees each whose TTL has elapsed, and returns how many were freed. Nothing calls this for you -- a long-running process must call it periodically or expired secrets accumulate in locked memory.

### securemem.isavailable(): bool

`true` if `mlock()` succeeds on a one-page test allocation. Use it to find out whether the swap-protection guarantee actually holds in this process before relying on it.

## A complete life cycle

```toke
m=securememdemo;
i=sm:std.securemem;
i=io:std.io;
i=str:std.str;

f=main():i64{
  if(sm.isavailable()==0){
    io.println("mlock unavailable: secrets may be paged to disk")
  };

  (* 64 bytes, five-minute TTL *)
  let buf=sm.alloc(64;300);
  let wrote=sm.write(buf;"s3cr3t-tok3n");
  io.println(str.concat("write=";str.fromint(wrote)));

  (* str.len is the null-safe probe; == on the none sentinel segfaults *)
  let got=sm.read(buf);
  if(str.len(got)>0){
    io.println(str.concat("held ";str.concat(str.fromint(str.len(got));" bytes")))
  }el{
    io.println("buffer expired or wiped")
  };

  let wiped=sm.wipe(buf);
  let after=sm.read(buf);
  io.println(str.concat("wipe=";str.concat(str.fromint(wiped);str.concat(" lenafter=";str.fromint(str.len(after))))));

  let freed=sm.sweep();
  io.println(str.concat("swept ";str.concat(str.fromint(freed);" expired buffers")));
  <0
};
```

## Platform notes

| Platform | Swap lock | Notes |
|---|---|---|
| Linux | `mlock(2)` | needs `CAP_IPC_LOCK` or `RLIMIT_MEMLOCK` headroom |
| macOS | `mlock(2)` | available without special privilege for small regions |
| Windows | `VirtualLock()` | via `<memoryapi.h>` |
| Other | none | `isavailable()` returns `false`; allocation still works |

## Security notes

- Buffer ids come from an atomic counter. They are **not** cryptographic nonces; do not expose them outside the process.
- `sweep()` is never called automatically.
- `mlock()` prevents swap. It does not stop a privileged process reading `/proc/<pid>/mem` on Linux, or a debugger attaching. For stronger isolation, combine it with OS process sandboxing.

## See Also

- `std.keychain` -- persisting a secret in the OS credential store, which is where it should come from before it reaches a `SecureBuf`.
- `std.crypto` -- the primitives you are most likely holding key material for.
