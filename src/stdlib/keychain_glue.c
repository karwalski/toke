/* SPDX-License-Identifier: MIT */
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
    char *secret = keychain_get(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
    return secret ? (int64_t)(intptr_t)secret : 0;
}

/* keychain.set(service, account, secret) — store secret */
int64_t tk_keychain_set_w(int64_t service, int64_t account, int64_t secret) {
    return (int64_t)keychain_set(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account,
        (const char *)(intptr_t)secret);
}

/* keychain.delete(service, account) — remove entry */
int64_t tk_keychain_delete_w(int64_t service, int64_t account) {
    return (int64_t)keychain_delete(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
}

/* keychain.exists(service, account) — check if entry exists */
int64_t tk_keychain_exists_w(int64_t service, int64_t account) {
    return (int64_t)keychain_exists(
        (const char *)(intptr_t)service,
        (const char *)(intptr_t)account);
}

/* keychain.isavailable() — is the OS credential store usable here?
 *
 * 136.3: the `.tki` has always exported `keychain.isavailable`, but no `_w`
 * symbol existed, so any consumer that called it failed to link with an
 * undefined `_tk_keychain_isavailable_w`.  The C core (keychain_is_available)
 * has been present since 72.1.2; only the wrapper was missing.  The name
 * differs from the core's because Profile-1 call-names carry no underscore
 * (113.2a), so the wrapper cannot simply be a rename.
 */
int64_t tk_keychain_isavailable_w(void) {
    return (int64_t)keychain_is_available();
}

/* ── 138.4: the error channel ──────────────────────────────────────────────
 * What the calling thread's last keychain call did (keychain.h).  get /
 * exists / delete keep their types; a caller that got an empty / false
 * answer asks keychain.lasterrkind() whether it means "absent" ("notfound")
 * or "the query failed" (anything else non-empty). */
int64_t tk_keychain_laststatus_w(void) {
    return (int64_t)keychain_last_status();
}

int64_t tk_keychain_lasterrkind_w(void) {
    const char *k = keychain_last_kind();
    return (int64_t)(intptr_t)(k ? k : "");
}

int64_t tk_keychain_lasterr_w(void) {
    const char *m = keychain_last_message();
    return (int64_t)(intptr_t)(m ? m : "");
}

/* ── 138.4: named (throwaway) keychains ─────────────────────────────────── */
int64_t tk_keychain_create_w(int64_t path, int64_t password) {
    return (int64_t)keychain_create((const char *)(intptr_t)path,
                                    (const char *)(intptr_t)password);
}

int64_t tk_keychain_use_w(int64_t path) {
    return (int64_t)keychain_use((const char *)(intptr_t)path);
}

int64_t tk_keychain_destroy_w(int64_t path) {
    return (int64_t)keychain_destroy((const char *)(intptr_t)path);
}
