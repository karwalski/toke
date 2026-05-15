/*
 * clipboard_glue.c — stubs for std.clipboard module.
 * Clipboard access requires platform-specific code (macOS pbcopy/pbpaste,
 * X11 xclip, etc.). Stubs return empty for headless/server environments.
 */
#include <stdint.h>
#include <string.h>

int64_t tk_clipboard_read_w(void) {
    return (int64_t)(intptr_t)"";
}

int64_t tk_clipboard_write_w(int64_t text) {
    (void)text;
    return 0;
}
