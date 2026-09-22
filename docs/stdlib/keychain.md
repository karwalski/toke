# std.keychain -- OS Keychain / Credential Store

## Overview

The `std.keychain` module provides secure storage and retrieval of secrets (passwords, tokens, API keys) using the operating system's native credential store. On macOS the backend is the Keychain Services API (`Security.framework`). On Windows the backend is the Windows Credential Manager (`wincred`). On unsupported platforms all operations are no-ops and `keychain.is_available()` returns `false`.

Secrets are indexed by a **service** name (e.g. `"myapp"`) and an **account** name (e.g. `"api_key"`). Neither service nor account names are treated as sensitive; only the **secret** value is protected by the OS credential store.

The module never logs or exposes secret values. If the keychain is locked or otherwise unavailable, `keychain.get()` returns `?(none)` rather than crashing.

## Functions

### keychain.is_available(): bool

Returns `true` if the OS credential store is available on the current platform, `false` otherwise.

On macOS and Windows this always returns `true` at runtime unless the security framework itself fails to load. On Linux and other POSIX systems this returns `false`.

**Example:**
```toke
if keychain.is_available() {
    (* safe to use keychain operations *)
}
```

### keychain.set(service: str; account: str; secret: str): bool

Stores or updates the secret for the given service/account pair. Returns `true` on success, `false` if the write failed (e.g. permission denied, keychain locked).

If an entry for the service/account pair already exists it is updated in place. The secret must be a valid UTF-8 string and must not be empty.

**Example:**
```toke
let ok = keychain.set("myapp"; "db_password"; "s3cr3t");
```

### keychain.get(service: str; account: str): ?(str)

Retrieves the secret for the given service/account pair. Returns `?(none)` if no matching entry exists, the keychain is locked, or the platform is unsupported.

The returned string is a heap-allocated copy; the caller owns it.

**Example:**
```toke
let pw = keychain.get("myapp"; "db_password");
match pw {
    ?(some s) -> (* use s *)
    ?(none)   -> (* not found or unavailable *)
}
```

### keychain.delete(service: str; account: str): bool

Removes the entry for the given service/account pair from the credential store. Returns `true` if the entry was deleted, `false` if it did not exist or the delete failed.

**Example:**
```toke
let removed = keychain.delete("myapp"; "db_password");
```

### keychain.exists(service: str; account: str): bool

Returns `true` if an entry for the given service/account pair exists in the credential store, `false` otherwise. Does not retrieve or expose the secret value.

**Example:**
```toke
if keychain.exists("myapp"; "api_key") {
    (* entry is present, safe to call keychain.get *)
}
```

## Platform Notes

| Platform | Backend | `is_available()` |
|----------|---------|-----------------|
| macOS    | Security.framework (`SecItemAdd`, `SecItemCopyMatching`, `SecItemUpdate`, `SecItemDelete`) | `true` |
| Windows  | `wincred.h` (`CredWriteA`, `CredReadA`, `CredDeleteA`) | `true` |
| Other    | No-op stub | `false` |

## Security Notes

- Secret values are never written to logs, error messages, or standard output by this module.
- The `service` and `account` parameters are treated as non-sensitive identifiers.
- If the OS keychain is locked (e.g. user has not yet authenticated), `keychain.get()` returns `?(none)` and `keychain.set()` returns `false`.
