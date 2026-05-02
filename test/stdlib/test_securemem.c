/*
 * test_securemem.c — Unit tests for the std.securemem C library.
 *
 * Tests cover: alloc, write, read, wipe, sweep, is_available, expiry, and
 * basic thread-safety (two threads writing to separate buffers concurrently).
 *
 * Build and run:
 *   cc -Wall -Wextra -Wpedantic -Isrc -Isrc/stdlib \
 *       src/stdlib/securemem.c test/stdlib/test_securemem.c \
 *       -lpthread -o /tmp/test_securemem && /tmp/test_securemem
 *
 * Story: 72.3.4
 */

#if !defined(_GNU_SOURCE) && !defined(_BSD_SOURCE) && \
    !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>    /* sleep() */
#include <pthread.h>

#include "../../src/stdlib/securemem.h"

/* -------------------------------------------------------------------------
 * Test helpers
 * ------------------------------------------------------------------------- */

static int g_failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            g_failures++; \
        } else { \
            printf("pass: %s\n", (msg)); \
        } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) != NULL && (b) != NULL && strcmp((a), (b)) == 0, (msg))

/* -------------------------------------------------------------------------
 * test_alloc — basic allocation returns a valid handle
 * ------------------------------------------------------------------------- */
static void test_alloc(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 300);

    ASSERT(buf.id[0] != '\0',     "alloc: id is non-empty");
    ASSERT(buf.size == 64,        "alloc: size matches request");
    ASSERT(buf.expires_at > 0,    "alloc: expires_at is set for ttl > 0");

    /* Allocate with no expiry. */
    TkSecureBuf buf2 = tk_securemem_alloc(32, 0);
    ASSERT(buf2.id[0] != '\0',    "alloc(ttl=0): id is non-empty");
    ASSERT(buf2.expires_at == 0,  "alloc(ttl=0): expires_at is zero");

    /* Zero size should return an empty handle (id[0] == '\0'). */
    TkSecureBuf bad = tk_securemem_alloc(0, 10);
    ASSERT(bad.id[0] == '\0',     "alloc(size=0): returns empty handle");

    /* Wipe to clean up. */
    tk_securemem_wipe(buf);
    tk_securemem_wipe(buf2);
}

/* -------------------------------------------------------------------------
 * test_write_read — write then read back the same value
 * ------------------------------------------------------------------------- */
static void test_write_read(void)
{
    TkSecureBuf buf = tk_securemem_alloc(128, 60);

    int wrote = tk_securemem_write(buf, "hello-secret");
    ASSERT(wrote == 1, "write: returns 1 on success");

    TkSecureReadResult r = tk_securemem_read(buf);
    ASSERT(r.is_none == 0,              "read: is_none=0 after write");
    ASSERT_STREQ(r.value, "hello-secret", "read: value matches written data");
    free(r.value);

    /* Overwrite with a different value. */
    tk_securemem_write(buf, "updated");
    TkSecureReadResult r2 = tk_securemem_read(buf);
    ASSERT(r2.is_none == 0,          "read after overwrite: is_none=0");
    ASSERT_STREQ(r2.value, "updated", "read after overwrite: value correct");
    free(r2.value);

    tk_securemem_wipe(buf);
}

/* -------------------------------------------------------------------------
 * test_write_too_long — write rejects data >= capacity
 * ------------------------------------------------------------------------- */
static void test_write_too_long(void)
{
    TkSecureBuf buf = tk_securemem_alloc(8, 60);  /* capacity: 8 bytes */

    /* 8 bytes of data requires 9 bytes (data + NUL) — must be rejected. */
    int wrote = tk_securemem_write(buf, "12345678");
    ASSERT(wrote == 0, "write: rejects data == capacity (no room for NUL)");

    /* 7 bytes of data fits (7 + NUL = 8). */
    int wrote2 = tk_securemem_write(buf, "1234567");
    ASSERT(wrote2 == 1, "write: accepts data of size = capacity-1");

    tk_securemem_wipe(buf);
}

/* -------------------------------------------------------------------------
 * test_read_empty — read on a freshly allocated (never written) buffer
 * ------------------------------------------------------------------------- */
static void test_read_empty(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 60);

    TkSecureReadResult r = tk_securemem_read(buf);
    /* After alloc the data region is zeroed; strnlen returns 0.
     * The result should be Some("") — an empty string, not None. */
    ASSERT(r.is_none == 0,         "read on fresh buf: is_none=0");
    ASSERT(r.value != NULL,        "read on fresh buf: value != NULL");
    ASSERT(strlen(r.value) == 0,   "read on fresh buf: empty string");
    free(r.value);

    tk_securemem_wipe(buf);
}

