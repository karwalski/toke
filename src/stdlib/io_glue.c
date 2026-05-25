/*
 * io_glue.c — i64-ABI wrappers for std.io module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.io.
 */

#include <stdint.h>
#include <stdio.h>

int64_t tk_io_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}
int64_t tk_io_println_w(int64_t s) {
    if (s) puts((const char *)(intptr_t)s);
    else puts("");
    return 0;
}

int64_t tk_io_readln_w(void) {
    static char buf[4096];
    if (fgets(buf, sizeof(buf), stdin)) {
        /* Strip trailing newline */
        size_t len = 0;
        while (buf[len] && buf[len] != '\n') len++;
        buf[len] = '\0';
        return (int64_t)(intptr_t)buf;
    }
    return (int64_t)(intptr_t)"";
}
