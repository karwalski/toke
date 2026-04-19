#ifndef TK_STDLIB_MDNS_H
#define TK_STDLIB_MDNS_H

/*
 * mdns.h — C interface for the std.mdns standard library module.
 *
 * Provides mDNS/Bonjour service advertisement and discovery.
 *
 * Platform support:
 *   macOS   — full implementation via dns_sd (Bonjour)
 *   Linux   — stub; mdns_is_available() returns 0
 *   Windows — stub; mdns_is_available() returns 0
 *
 * Type mappings:
 *   str    = const char *  (null-terminated UTF-8, caller owns unless noted)
 *   i32    = int32_t
 *   bool   = int           (0 = false, non-zero = true)
 *   @(str) = MdnsTxtArray  (array of "key=value" strings)
 *
 * Thread safety:
 *   All public functions are safe to call from any thread.  Internal state is
 *   protected by a mutex.  The browse callback may be invoked from a
 *   background thread — callers must synchronise their own state.
 *
 * Story: 72.8
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * TXT record array — array of "key=value" strings
 * ------------------------------------------------------------------------- */

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

typedef StrArray MdnsTxtArray;

/* -------------------------------------------------------------------------
 * $service_record — service to advertise
 * ------------------------------------------------------------------------- */

typedef struct {
    const char  *name;  /* service instance name, e.g. "My Server" */
    const char  *type;  /* DNS-SD type, e.g. "_http._tcp"           */
    int32_t      port;  /* port number                              */
    MdnsTxtArray txt;   /* TXT record entries ("key=value")         */
} MdnsServiceRecord;

/* -------------------------------------------------------------------------
 * $discovered — service found during browse or resolve
 * ------------------------------------------------------------------------- */

typedef struct {
    char        *name;  /* service instance name (heap-allocated)   */
    char        *host;  /* resolved hostname    (heap-allocated)    */
    int32_t      port;  /* port number                              */
    MdnsTxtArray txt;   /* TXT entries (heap-allocated strings)     */
} MdnsDiscovered;

/* Free a heap-allocated MdnsDiscovered and all its contents. */
void mdns_discovered_free(MdnsDiscovered *d);

/* -------------------------------------------------------------------------
 * Optional result for mdns_resolve
 * ------------------------------------------------------------------------- */

typedef struct {
    int           has_value; /* 1 = found, 0 = none/timeout */
    MdnsDiscovered value;    /* valid only when has_value == 1 */
} MdnsOptDiscovered;

/* -------------------------------------------------------------------------
 * Browse callback type
 *
 * Called (possibly from a background thread) each time a service of the
 * browsed type is discovered.  The MdnsDiscovered pointer is valid only for
 * the duration of the callback; do not retain it.
 * ------------------------------------------------------------------------- */

typedef void (*MdnsBrowseCallback)(const MdnsDiscovered *discovered,
                                   void *user_data);

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * mdns_is_available — returns 1 on platforms with a working mDNS backend,
 * 0 otherwise (Linux stub, Windows stub).
 */
int mdns_is_available(void);

/*
 * mdns_advertise — register svc for advertisement on the local network.
 * Returns 1 on success, 0 on failure (not available, duplicate name,
 * Bonjour error).  The registration persists until mdns_stop_advertise or
 * process exit.
 */
int mdns_advertise(const MdnsServiceRecord *svc);

/*
 * mdns_stop_advertise — deregister the service with the given instance name.
 * Returns 1 if the registration was found and removed, 0 otherwise.
 */
int mdns_stop_advertise(const char *name);

/*
 * mdns_browse — start an asynchronous background browse for service_type.
 * cb is called (from a background thread) each time a matching service is
 * found.  user_data is passed through to every callback invocation.
 * Returns 1 if the browse was started, 0 on error or duplicate browse.
 */
int mdns_browse(const char *service_type, MdnsBrowseCallback cb,
                void *user_data);

/*
 * mdns_stop_browse — cancel the background browse for service_type.
 * Returns 1 if a browse was active and has been stopped, 0 otherwise.
 */
int mdns_stop_browse(const char *service_type);

/*
 * mdns_resolve — synchronously resolve a named service instance.
 * Blocks for up to MDNS_RESOLVE_TIMEOUT_MS milliseconds.
 * Returns an MdnsOptDiscovered; caller must call mdns_discovered_free on
 * &result.value when has_value == 1.
 */
MdnsOptDiscovered mdns_resolve(const char *name, const char *service_type);

/* Resolve timeout in milliseconds. */
#define MDNS_RESOLVE_TIMEOUT_MS 5000

#endif /* TK_STDLIB_MDNS_H */