/* -------------------------------------------------------------------------
 * test_wipe — wipe makes the buffer unreadable
 * ------------------------------------------------------------------------- */
static void test_wipe(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 300);
    tk_securemem_write(buf, "to-be-wiped");

    int wiped = tk_securemem_wipe(buf);
    ASSERT(wiped == 1, "wipe: returns 1 on first call");

    TkSecureReadResult r = tk_securemem_read(buf);
    ASSERT(r.is_none == 1, "read after wipe: is_none=1");

    /* Wipe again — should return 0 (already gone). */
    int wiped2 = tk_securemem_wipe(buf);
    ASSERT(wiped2 == 0, "wipe: returns 0 on second call (already wiped)");
}

/* -------------------------------------------------------------------------
 * test_write_wiped — write to a wiped buffer returns 0
 * ------------------------------------------------------------------------- */
static void test_write_wiped(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 300);
    tk_securemem_wipe(buf);

    int wrote = tk_securemem_write(buf, "ghost");
    ASSERT(wrote == 0, "write to wiped buf: returns 0");
}

/* -------------------------------------------------------------------------
 * test_sweep — expired buffers are freed by sweep()
 * ------------------------------------------------------------------------- */
static void test_sweep(void)
{
    /* Allocate three buffers with 1-second TTL. */
    TkSecureBuf b1 = tk_securemem_alloc(32, 1);
    TkSecureBuf b2 = tk_securemem_alloc(32, 1);
    TkSecureBuf b3 = tk_securemem_alloc(32, 300);  /* longer TTL */

    tk_securemem_write(b1, "expire1");
    tk_securemem_write(b2, "expire2");
    tk_securemem_write(b3, "survivor");

    /* Before expiry, sweep should free nothing (or at most 0 of these 3). */
    int32_t before = tk_securemem_sweep();
    /* We can't assert exactly 0 because other tests may have left expired
     * nodes, but the live count here should not include b3. */
    (void)before;

    /* Wait for TTL to elapse. */
    sleep(2);

    int32_t freed = tk_securemem_sweep();
    ASSERT(freed >= 2, "sweep: freed at least 2 expired buffers");

    /* b1 and b2 are now gone. */
    TkSecureReadResult r1 = tk_securemem_read(b1);
    ASSERT(r1.is_none == 1, "read b1 after sweep: is_none=1");

    TkSecureReadResult r3 = tk_securemem_read(b3);
    ASSERT(r3.is_none == 0, "read b3 after sweep: still live");
    free(r3.value);

    tk_securemem_wipe(b3);
}

/* -------------------------------------------------------------------------
 * test_expiry_read — read returns None after TTL elapses (no sweep needed)
 * ------------------------------------------------------------------------- */
static void test_expiry_read(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 1);  /* 1-second TTL */
    tk_securemem_write(buf, "expires-soon");

    /* Immediately readable. */
    TkSecureReadResult r1 = tk_securemem_read(buf);
    ASSERT(r1.is_none == 0, "read before expiry: is_none=0");
    free(r1.value);

    sleep(2);

    /* Should now return None even without calling sweep(). */
    TkSecureReadResult r2 = tk_securemem_read(buf);
    ASSERT(r2.is_none == 1, "read after TTL elapsed: is_none=1");

    /* Clean up the stale node via sweep. */
    tk_securemem_sweep();
}

/* -------------------------------------------------------------------------
 * test_is_available — simply check the function runs without crashing
 * ------------------------------------------------------------------------- */
static void test_is_available(void)
{
    int avail = tk_securemem_isavailable();
    /* We can't assert true/false — mlock availability varies by platform and
     * privilege level.  Just verify the call returns 0 or 1. */
    ASSERT(avail == 0 || avail == 1, "is_available: returns 0 or 1");
    printf("      (is_available() = %s on this platform)\n",
           avail ? "true" : "false");
}

/* -------------------------------------------------------------------------
 * test_null_write — write with NULL data returns 0
 * ------------------------------------------------------------------------- */
static void test_null_write(void)
{
    TkSecureBuf buf = tk_securemem_alloc(64, 60);
    int wrote = tk_securemem_write(buf, NULL);
    ASSERT(wrote == 0, "write(NULL): returns 0");
    tk_securemem_wipe(buf);
}

