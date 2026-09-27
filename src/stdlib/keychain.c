/*
 * keychain.c — Implementation of the std.keychain standard library module.
 *
 * Platform dispatch:
 *   __APPLE__  : Security.framework (SecItemAdd / SecItemCopyMatching /
 *                SecItemUpdate / SecItemDelete)
 *   _WIN32     : Windows Credential Manager (CredWriteA / CredReadA /
 *                CredDeleteA / CredFree)
 *   (other)    : no-op stubs; keychain_is_available() returns 0
 *
 * Secret values are never written to logs, error paths, or any output.
 * All functions return gracefully (0 / NULL) rather than crashing when
 * the keychain is locked or unavailable.
 *
 * Story: 72.1
 */

#include "keychain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 138.4 — the error channel, shared by every backend.
 *
 * THE DEFECT.  get/exists/delete mapped every non-success status to
 * NULL / 0, so a locked or denied keychain read exactly like "no such item"
 * and a caller's first-use branch ran against a store it could not read
 * (loke X8.3, R4).  Each call now records its outcome for the calling thread
 * before it returns: the platform status (0 = success), a kind to branch on,
 * and a message.  After a false / empty result, kind "notfound" means the
 * item is absent; any other non-empty kind means the query failed and says
 * nothing about whether the item exists.
 * ========================================================================= */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define KC_TLS _Thread_local
#else
#  define KC_TLS __thread
#endif
static KC_TLS long        g_status;
static KC_TLS const char *g_kind = "";
static KC_TLS char        g_msg[256];

static void kc_ok(void) { g_status = 0; g_kind = ""; g_msg[0] = '\0'; }
static void kc_fail(long status, const char *kind, const char *msg)
{
    g_status = status;
    g_kind = kind;
    snprintf(g_msg, sizeof g_msg, "%s (status %ld)", msg ? msg : kind, status);
}

long        keychain_last_status(void)   { return g_status; }
const char *keychain_last_kind(void)     { return g_kind; }
const char *keychain_last_message(void)  { return g_msg; }

/* =========================================================================
 * macOS backend
 * ========================================================================= */
#ifdef __APPLE__

/* SecKeychainCreate/Open/Delete/Lock and the search-list calls are the only
 * way to name a keychain file on macOS; Apple marks them deprecated (the
 * data-protection keychain has no files) but they are the file-based
 * keychain's API and remain supported for it. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>

static void kc_status(OSStatus st)
{
    if (st == errSecSuccess) { kc_ok(); return; }
    const char *kind;
    switch (st) {
    case errSecItemNotFound:          kind = "notfound";   break;
    case errSecInteractionNotAllowed:
    case errSecAuthFailed:            kind = "locked";     break;
    case errSecUserCanceled:
    case errSecMissingEntitlement:
    case errSecWrPerm:                kind = "denied";     break;
    case errSecNoSuchKeychain:
    case errSecNotAvailable:
    case errSecNoDefaultKeychain:     kind = "nokeychain"; break;
    case errSecParam:                 kind = "badarg";     break;
    default:                          kind = "failed";     break;
    }
    char text[200] = "";
    CFStringRef m = SecCopyErrorMessageString(st, NULL);
    if (m) {
        if (!CFStringGetCString(m, text, sizeof text, kCFStringEncodingUTF8)) text[0] = '\0';
        CFRelease(m);
    }
    kc_fail((long)st, kind, text[0] ? text : kind);
}

/* The keychain keychain.use() selected, or NULL for the user's default
 * search list.  Process-wide, like the search list it stands in for. */
static SecKeychainRef g_kc = NULL;

/* While a named keychain is selected every call runs with user interaction
 * disallowed, so a locked throwaway keychain fails with "locked" instead of
 * raising an unlock dialog. */
typedef struct { Boolean saved; int active; } KcUi;
static KcUi kc_ui_begin(void)
{
    KcUi u = { 0, 0 };
    if (g_kc && SecKeychainGetUserInteractionAllowed(&u.saved) == errSecSuccess) {
        SecKeychainSetUserInteractionAllowed(false);
        u.active = 1;
    }
    return u;
}
static void kc_ui_end(KcUi u)
{
    if (u.active) SecKeychainSetUserInteractionAllowed(u.saved);
}

/* A query dictionary for service/account, scoped to the selected keychain
 * when there is one.  NULL (and the status recorded) on bad input. */
