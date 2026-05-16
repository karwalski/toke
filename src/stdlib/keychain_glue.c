/*
 * keychain_glue.c — i64-ABI wrappers for std.keychain module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.keychain.
 */

#include "keychain.h"
#include <stdint.h>

/* keychain.get(service, account) — retrieve secret string */
int64_t tk_keychain_get_w(int64_t service, int64_t account) {
    if (!service || !account) return 0;
    char *secret = keychain_get(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
    return secret ? (int64_t)(intptr_t)secret : 0;
}

/* keychain.set(service, account, secret) — store secret */
int64_t tk_keychain_set_w(int64_t service, int64_t account, int64_t secret) {
    if (!service || !account || !secret) return 0;
    return (int64_t)keychain_set(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account,
        (const char *)(intptr_t)secret);
}

/* keychain.delete(service, account) — remove entry */
int64_t tk_keychain_delete_w(int64_t service, int64_t account) {
    if (!service || !account) return 0;
    return (int64_t)keychain_delete(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
}

/* keychain.exists(service, account) — check if entry exists */
int64_t tk_keychain_exists_w(int64_t service, int64_t account) {
    if (!service || !account) return 0;
    return (int64_t)keychain_exists(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
}
