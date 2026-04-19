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
 * or the platform is unsupported.
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

#endif /* TK_STDLIB_KEYCHAIN_H */
