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

#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * macOS backend
 * ========================================================================= */
#ifdef __APPLE__

#include <Security/Security.h>

int keychain_is_available(void)
{
    return 1;
}

int keychain_set(const char *service, const char *account, const char *secret)
{
    if (!service || !account || !secret || secret[0] == '\0') return 0;

    /* Build the base attribute dictionary shared by add and update paths. */
    CFStringRef cf_service = CFStringCreateWithCString(
        kCFAllocatorDefault, service, kCFStringEncodingUTF8);
    CFStringRef cf_account = CFStringCreateWithCString(
        kCFAllocatorDefault, account, kCFStringEncodingUTF8);
    if (!cf_service || !cf_account) {
        if (cf_service) CFRelease(cf_service);
        if (cf_account) CFRelease(cf_account);
        return 0;
    }

    size_t secret_len = strlen(secret);
    CFDataRef cf_secret = CFDataCreate(
        kCFAllocatorDefault, (const UInt8 *)secret, (CFIndex)secret_len);
    if (!cf_secret) {
        CFRelease(cf_service);
        CFRelease(cf_account);
        return 0;
    }

    /* Query dict to locate an existing item. */
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);

    /* Attempt to update an existing item first. */
    CFMutableDictionaryRef update_attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update_attrs, kSecValueData, cf_secret);

    OSStatus status = SecItemUpdate(query, update_attrs);
    int result = 0;

    if (status == errSecItemNotFound) {
        /* No existing item — add a new one. */
        CFDictionarySetValue(query, kSecValueData, cf_secret);
        status = SecItemAdd(query, NULL);
        result = (status == errSecSuccess) ? 1 : 0;
    } else {
        result = (status == errSecSuccess) ? 1 : 0;
    }

    CFRelease(update_attrs);
    CFRelease(query);
    CFRelease(cf_secret);
    CFRelease(cf_account);
    CFRelease(cf_service);
    return result;
}

char *keychain_get(const char *service, const char *account)
{
    if (!service || !account) return NULL;

    CFStringRef cf_service = CFStringCreateWithCString(
        kCFAllocatorDefault, service, kCFStringEncodingUTF8);
    CFStringRef cf_account = CFStringCreateWithCString(
        kCFAllocatorDefault, account, kCFStringEncodingUTF8);
    if (!cf_service || !cf_account) {
        if (cf_service) CFRelease(cf_service);
        if (cf_account) CFRelease(cf_account);
        return NULL;
    }

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 5,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass,        kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService,  cf_service);
    CFDictionarySetValue(query, kSecAttrAccount,  cf_account);
    CFDictionarySetValue(query, kSecReturnData,   kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit,   kSecMatchLimitOne);

    CFTypeRef result_ref = NULL;
    OSStatus status = SecItemCopyMatching(query, &result_ref);

    CFRelease(query);
    CFRelease(cf_account);
    CFRelease(cf_service);

    if (status != errSecSuccess || !result_ref) return NULL;

    CFDataRef data = (CFDataRef)result_ref;
    CFIndex len = CFDataGetLength(data);
    if (len < 0) {
        CFRelease(result_ref);
        return NULL;
    }

    /* Heap-allocate a NUL-terminated copy for the caller. */
    char *out = malloc((size_t)len + 1);
    if (out) {
        memcpy(out, CFDataGetBytePtr(data), (size_t)len);
        out[len] = '\0';
    }
    CFRelease(result_ref);
    return out;
}

int keychain_delete(const char *service, const char *account)
{
    if (!service || !account) return 0;

    CFStringRef cf_service = CFStringCreateWithCString(
        kCFAllocatorDefault, service, kCFStringEncodingUTF8);
    CFStringRef cf_account = CFStringCreateWithCString(
        kCFAllocatorDefault, account, kCFStringEncodingUTF8);
    if (!cf_service || !cf_account) {
        if (cf_service) CFRelease(cf_service);
        if (cf_account) CFRelease(cf_account);
        return 0;
    }

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);

    OSStatus status = SecItemDelete(query);

    CFRelease(query);
    CFRelease(cf_account);
    CFRelease(cf_service);

    return (status == errSecSuccess) ? 1 : 0;
}

int keychain_exists(const char *service, const char *account)
{
    if (!service || !account) return 0;

    CFStringRef cf_service = CFStringCreateWithCString(
        kCFAllocatorDefault, service, kCFStringEncodingUTF8);
    CFStringRef cf_account = CFStringCreateWithCString(
        kCFAllocatorDefault, account, kCFStringEncodingUTF8);
    if (!cf_service || !cf_account) {
        if (cf_service) CFRelease(cf_service);
        if (cf_account) CFRelease(cf_account);
        return 0;
    }

    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass,       kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, cf_service);
    CFDictionarySetValue(query, kSecAttrAccount, cf_account);
    CFDictionarySetValue(query, kSecMatchLimit,  kSecMatchLimitOne);

    OSStatus status = SecItemCopyMatching(query, NULL);

    CFRelease(query);
    CFRelease(cf_account);
    CFRelease(cf_service);

    return (status == errSecSuccess) ? 1 : 0;
}

/* =========================================================================
 * Windows backend
 * ========================================================================= */
#elif defined(_WIN32)

#include <windows.h>
#include <wincred.h>

int keychain_is_available(void)
{
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
    if (!service || !account || !secret || secret[0] == '\0') return 0;

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
    free(target);
    return ok ? 1 : 0;
}

char *keychain_get(const char *service, const char *account)
{
    if (!service || !account) return NULL;

    char *target = build_target(service, account);
    if (!target) return NULL;

    PCREDENTIALA pcred = NULL;
    BOOL ok = CredReadA(target, CRED_TYPE_GENERIC, 0, &pcred);
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
    if (!service || !account) return 0;

    char *target = build_target(service, account);
    if (!target) return 0;

    BOOL ok = CredDeleteA(target, CRED_TYPE_GENERIC, 0);
    free(target);
    return ok ? 1 : 0;
}

int keychain_exists(const char *service, const char *account)
{
    if (!service || !account) return 0;

    char *target = build_target(service, account);
    if (!target) return 0;

    PCREDENTIALA pcred = NULL;
    BOOL ok = CredReadA(target, CRED_TYPE_GENERIC, 0, &pcred);
    free(target);

    if (ok && pcred) {
        CredFree(pcred);
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Fallback stubs — unsupported platforms
 * ========================================================================= */
#else

int keychain_is_available(void)
{
    return 0;
}

int keychain_set(const char *service, const char *account, const char *secret)
{
    (void)service; (void)account; (void)secret;
    return 0;
}

char *keychain_get(const char *service, const char *account)
{
    (void)service; (void)account;
    return NULL;
}

int keychain_delete(const char *service, const char *account)
{
    (void)service; (void)account;
    return 0;
}

int keychain_exists(const char *service, const char *account)
{
    (void)service; (void)account;
    return 0;
}

#endif /* platform dispatch */
