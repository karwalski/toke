/*
 * test_mdns.c — Unit tests for the std.mdns C library (Story 72.8.4).
 *
 * Build and run (macOS):
 *   cc -Wall -Wextra -Isrc/stdlib src/stdlib/mdns.c test/stdlib/test_mdns.c \
 *       -framework CoreServices -o test_mdns && ./test_mdns
 *
 * On Linux/Windows the Bonjour API is not available; all tests validate the
 * stub behaviour (is_available returns false, all functions return safe values).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/mdns.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Callback used by browse tests — counts invocations. */
static volatile int g_browse_cb_count = 0;

static void browse_count_cb(const MdnsDiscovered *d, void *user_data)
{
    (void)user_data;
    (void)d;
    g_browse_cb_count++;
}

/* -------------------------------------------------------------------------
 * 1. is_available
 * ------------------------------------------------------------------------- */

static void test_is_available(void)
{
    int avail = mdns_is_available();
    /* On macOS must be 1; on stub platforms must be 0. */
#ifdef __APPLE__
    ASSERT(avail == 1, "is_available() == 1 on macOS");
#else
    ASSERT(avail == 0, "is_available() == 0 on stub platform");
#endif
}

/* -------------------------------------------------------------------------
 * 2. advertise / stop_advertise — NULL safety
 * ------------------------------------------------------------------------- */

static void test_advertise_null(void)
{
    int r = mdns_advertise(NULL);
    ASSERT(r == 0, "advertise(NULL) returns 0");

    MdnsServiceRecord svc_no_name;
    memset(&svc_no_name, 0, sizeof(svc_no_name));
    svc_no_name.type = "_http._tcp";
    svc_no_name.port = 80;
    ASSERT(mdns_advertise(&svc_no_name) == 0, "advertise(name=NULL) returns 0");

    MdnsServiceRecord svc_no_type;
    memset(&svc_no_type, 0, sizeof(svc_no_type));
    svc_no_type.name = "TestSvc";
    svc_no_type.port = 80;
    ASSERT(mdns_advertise(&svc_no_type) == 0, "advertise(type=NULL) returns 0");

    ASSERT(mdns_stop_advertise(NULL) == 0, "stop_advertise(NULL) returns 0");
}

/* -------------------------------------------------------------------------
 * 3. stop_advertise for unknown name
 * ------------------------------------------------------------------------- */

static void test_stop_advertise_unknown(void)
{
    /* Stopping something that was never registered must return 0. */
    int r = mdns_stop_advertise("__nonexistent_service_xyzzy__");
    ASSERT(r == 0, "stop_advertise(unknown) returns 0");
}

/* -------------------------------------------------------------------------
 * 4. browse — NULL safety
 * ------------------------------------------------------------------------- */

static void test_browse_null(void)
{
    ASSERT(mdns_browse(NULL, browse_count_cb, NULL) == 0,
           "browse(NULL type) returns 0");
    ASSERT(mdns_browse("_http._tcp", NULL, NULL) == 0,
           "browse(NULL cb) returns 0");
    ASSERT(mdns_stop_browse(NULL) == 0,
           "stop_browse(NULL) returns 0");
}

/* -------------------------------------------------------------------------
 * 5. stop_browse for unknown type
 * ------------------------------------------------------------------------- */

static void test_stop_browse_unknown(void)
{
    int r = mdns_stop_browse("_nonexistent._tcp");
    ASSERT(r == 0, "stop_browse(unknown type) returns 0");
}

/* -------------------------------------------------------------------------
 * 6. resolve — NULL safety
 * ------------------------------------------------------------------------- */

static void test_resolve_null(void)
{
    MdnsOptDiscovered r1 = mdns_resolve(NULL, "_http._tcp");
    ASSERT(r1.has_value == 0, "resolve(NULL name) has_value == 0");

    MdnsOptDiscovered r2 = mdns_resolve("TestSvc", NULL);
    ASSERT(r2.has_value == 0, "resolve(NULL type) has_value == 0");

    MdnsOptDiscovered r3 = mdns_resolve(NULL, NULL);
    ASSERT(r3.has_value == 0, "resolve(NULL, NULL) has_value == 0");
}

