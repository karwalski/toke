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

/* Issue 113.B.22: a blank input line and end-of-input both make readln
 * return "", so a program cannot distinguish them. We record whether the
 * most recent readln reached EOF in this file-static flag, exposed via
 * io.eof() (tk_io_eof_w). readln's return value is unchanged, so existing
 * `if(str.len(l)=0){br}` programs are unaffected; new programs can read
 * blank-line-delimited input with `let l=io.readln(); if(io.eof()){br}; …`. */
static int tk_io_eof_flag = 0;

int64_t tk_io_readln_w(void) {
    /* Issue 112.1: previously returned a pointer to a static buffer, which
     * meant `let a=io.readln();let b=io.readln()` had both a and b pointing
     * to the same memory — every binding aliased the latest read.
     * Fix: allocate a fresh buffer per call so each binding owns its value. */
    char tmp[4096];
    if (fgets(tmp, sizeof(tmp), stdin)) {
        tk_io_eof_flag = 0;
        size_t len = 0;
        while (tmp[len] && tmp[len] != '\n') len++;
        tmp[len] = '\0';
        char *out = (char *)malloc(len + 1);
        if (!out) return (int64_t)(intptr_t)"";
        memcpy(out, tmp, len + 1);
        return (int64_t)(intptr_t)out;
    }
    tk_io_eof_flag = 1;
    return (int64_t)(intptr_t)"";
}

/* io.eof() — returns 1 (true) if the most recent io.readln() reached
 * end-of-input rather than reading a (possibly blank) line. Issue 113.B.22. */
int64_t tk_io_eof_w(void) {
    return (int64_t)tk_io_eof_flag;
}

/* io.printf(fmt, val) — formatted print (Story 103) */
int64_t tk_io_printf_w(int64_t fmt, int64_t val) {
    if (fmt) printf((const char *)(intptr_t)fmt, (const char *)(intptr_t)val);
    return 0;
}
