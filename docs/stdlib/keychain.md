---
title: std.keychain
slug: keychain
section: reference/stdlib
order: 46
---

**Status: Implemented** -- C runtime backing, over the operating system's own credential store. Exercised end to end by `make test-stdlib-keychain`, which writes in one process and reads in another.

`std.keychain` stores and retrieves short secrets -- passwords, tokens, API keys -- in the OS credential store rather than in a file your program owns. On macOS the backend is Keychain Services (`Security.framework`); on Windows it is the Credential Manager (`wincred`). On every other platform there is no store, `keychain.isavailable` returns `false`, and every other function fails with `keychain.lasterrkind()` = `"unavailable"`.

Secrets are indexed by a **service** name (`"myapp"`) and an **account** name (`"apikey"`). Neither is treated as sensitive -- they are lookup keys and may appear in logs. Only the secret value is protected.

## The two-process property is the point

A credential store that only looks right inside one process is a cache, not a store. The value of this module is that a secret written by one run of your program is readable by the next, because it left your address space and reached the OS. `test/stdlib/keychain_roundtrip.tk` is driven by `TKKCPHASE` precisely so the harness can run the write and the read as separate processes of the same binary; a single-process round trip would pass against a hash map.

## Names, not underscores

The published call-names are `isavailable`, `set`, `get`, `delete` and `exists`. There is no `is_available`: toke's default 59-character profile excludes `_`, so a call-name containing one cannot be written in a toke program at all. Service and account strings are string literals, so underscores in *those* are fine.

## Functions

### keychain.isavailable(): bool

`true` if the current platform has a credential store this module can reach, `false` otherwise. On macOS and Windows this is `true` unless the security framework fails to load; on Linux and other POSIX systems it is `false`.

Call it first, then read `keychain.lasterrkind()` after any `false` or empty result -- see [Telling "absent" from "failed"](#telling-absent-from-failed).

### keychain.set(service: str; account: str; secret: str): bool

Stores or replaces the secret for the service/account pair. Returns `true` on success, `false` on failure (permission denied, keychain locked, unsupported platform). An existing entry is updated in place rather than duplicated.

### keychain.get(service: str; account: str): ?(str)

Returns the stored secret, or none if there is no entry **or the read failed** (locked keychain, denied access, no store). `keychain.lasterrkind()` says which: `"notfound"` means there is no entry; anything else means the read failed and says nothing about whether one exists.

The interface declares this as `?(str)`. Probe the result with `str.len` rather than comparing it against a sentinel -- the same null-safety rule `std.securemem` documents for its `read`.

### keychain.delete(service: str; account: str): bool

Removes the entry. Returns `true` if an entry was deleted, `false` if there was none (`lasterrkind()` = `"notfound"`) or the delete failed (any other kind).

### keychain.exists(service: str; account: str): bool

`true` if an entry exists for the pair. Does not retrieve or expose the secret. `false` with `lasterrkind()` = `"notfound"` means there is none; `false` with any other kind means the query failed. On macOS the item attributes of a *locked* file keychain stay readable, so `exists` answers truthfully there even though `get` fails `"locked"`.

### keychain.lasterrkind(): str

What the calling thread's most recent keychain call did: `""` on success, otherwise one of

| Kind | Meaning |
|---|---|
| `notfound` | there is no such item -- the only kind that means "absent" |
| `locked` | the keychain is locked and could not be unlocked without a prompt (or the password was wrong) |
| `denied` | access was refused (user cancelled, missing entitlement, write-protected); also `destroy` on a protected keychain |
| `nokeychain` | the named keychain does not exist, or there is no default keychain |
| `badarg` | a null/empty argument, an empty secret, a relative keychain path |
| `unavailable` | this platform has no credential store (or no named keychains) |
| `failed` | anything else -- read `keychain.lasterr()` and `keychain.laststatus()` |

(138.4.) Branch on this, never on the message.

### keychain.laststatus(): i64

The platform status of that call: on macOS the `OSStatus` (`0` success, `-25300` `errSecItemNotFound`, `-25308` `errSecInteractionNotAllowed`, ...), on Windows `GetLastError()`, and `-4` where there is no store.

### keychain.lasterr(): str

A description of that call's failure, `""` after a success. It never contains a secret; its wording is not an interface.

### keychain.create(path: str; password: str): bool

Creates a new, unlocked keychain file at the **absolute** `path`, protected by `password`. It is not added to the user's keychain search list, so nothing outside your program sees it, and it is not selected -- call `keychain.use`. Fails if the file exists. macOS only; elsewhere `false` with kind `"unavailable"`. (138.4.)

### keychain.use(path: str): bool

Every later `set`/`get`/`delete`/`exists` in this process goes to the keychain at `path` alone, with user interaction disallowed: a locked keychain fails `"locked"` instead of raising an unlock dialog. `keychain.use("")` returns to the user's default keychain. Fails with `"nokeychain"` if there is no keychain at `path`. This is how tests stay out of the login keychain:

```toke
m=kctest;
i=kc:std.keychain;
i=io:std.io;

f=main():i64{
  let p="/tmp/kctest.keychain";
  if(kc.create(p;"throwaway")==false){
    io.println(kc.lasterr());
    <1
  };
  let u=kc.use(p);
  let s=kc.set("kctest";"acct";"value");
  let e=kc.exists("kctest";"other");
  let k=kc.lasterrkind();
  let d=kc.destroy(p);
  io.println(k);
  <0
};
```

### keychain.destroy(path: str): bool

Deletes the keychain file at `path` (and deselects it if it was in use). Refuses -- `false`, kind `"denied"` -- the user's default keychain and any keychain on the user's search list, the login and System keychains among them: it is for keychains your program made with `keychain.create`.

## Telling "absent" from "failed"

```toke
m=kcfirst;
i=kc:std.keychain;
i=io:std.io;
i=str:std.str;

f=main():i64{
  let key=kc.get("myapp";"dbkey");
  if(str.len(key)>0){
    io.println("have a key");
    <0
  };
  if(kc.lasterrkind()=="notfound"){
    io.println("first use: create a key");
    <0
  };
  io.println(str.concat("keychain unreadable: ";kc.lasterr()));
  <1
};
```

## A complete round trip

```toke
m=keychaindemo;
i=kc:std.keychain;
i=io:std.io;
i=str:std.str;

f=main():i64{
  if(!kc.isavailable()){
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
| macOS | `Security.framework` (`SecItemAdd`, `SecItemCopyMatching`, `SecItemUpdate`, `SecItemDelete`; named keychains via `SecKeychainCreate`/`Open`/`Delete`) | `true` |
| Windows | `wincred.h` (`CredWriteA`, `CredReadA`, `CredDeleteA`) | `true` |
| Other | none | `false` |

## Security notes

- This module never writes a secret value to a log, an error message or standard output. Your program can, so do not.
- `service` and `account` are non-sensitive identifiers by design; do not encode anything secret in them.
- A locked keychain is **not** an absent entry (138.4): `get` fails with `lasterrkind()` = `"locked"`, never `"notfound"`. Before 138.4 the two were indistinguishable, and a program's first-use branch could run against a store it simply could not read.

## See Also

- `std.securemem` -- holding a secret in RAM once you have fetched it, locked out of swap and wiped on release.
- `std.env` -- the usual alternative, and the one to move away from: environment variables are readable from the process table and inherited by every child.