static CFMutableDictionaryRef kc_query(const char *service, const char *account)
{
    if (!service || !account) { kc_fail(errSecParam, "badarg", "null service or account"); return NULL; }
    CFStringRef cf_service = CFStringCreateWithCString(kCFAllocatorDefault, service, kCFStringEncodingUTF8);
    CFStringRef cf_account = CFStringCreateWithCString(kCFAllocatorDefault, account, kCFStringEncodingUTF8);
    if (!cf_service || !cf_account) {
        if (cf_service) CFRelease(cf_service);
        if (cf_account) CFRelease(cf_account);
        kc_fail(errSecParam, "badarg", "service or account is not valid UTF-8");
        return NULL;
    }
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, cf_service);
    CFDictionarySetValue(q, kSecAttrAccount, cf_account);
    CFRelease(cf_service);
    CFRelease(cf_account);
    if (g_kc) {
        CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, (const void **)&g_kc, 1, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(q, kSecMatchSearchList, list);
        CFRelease(list);
    }
    return q;
}

int keychain_is_available(void)
{
    kc_ok();
    return 1;
}

int keychain_set(const char *service, const char *account, const char *secret)
{
    if (!secret || secret[0] == '\0') { kc_fail(errSecParam, "badarg", "empty secret"); return 0; }
    CFMutableDictionaryRef query = kc_query(service, account);
    if (!query) return 0;
    CFDataRef cf_secret = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)secret, (CFIndex)strlen(secret));
    CFMutableDictionaryRef update_attrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update_attrs, kSecValueData, cf_secret);

    KcUi ui = kc_ui_begin();
    /* Attempt to update an existing item first. */
    OSStatus status = SecItemUpdate(query, update_attrs);
    if (status == errSecItemNotFound) {
        /* No existing item — add a new one, into the selected keychain if
         * there is one (an add takes kSecUseKeychain, not a search list). */
        CFDictionaryRemoveValue(query, kSecMatchSearchList);
        if (g_kc) CFDictionarySetValue(query, kSecUseKeychain, g_kc);
        CFDictionarySetValue(query, kSecValueData, cf_secret);
        status = SecItemAdd(query, NULL);
    }
    kc_ui_end(ui);
    kc_status(status);

    CFRelease(update_attrs);
    CFRelease(query);
    CFRelease(cf_secret);
    return status == errSecSuccess ? 1 : 0;
}

char *keychain_get(const char *service, const char *account)
{
    CFMutableDictionaryRef query = kc_query(service, account);
    if (!query) return NULL;
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result_ref = NULL;
    KcUi ui = kc_ui_begin();
    OSStatus status = SecItemCopyMatching(query, &result_ref);
    kc_ui_end(ui);
    CFRelease(query);
    kc_status(status);
    if (status != errSecSuccess) return NULL;
    if (!result_ref || CFGetTypeID(result_ref) != CFDataGetTypeID()) {
        if (result_ref) CFRelease(result_ref);
        kc_fail(errSecInternalComponent, "failed", "the item has no data");
        return NULL;
    }

    CFDataRef data = (CFDataRef)result_ref;
    CFIndex len = CFDataGetLength(data);
    /* Heap-allocate a NUL-terminated copy for the caller. */
    char *out = malloc((size_t)len + 1);
    if (out) {
        memcpy(out, CFDataGetBytePtr(data), (size_t)len);
        out[len] = '\0';
    } else {
        kc_fail(errSecAllocate, "failed", "out of memory");
    }
    CFRelease(result_ref);
    return out;
}

int keychain_delete(const char *service, const char *account)
{
    CFMutableDictionaryRef query = kc_query(service, account);
    if (!query) return 0;
    KcUi ui = kc_ui_begin();
    OSStatus status = SecItemDelete(query);
    kc_ui_end(ui);
    CFRelease(query);
    kc_status(status);
    return status == errSecSuccess ? 1 : 0;
}

int keychain_exists(const char *service, const char *account)
{
    CFMutableDictionaryRef query = kc_query(service, account);
    if (!query) return 0;
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    KcUi ui = kc_ui_begin();
    OSStatus status = SecItemCopyMatching(query, NULL);
    kc_ui_end(ui);
    CFRelease(query);
    kc_status(status);
    return status == errSecSuccess ? 1 : 0;
}

/* ── 138.4: named (throwaway) keychains ─────────────────────────────────── */

/* Would deleting this keychain delete one the user relies on?  Refuses the
 * default keychain and every keychain on the user's search list (the login
 * and System keychains among them): keychain.destroy is for keychains a
 * program made with keychain.create. */
