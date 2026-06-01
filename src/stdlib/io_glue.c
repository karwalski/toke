/*
 * io_glue.c — i64-ABI wrappers for std.io module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.io.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_io_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}
int64_t tk_io_println_w(int64_t s) {
    if (s) puts((const char *)(intptr_t)s);
    else puts("");
    return 0;
}

int64_t tk_io_eprintln_w(int64_t s) {
    if (s) fprintf(stderr, "%s\n", (const char *)(intptr_t)s);
    else fprintf(stderr, "\n");
    return 0;
}

int64_t tk_io_readln_w(void) {
    /* Issue 112.1: previously returned a pointer to a static buffer, which
     * meant `let a=io.readln();let b=io.readln()` had both a and b pointing
     * to the same memory — every binding aliased the latest read.
     * Fix: allocate a fresh buffer per call so each binding owns its value. */
    char tmp[4096];
    if (fgets(tmp, sizeof(tmp), stdin)) {
        size_t len = 0;
        while (tmp[len] && tmp[len] != '\n') len++;
        tmp[len] = '\0';
        char *out = (char *)malloc(len + 1);
        if (!out) return (int64_t)(intptr_t)"";
        memcpy(out, tmp, len + 1);
        return (int64_t)(intptr_t)out;
    }
    return (int64_t)(intptr_t)"";
}

/* io.printf(fmt, val) — formatted print (Story 103) */
int64_t tk_io_printf_w(int64_t fmt, int64_t val) {
    if (fmt) printf((const char *)(intptr_t)fmt, (const char *)(intptr_t)val);
    return 0;
}