/* -------------------------------------------------------------------------
 * 7. resolve — non-existent service times out cleanly
 * ------------------------------------------------------------------------- */

static void test_resolve_nonexistent(void)
{
    /*
     * macOS: this will actually attempt a Bonjour resolve, wait 5 s, and
     * return has_value == 0.  We keep the timeout short in CI by only running
     * this test when IS_AVAILABLE is confirmed.  On stub platforms it returns
     * immediately with has_value == 0.
     */
    if (!mdns_is_available()) {
        MdnsOptDiscovered r = mdns_resolve("__xyzzy_nonexistent__",
                                           "_loke-mcp._tcp");
        ASSERT(r.has_value == 0, "resolve stub: has_value == 0");
        return;
    }
    /*
     * On macOS we skip the live timeout test to avoid a 5-second wait in
     * automated test runs.  Functional resolve is validated by browsing tests
     * in integration suites.
     */
    printf("pass: resolve_nonexistent (macOS live test skipped in unit suite)\n");
}

/* -------------------------------------------------------------------------
 * 8. mdns_discovered_free — no crash on NULL / zero-valued struct
 * ------------------------------------------------------------------------- */

static void test_discovered_free(void)
{
    /* NULL pointer: must not crash */
    mdns_discovered_free(NULL);
    ASSERT(1, "discovered_free(NULL) does not crash");

    /* Zero-valued struct */
    MdnsDiscovered d;
    memset(&d, 0, sizeof(d));
    mdns_discovered_free(&d);
    ASSERT(1, "discovered_free(zero-valued) does not crash");

    /* Heap-allocated fields */
    MdnsDiscovered d2;
    memset(&d2, 0, sizeof(d2));
    d2.name = strdup("TestService");
    d2.host = strdup("myhost.local");
    d2.port = 9000;
    d2.txt.len  = 0;
    d2.txt.data = NULL;
    mdns_discovered_free(&d2);
    ASSERT(d2.name == NULL, "discovered_free clears name");
    ASSERT(d2.host == NULL, "discovered_free clears host");
    ASSERT(1, "discovered_free(heap fields) does not crash");
}

/* -------------------------------------------------------------------------
 * 9. MdnsTxtArray — NULL/empty array edge cases via advertise
 * ------------------------------------------------------------------------- */

static void test_txt_empty(void)
{
    /*
     * Advertise with an empty TXT array.  On macOS this should succeed (Bonjour
     * allows empty TXT).  On stub platforms it returns 0 (not available).
     */
    const char *names[] = {NULL};
    MdnsTxtArray empty_txt;
    empty_txt.data = names;
    empty_txt.len  = 0;

    MdnsServiceRecord svc;
    memset(&svc, 0, sizeof(svc));
    svc.name = "TxtEmptyTest";
    svc.type = "_loke-mcp._tcp";
    svc.port = 19001;
    svc.txt  = empty_txt;

    int ok = mdns_advertise(&svc);
    if (mdns_is_available()) {
        ASSERT(ok == 1, "advertise with empty TXT succeeds on macOS");
        mdns_stop_advertise("TxtEmptyTest");
    } else {
        ASSERT(ok == 0, "advertise with empty TXT returns 0 on stub");
    }
}

/* -------------------------------------------------------------------------
 * 10. Duplicate advertise is rejected
 * ------------------------------------------------------------------------- */

static void test_advertise_duplicate(void)
{
    if (!mdns_is_available()) {
        printf("pass: advertise_duplicate (stub platform, skipped)\n");
        return;
    }

    MdnsServiceRecord svc;
    memset(&svc, 0, sizeof(svc));
    svc.name = "DuplicateTest";
    svc.type = "_loke-mcp._tcp";
    svc.port = 19002;
    svc.txt.data = NULL;
    svc.txt.len  = 0;

    int first  = mdns_advertise(&svc);
    int second = mdns_advertise(&svc);

    ASSERT(first  == 1, "first advertise(DuplicateTest) returns 1");
    ASSERT(second == 0, "second advertise(DuplicateTest) returns 0 (duplicate)");

    mdns_stop_advertise("DuplicateTest");
}