static int kc_is_protected(SecKeychainRef kc)
{
    char want[1024]; UInt32 wl = sizeof want;
    if (SecKeychainGetPath(kc, &wl, want) != errSecSuccess) return 1;
    want[wl < sizeof want ? wl : sizeof want - 1] = '\0';
    int hit = 0;
    SecKeychainRef def = NULL;
    CFArrayRef list = NULL;
    if (SecKeychainCopyDefault(&def) == errSecSuccess && def) {
        char p[1024]; UInt32 pl = sizeof p;
        if (SecKeychainGetPath(def, &pl, p) == errSecSuccess) {
            p[pl < sizeof p ? pl : sizeof p - 1] = '\0';
            if (!strcmp(p, want)) hit = 1;
        }
        CFRelease(def);
    }
    if (!hit && SecKeychainCopySearchList(&list) == errSecSuccess && list) {
        for (CFIndex i = 0; i < CFArrayGetCount(list) && !hit; i++) {
            SecKeychainRef k = (SecKeychainRef)CFArrayGetValueAtIndex(list, i);
            char p[1024]; UInt32 pl = sizeof p;
            if (SecKeychainGetPath(k, &pl, p) == errSecSuccess) {
                p[pl < sizeof p ? pl : sizeof p - 1] = '\0';
                if (!strcmp(p, want)) hit = 1;
            }
        }
        CFRelease(list);
    }
    return hit;
}

int keychain_create(const char *path, const char *password)
{
    if (!path || !path[0] || !password) { kc_fail(errSecParam, "badarg", "empty path or null password"); return 0; }
    if (path[0] != '/') { kc_fail(errSecParam, "badarg", "keychain path must be absolute"); return 0; }
    SecKeychainRef kc = NULL;
    /* promptUser=false: the password is given; no dialog.  A keychain made
     * this way is NOT added to the user's search list, so nothing outside
     * this program sees it. */
    OSStatus st = SecKeychainCreate(path, (UInt32)strlen(password), password, false, NULL, &kc);
    kc_status(st);
    if (kc) CFRelease(kc);
    return st == errSecSuccess ? 1 : 0;
}

static SecKeychainRef kc_open_existing(const char *path)
{
    SecKeychainRef kc = NULL;
    OSStatus st = SecKeychainOpen(path, &kc);
    if (st == errSecSuccess) {
        /* SecKeychainOpen succeeds for any path; the status call is what
         * says whether a keychain is there. */
        SecKeychainStatus ks = 0;
        st = SecKeychainGetStatus(kc, &ks);
    }
    if (st != errSecSuccess) {
        if (kc) CFRelease(kc);
        kc_status(st);
        return NULL;
    }
    return kc;
}

int keychain_use(const char *path)
{
    if (!path) { kc_fail(errSecParam, "badarg", "null path"); return 0; }
    if (!path[0]) {
        if (g_kc) { CFRelease(g_kc); g_kc = NULL; }
        kc_ok();
        return 1;
    }
    SecKeychainRef kc = kc_open_existing(path);
    if (!kc) return 0;
    if (g_kc) CFRelease(g_kc);
    g_kc = kc;
    kc_ok();
    return 1;
}

int keychain_destroy(const char *path)
{
    if (!path || !path[0]) { kc_fail(errSecParam, "badarg", "empty path"); return 0; }
    SecKeychainRef kc = kc_open_existing(path);
    if (!kc) return 0;
    if (kc_is_protected(kc)) {
        CFRelease(kc);
        kc_fail(errSecWrPerm, "denied",
                "refusing to delete the default keychain or one on the user's search list");
        return 0;
    }
    if (g_kc) {
        char a[1024], b[1024]; UInt32 al = sizeof a, bl = sizeof b;
        if (SecKeychainGetPath(g_kc, &al, a) == errSecSuccess &&
            SecKeychainGetPath(kc, &bl, b) == errSecSuccess &&
            al == bl && !memcmp(a, b, al)) { CFRelease(g_kc); g_kc = NULL; }
    }
    OSStatus st = SecKeychainDelete(kc);
    CFRelease(kc);
    kc_status(st);
    return st == errSecSuccess ? 1 : 0;
}

/* =========================================================================
 * Windows backend
 * ========================================================================= */
#elif defined(_WIN32)

#include <windows.h>
#include <wincred.h>

/* 138.4: GetLastError() is the status; ERROR_NOT_FOUND is "notfound". */
static int kc_win(BOOL ok)
{
    if (ok) { kc_ok(); return 1; }
    DWORD e = GetLastError();
    kc_fail((long)e, e == ERROR_NOT_FOUND ? "notfound"
                   : e == ERROR_ACCESS_DENIED ? "denied" : "failed",
            e == ERROR_NOT_FOUND ? "no such credential" : "credential manager call failed");
    return 0;
}

int keychain_is_available(void)
{
    kc_ok();
    return 1;
}

/*
 * Build a credential target name from service + account.
 * Format: "<service>/<account>"
 * Caller must free the returned buffer.
 */
