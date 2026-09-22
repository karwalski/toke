---
title: std.keychain
slug: keychain
section: reference/stdlib
order: 46
---

**Status: Implemented** -- C runtime backing, over the operating system's own credential store. Exercised end to end by `make test-stdlib-keychain`, which writes in one process and reads in another.

`std.keychain` stores and retrieves short secrets -- passwords, tokens, API keys -- in the OS credential store rather than in a file your program owns. On macOS the backend is Keychain Services (`Security.framework`); on Windows it is the Credential Manager (`wincred`). On every other platform there is no store, `keychain.isavailable` returns `false`, and the other four functions are no-ops.

Secrets are indexed by a **service** name (`"myapp"`) and an **account** name (`"apikey"`). Neither is treated as sensitive -- they are lookup keys and may appear in logs. Only the secret value is protected.

## The two-process property is the point

A credential store that only looks right inside one process is a cache, not a store. The value of this module is that a secret written by one run of your program is readable by the next, because it left your address space and reached the OS. `test/stdlib/keychain_roundtrip.tk` is driven by `TKKCPHASE` precisely so the harness can run the write and the read as separate processes of the same binary; a single-process round trip would pass against a hash map.

## Names, not underscores

The published call-names are `isavailable`, `set`, `get`, `delete` and `exists`. There is no `is_available`: toke's default 59-character profile excludes `_`, so a call-name containing one cannot be written in a toke program at all. Service and account strings are string literals, so underscores in *those* are fine.

## Functions

### keychain.isavailable(): bool

`true` if the current platform has a credential store this module can reach, `false` otherwise. On macOS and Windows this is `true` unless the security framework fails to load; on Linux and other POSIX systems it is `false`.

Call it first. The other four functions do not distinguish "no store on this platform" from "the write failed" -- both come back `false` -- so the availability check is what makes a failure legible.

### keychain.set(service: str; account: str; secret: str): bool

Stores or replaces the secret for the service/account pair. Returns `true` on success, `false` on failure (permission denied, keychain locked, unsupported platform). An existing entry is updated in place rather than duplicated.

### keychain.get(service: str; account: str): str

Returns the stored secret, or an empty string if no entry exists, the keychain is locked, or the platform has no store.

The interface declares this as `?(str)`. Probe the result with `str.len` rather than comparing it against a sentinel -- the same null-safety rule `std.securemem` documents for its `read`.

### keychain.delete(service: str; account: str): bool

Removes the entry. Returns `true` if an entry was deleted, `false` if there was none or the delete failed.

### keychain.exists(service: str; account: str): bool

`true` if an entry exists for the pair. Does not retrieve or expose the secret.

## A complete round trip

```toke
m=keychaindemo;
i=kc:std.keychain;
i=io:std.io;
i=str:std.str;

f=main():i64{
  if(kc.isavailable()==0){
    io.println("no OS credential store on this platform");
    <0
  };

  let wrote=kc.set("tokedemo";"apikey";"s3cr3t-value");
  io.println(str.concat("set=";str.fromint(wrote)));

  let present=kc.exists("tokedemo";"apikey");
  io.println(str.concat("exists=";str.fromint(present)));

  let got=kc.get("tokedemo";"apikey");
  io.println(str.concat("length=";str.fromint(str.len(got))));

  let removed=kc.delete("tokedemo";"apikey");
  let after=kc.exists("tokedemo";"apikey");
  io.println(str.concat("deleted=";str.concat(str.fromint(removed);str.concat(" exists=";str.fromint(after)))));
  <0
};
```

Note what the example prints: the length of the secret, never the secret. A documented example gets copied, and one that prints a credential teaches printing credentials.

## Platform notes

| Platform | Backend | `isavailable()` |
|---|---|---|
| macOS | `Security.framework` (`SecItemAdd`, `SecItemCopyMatching`, `SecItemUpdate`, `SecItemDelete`) | `true` |
| Windows | `wincred.h` (`CredWriteA`, `CredReadA`, `CredDeleteA`) | `true` |
| Other | none | `false` |

## Security notes

- This module never writes a secret value to a log, an error message or standard output. Your program can, so do not.
- `service` and `account` are non-sensitive identifiers by design; do not encode anything secret in them.
- A locked keychain is indistinguishable from an absent entry: `get` returns empty and `set` returns `false`. If that distinction matters to your program, you need a platform call this module does not wrap.

## See Also

- `std.securemem` -- holding a secret in RAM once you have fetched it, locked out of swap and wiped on release.
- `std.env` -- the usual alternative, and the one to move away from: environment variables are readable from the process table and inherited by every child.
