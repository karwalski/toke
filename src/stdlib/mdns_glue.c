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

/* mdns.advertise(service_type, name, port) — advertise a service */
int64_t tk_mdns_advertise_w(int64_t service_type, int64_t name, int64_t port) {
    (void)service_type;
    (void)name;
    (void)port;
    /* Return 0 = no handle (not available) */
    return 0;
}

/* mdns.stopadvertise(handle) — stop advertising */
int64_t tk_mdns_stopadvertise_w(int64_t handle) {
    (void)handle;
    return 0;
}

/* mdns.browse(service_type) — browse for services, return array of records */
int64_t tk_mdns_browse_w(int64_t service_type) {
    (void)service_type;
    /* Return empty array */
    return tk_arr_alloc(0, 0);
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
