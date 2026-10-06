/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_KEYCHAIN_H
#define TK_STDLIB_KEYCHAIN_H

/*
 * keychain.h — C interface for the std.keychain standard library module.
 *
 * Provides secure secret storage backed by the OS credential store:
 *   - macOS  : Security.framework (SecItem* APIs)
 *   - Windows: wincred.h (Cred* APIs)
 *   - Other  : no-op stubs; keychain_is_available() returns 0
 *
 * Type mappings:
 *   str  = const char *  (null-terminated UTF-8)
 *   bool = int           (0 = false, 1 = true)
 *   ?(str) = char *      (heap-allocated; NULL represents ?(none))
 *
 * Secret values are NEVER logged or exposed in error paths.
 * Callers own any non-NULL char * returned by keychain_get().
 *
 * Story: 72.1
 */

/*
 * keychain_is_available(): bool
 * Returns 1 if the OS credential store is usable on this platform, 0 otherwise.
 */
int   keychain_is_available(void);

/*
 * keychain_set(service, account, secret): bool
 * Stores or updates the secret for the given service/account pair.
 * Returns 1 on success, 0 on failure (locked keychain, permission denied, etc.).
 * secret must be a non-NULL, non-empty UTF-8 string.
 */
int   keychain_set(const char *service, const char *account, const char *secret);

/*
 * keychain_get(service, account): ?(str)
 * Retrieves the secret for the given service/account pair.
 * Returns a heap-allocated copy of the secret on success (caller must free).
 * Returns NULL if no matching entry exists, the keychain is locked,
 * or the platform is unsupported -- keychain_last_kind() says which (138.4).
 */
char *keychain_get(const char *service, const char *account);

/*
 * keychain_delete(service, account): bool
 * Removes the entry for the given service/account pair.
 * Returns 1 if the entry was deleted, 0 if it did not exist or deletion failed.
 */
int   keychain_delete(const char *service, const char *account);

/*
 * keychain_exists(service, account): bool
 * Returns 1 if an entry for the given service/account pair exists, 0 otherwise.
 * Does not retrieve or expose the secret value.
 */
int   keychain_exists(const char *service, const char *account);

/*
 * 138.4 — what the calling thread's last keychain call did.
 *   keychain_last_status : the platform status (macOS OSStatus, Windows
 *                          GetLastError, -4 where there is no store); 0 = ok
 *   keychain_last_kind   : "" on success, else one of notfound locked denied
 *                          nokeychain badarg unavailable failed.  After a
 *                          false / NULL result, "notfound" means the item is
 *                          absent; any other kind means the call failed.
 *   keychain_last_message: a description; never contains a secret.
 */
long        keychain_last_status(void);
const char *keychain_last_kind(void);
const char *keychain_last_message(void);

/*
 * 138.4 — named keychains, so tests (and programs that want isolation) never
 * touch the login keychain.  macOS only; elsewhere they fail "unavailable".
 *   keychain_create(path, password): make a new keychain file at the absolute
 *     path, unlocked, NOT added to the user's search list.  Fails if it exists.
 *   keychain_use(path): every later set/get/delete/exists goes to that
 *     keychain alone, with user interaction disallowed (a locked keychain
 *     fails "locked" rather than prompting).  "" returns to the default.
 *   keychain_destroy(path): delete that keychain file.  Refuses the default
 *     keychain and any keychain on the user's search list ("denied").
 */
int keychain_create(const char *path, const char *password);
int keychain_use(const char *path);
int keychain_destroy(const char *path);

#endif /* TK_STDLIB_KEYCHAIN_H */