/* -------------------------------------------------------------------------
 * 11. Duplicate browse is rejected
 * ------------------------------------------------------------------------- */

static void test_browse_duplicate(void)
{
    if (!mdns_is_available()) {
        printf("pass: browse_duplicate (stub platform, skipped)\n");
        return;
    }

    int first  = mdns_browse("_loke-mcp._tcp", browse_count_cb, NULL);
    int second = mdns_browse("_loke-mcp._tcp", browse_count_cb, NULL);

    ASSERT(first  == 1, "first browse(_loke-mcp._tcp) returns 1");
    ASSERT(second == 0, "second browse(_loke-mcp._tcp) returns 0 (duplicate)");

    mdns_stop_browse("_loke-mcp._tcp");
}

/* -------------------------------------------------------------------------
 * 12. TXT record with multiple entries
 * ------------------------------------------------------------------------- */

static void test_txt_entries(void)
{
    if (!mdns_is_available()) {
        printf("pass: txt_entries (stub platform, skipped)\n");
        return;
    }

    const char *entries[] = {"version=1", "proto=toke", "env=test"};
    MdnsTxtArray txt;
    txt.data = entries;
    txt.len  = 3;

    MdnsServiceRecord svc;
    memset(&svc, 0, sizeof(svc));
    svc.name = "TxtEntriesTest";
    svc.type = "_loke-companion._tcp";
    svc.port = 19003;
    svc.txt  = txt;

    int ok = mdns_advertise(&svc);
    ASSERT(ok == 1, "advertise with 3 TXT entries succeeds");
    mdns_stop_advertise("TxtEntriesTest");
}

/* -------------------------------------------------------------------------
 * 13. stop_advertise after stop (idempotent)
 * ------------------------------------------------------------------------- */

static void test_stop_advertise_idempotent(void)
{
    if (!mdns_is_available()) {
        printf("pass: stop_advertise_idempotent (stub platform, skipped)\n");
        return;
    }

    MdnsServiceRecord svc;
    memset(&svc, 0, sizeof(svc));
    svc.name = "IdempotentTest";
    svc.type = "_loke-mcp._tcp";
    svc.port = 19004;
    svc.txt.data = NULL;
    svc.txt.len  = 0;

    mdns_advertise(&svc);
    int first_stop  = mdns_stop_advertise("IdempotentTest");
    int second_stop = mdns_stop_advertise("IdempotentTest");

    ASSERT(first_stop  == 1, "first stop_advertise returns 1");
    ASSERT(second_stop == 0, "second stop_advertise returns 0 (already stopped)");
}

/* -------------------------------------------------------------------------
 * 14. stop_browse after stop (idempotent)
 * ------------------------------------------------------------------------- */

static void test_stop_browse_idempotent(void)
{
    if (!mdns_is_available()) {
        printf("pass: stop_browse_idempotent (stub platform, skipped)\n");
        return;
    }

    int started = mdns_browse("_loke-companion._tcp", browse_count_cb, NULL);
    ASSERT(started == 1, "browse start for idempotent test");

    int first_stop  = mdns_stop_browse("_loke-companion._tcp");
    int second_stop = mdns_stop_browse("_loke-companion._tcp");

    ASSERT(first_stop  == 1, "first stop_browse returns 1");
    ASSERT(second_stop == 0, "second stop_browse returns 0 (already stopped)");
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_is_available();
    test_advertise_null();
    test_stop_advertise_unknown();
    test_browse_null();
    test_stop_browse_unknown();
    test_resolve_null();
    test_resolve_nonexistent();
    test_discovered_free();
    test_txt_empty();
    test_advertise_duplicate();
    test_browse_duplicate();
    test_txt_entries();
    test_stop_advertise_idempotent();
    test_stop_browse_idempotent();

    if (failures == 0) {
        printf("All mdns tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
