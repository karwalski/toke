/* SPDX-License-Identifier: MIT */
/*
 * mdns_glue.c — i64-ABI wrappers for std.mdns module.
 *
 * mDNS/Bonjour service discovery. These require platform-specific
 * DNS-SD implementations (dns_sd.h on macOS, avahi on Linux).
 * Stubs return 0/error when not linked with the platform library.
 */

#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include <stdint.h>
#include <stdlib.h>

/* mdns.advertise(svc) — advertise a service.  135.31 (1): takes the one
 * $servicerecord the .tki publishes (what mdns.servicerecord builds); it took
 * three scalars, so the published call was E4026. */
int64_t tk_mdns_advertise_w(int64_t svc) {
    (void)svc;
    /* Return 0 = no handle (not available) */
    return 0;
}

/* mdns.stopadvertise(handle) — stop advertising */
int64_t tk_mdns_stopadvertise_w(int64_t handle) {
    (void)handle;
    return 0;
}

/* mdns.browse(service_type; cb) -> bool — 137.11 / 121.53: takes the
 * callback the .tki publishes.  It took the service type alone and returned
 * an empty ARRAY into a bool slot: a non-null pointer, so the call read as
 * true — "browsing started" — when nothing was browsed.  No DNS-SD call is
 * made and cb is never invoked, so it now returns false. */
int64_t tk_mdns_browse_w(int64_t service_type, int64_t cb) {
    (void)service_type;
    (void)cb;
    return 0;
}

/* mdns.isavailable() -> bool — 137.11: declared in stdlib/mdns.tki with no
 * wrapper (E9003 at link).  false on every platform: mdns_is_available() in
 * mdns.c is 1 on macOS, but no wrapper in this file reaches that backend, so
 * true would promise advertise/browse that cannot happen. */
int64_t tk_mdns_isavailable_w(void) {
    return 0;
}

/* mdns.stopbrowse(handle) — stop browsing */
int64_t tk_mdns_stopbrowse_w(int64_t handle) {
    (void)handle;
    return 0;
}

/* mdns.servicerecord(name, type, port) — create a service record struct */
int64_t tk_mdns_servicerecord_w(int64_t name, int64_t type, int64_t port) {
    (void)name;
    (void)type;
    (void)port;
    return 0;
}