/* -------------------------------------------------------------------------
 * test_empty_handle — read/write/wipe on a zeroed handle are safe
 * ------------------------------------------------------------------------- */
static void test_empty_handle(void)
{
    TkSecureBuf empty;
    memset(&empty, 0, sizeof(empty));

    TkSecureReadResult r = tk_securemem_read(empty);
    ASSERT(r.is_none == 1, "read(empty handle): is_none=1");

    int wrote = tk_securemem_write(empty, "ghost");
    ASSERT(wrote == 0, "write(empty handle): returns 0");

    int wiped = tk_securemem_wipe(empty);
    ASSERT(wiped == 0, "wipe(empty handle): returns 0");
}

/* -------------------------------------------------------------------------
 * test_multiple_buffers — many buffers coexist without collisions
 * ------------------------------------------------------------------------- */
#define MULTI_N 20
static void test_multiple_buffers(void)
{
    TkSecureBuf bufs[MULTI_N];
    char expected[MULTI_N][32];
    int i;

    for (i = 0; i < MULTI_N; i++) {
        bufs[i] = tk_securemem_alloc(64, 60);
        snprintf(expected[i], sizeof(expected[i]), "secret-%d", i);
        tk_securemem_write(bufs[i], expected[i]);
    }

    /* Verify each buffer returns its own value. */
    int all_ok = 1;
    for (i = 0; i < MULTI_N; i++) {
        TkSecureReadResult r = tk_securemem_read(bufs[i]);
        if (r.is_none || strcmp(r.value, expected[i]) != 0) {
            all_ok = 0;
            fprintf(stderr, "FAIL: buffer %d: got '%s' want '%s'\n",
                    i, r.value ? r.value : "(null)", expected[i]);
        }
        free(r.value);
    }
    ASSERT(all_ok == 1, "multiple buffers: all return correct values");

    for (i = 0; i < MULTI_N; i++) tk_securemem_wipe(bufs[i]);
}

/* -------------------------------------------------------------------------
 * Concurrent write test — two threads write to different buffers
 * ------------------------------------------------------------------------- */
typedef struct {
    TkSecureBuf buf;
    const char *data;
    int         result; /* 1 = success */
} ThreadArg;

static void *thread_write(void *arg)
{
    ThreadArg *a = (ThreadArg *)arg;
    a->result = tk_securemem_write(a->buf, a->data);
    return NULL;
}

static void test_concurrent_write(void)
{
    TkSecureBuf b1 = tk_securemem_alloc(64, 60);
    TkSecureBuf b2 = tk_securemem_alloc(64, 60);

    ThreadArg a1 = {b1, "thread-one-data", 0};
    ThreadArg a2 = {b2, "thread-two-data", 0};

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_write, &a1);
    pthread_create(&t2, NULL, thread_write, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    ASSERT(a1.result == 1, "concurrent write: thread 1 succeeded");
    ASSERT(a2.result == 1, "concurrent write: thread 2 succeeded");

    TkSecureReadResult r1 = tk_securemem_read(b1);
    TkSecureReadResult r2 = tk_securemem_read(b2);
    ASSERT_STREQ(r1.value, "thread-one-data", "concurrent: b1 correct value");
    ASSERT_STREQ(r2.value, "thread-two-data", "concurrent: b2 correct value");
    free(r1.value);
    free(r2.value);

    tk_securemem_wipe(b1);
    tk_securemem_wipe(b2);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_securemem ===\n\n");

    printf("-- alloc --\n");
    test_alloc();

    printf("-- write/read --\n");
    test_write_read();

    printf("-- write too long --\n");
    test_write_too_long();

    printf("-- read empty buffer --\n");
    test_read_empty();

    printf("-- wipe --\n");
    test_wipe();

    printf("-- write to wiped --\n");
    test_write_wiped();

    printf("-- is_available --\n");
    test_is_available();

    printf("-- null write --\n");
    test_null_write();

    printf("-- empty handle --\n");
    test_empty_handle();

    printf("-- multiple buffers --\n");
    test_multiple_buffers();

    printf("-- concurrent write --\n");
    test_concurrent_write();

    /* Time-dependent tests last (they call sleep()). */
    printf("-- expiry read (waits 2s) --\n");
    test_expiry_read();

    printf("-- sweep (waits 2s) --\n");
    test_sweep();

    printf("\n=== %s: %d failure(s) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