static char *build_target(const char *service, const char *account)
{
    size_t slen = strlen(service);
    size_t alen = strlen(account);
    /* service + '/' + account + NUL */
    char *target = malloc(slen + 1 + alen + 1);
    if (!target) return NULL;
    memcpy(target, service, slen);
    target[slen] = '/';
    memcpy(target + slen + 1, account, alen);
    target[slen + 1 + alen] = '\0';
    return target;
}

int keychain_set(const char *service, const char *account, const char *secret)
{
    if (!service || !account || !secret || secret[0] == '\0') { kc_fail(87, "badarg", "null argument or empty secret"); return 0; }

    char *target = build_target(service, account);
    if (!target) return 0;

    CREDENTIALA cred;
    memset(&cred, 0, sizeof(cred));
    cred.Type              = CRED_TYPE_GENERIC;
    cred.TargetName        = target;
    cred.CredentialBlob    = (LPBYTE)secret;
    cred.CredentialBlobSize = (DWORD)strlen(secret);
    cred.Persist           = CRED_PERSIST_LOCAL_MACHINE;

    BOOL ok = CredWriteA(&cred, 0);
    kc_win(ok);
    free(target);
    return ok ? 1 : 0;
}

char *keychain_get(const char *service, const char *account)
{
    if (!service || !account) { kc_fail(87, "badarg", "null service or account"); return NULL; }

    char *target = build_target(service, account);
    if (!target) return NULL;

    PCREDENTIALA pcred = NULL;
    BOOL ok = CredReadA(target, CRED_TYPE_GENERIC, 0, &pcred);
    kc_win(ok && pcred);
    free(target);

    if (!ok || !pcred) return NULL;

    DWORD blob_size = pcred->CredentialBlobSize;
    char *out = malloc((size_t)blob_size + 1);
    if (out) {
        memcpy(out, pcred->CredentialBlob, blob_size);
        out[blob_size] = '\0';
    }
    CredFree(pcred);
    return out;
}

int keychain_delete(const char *service, const char *account)
{
    if (!service || !account) { kc_fail(87, "badarg", "null service or account"); return 0; }

    char *target = build_target(service, account);
    if (!target) return 0;

    BOOL ok = CredDeleteA(target, CRED_TYPE_GENERIC, 0);
    kc_win(ok);
    free(target);
    return ok ? 1 : 0;
}

int keychain_exists(const char *service, const char *account)
{
    if (!service || !account) { kc_fail(87, "badarg", "null service or account"); return 0; }

    char *target = build_target(service, account);
    if (!target) return 0;

    PCREDENTIALA pcred = NULL;
    BOOL ok = CredReadA(target, CRED_TYPE_GENERIC, 0, &pcred);
    kc_win(ok && pcred);
    free(target);

    if (ok && pcred) {
        CredFree(pcred);
        return 1;
    }
    return 0;
}

/* 138.4: the Credential Manager has no named stores to create or select. */
int keychain_create(const char *path, const char *password)
{
    (void)path; (void)password;
    kc_fail(50, "unavailable", "named keychains are not supported by the Windows Credential Manager");
    return 0;
}
int keychain_use(const char *path)
{
    if (path && !path[0]) { kc_ok(); return 1; }
    kc_fail(50, "unavailable", "named keychains are not supported by the Windows Credential Manager");
    return 0;
}
int keychain_destroy(const char *path)
{
    (void)path;
    kc_fail(50, "unavailable", "named keychains are not supported by the Windows Credential Manager");
    return 0;
}

/* =========================================================================
 * Fallback stubs — unsupported platforms.  138.4: every call reports kind
 * "unavailable" (status -4, errSecUnimplemented's value), so a false / empty
 * result is never mistaken for "no such item".
 * ========================================================================= */
#else

#define KC_UNAVAIL() kc_fail(-4, "unavailable", "no OS credential store on this platform")

int keychain_is_available(void)
{
    kc_ok();
    return 0;
}

int keychain_set(const char *service, const char *account, const char *secret)
{
    (void)service; (void)account; (void)secret;
    KC_UNAVAIL();
    return 0;
}

char *keychain_get(const char *service, const char *account)
{
    (void)service; (void)account;
    KC_UNAVAIL();
    return NULL;
}

int keychain_delete(const char *service, const char *account)
{
    (void)service; (void)account;
    KC_UNAVAIL();
    return 0;
}

int keychain_exists(const char *service, const char *account)
{
    (void)service; (void)account;
    KC_UNAVAIL();
    return 0;
}

int keychain_create(const char *path, const char *password)
{
    (void)path; (void)password;
    KC_UNAVAIL();
    return 0;
}

int keychain_use(const char *path)
{
    (void)path;
    KC_UNAVAIL();
    return 0;
}

int keychain_destroy(const char *path)
{
    (void)path;
    KC_UNAVAIL();
    return 0;
}

#endif /* platform dispatch */
