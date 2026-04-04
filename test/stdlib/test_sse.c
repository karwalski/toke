/*
 * test_sse.c — Unit tests for the std.sse C library (Story 15.1.2).
 *
 * Build and run: make test-stdlib-sse
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/sse.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* Assert that needle appears somewhere inside haystack. */
#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) != NULL, msg)

int main(void)
{
    TkSseCtx *ctx = sse_new();
    ASSERT(ctx != NULL, "sse_new returns non-NULL");

    /* --- Test 1: sse_is_closed starts as 0 --- */
    ASSERT(sse_is_closed(ctx) == 0, "sse_is_closed is 0 after sse_new");

    /* --- Test 2: sse_emitdata basic --- */
    const char *out2 = sse_emitdata(ctx, "hello");
    ASSERT_CONTAINS(out2, "data: hello\n\n", "sse_emitdata(hello) contains 'data: hello\\n\\n'");
    free((void *)out2);

    /* --- Test 3: sse_emit with event name --- */
    TkSseEvent ev3;
    ev3.event = "chat";
    ev3.data  = "msg";
    ev3.id    = NULL;
    ev3.retry = -1;
    const char *out3 = sse_emit(ctx, ev3);
    ASSERT_CONTAINS(out3, "event: chat\n", "sse_emit with event name contains 'event: chat\\n'");
    ASSERT_CONTAINS(out3, "data: msg\n\n", "sse_emit with event name contains 'data: msg\\n\\n'");
    free((void *)out3);

    /* --- Test 4: sse_emit with id --- */
    TkSseEvent ev4;
    ev4.event = NULL;
    ev4.data  = "payload";
    ev4.id    = "42";
    ev4.retry = -1;
    const char *out4 = sse_emit(ctx, ev4);
    ASSERT_CONTAINS(out4, "id: 42\n", "sse_emit with id contains 'id: 42\\n'");
    free((void *)out4);

    /* --- Test 5: sse_emit with retry --- */
    TkSseEvent ev5;
    ev5.event = NULL;
    ev5.data  = "ping";
    ev5.id    = NULL;
    ev5.retry = 1000;
    const char *out5 = sse_emit(ctx, ev5);
    ASSERT_CONTAINS(out5, "retry: 1000\n", "sse_emit with retry contains 'retry: 1000\\n'");
    free((void *)out5);

    /* --- Test 6: multi-line data split into multiple data: lines --- */
    const char *out6 = sse_emitdata(ctx, "line1\nline2");
    ASSERT_CONTAINS(out6, "data: line1\ndata: line2\n\n",
                    "sse_emitdata(line1\\nline2) splits into two data: lines");
    free((void *)out6);

    /* --- Test 7: sse_keepalive --- */
    const char *out7 = sse_keepalive(ctx);
    ASSERT_STREQ(out7, ": keepalive\n\n", "sse_keepalive returns ': keepalive\\n\\n'");
    free((void *)out7);

    /* --- Test 8: sse_close sets is_closed to 1 --- */
    const char *out8 = sse_close(ctx);
    ASSERT(out8 == NULL,          "sse_close returns NULL");
    ASSERT(sse_is_closed(ctx) == 1, "sse_is_closed is 1 after sse_close");

    /* --- Test 9: sse_emit returns NULL on closed context --- */
    TkSseEvent ev9;
    ev9.event = NULL;
    ev9.data  = "ignored";
    ev9.id    = NULL;
    ev9.retry = -1;
    const char *out9 = sse_emit(ctx, ev9);
    ASSERT(out9 == NULL, "sse_emit returns NULL when context is closed");

    /* --- Test 10: sse_emit all fields together --- */
    TkSseCtx *ctx2 = sse_new();
    TkSseEvent ev10;
    ev10.event = "update";
    ev10.data  = "hello\nworld";
    ev10.id    = "99";
    ev10.retry = 500;
    const char *out10 = sse_emit(ctx2, ev10);
    ASSERT_CONTAINS(out10, "event: update\n",       "full event contains event field");
    ASSERT_CONTAINS(out10, "data: hello\ndata: world\n", "full event splits multi-line data");
    ASSERT_CONTAINS(out10, "id: 99\n",              "full event contains id field");
    ASSERT_CONTAINS(out10, "retry: 500\n",          "full event contains retry field");
    free((void *)out10);
    sse_free(ctx2);

    sse_free(ctx);

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
}
