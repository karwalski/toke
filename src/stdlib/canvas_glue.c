/*
 * canvas_glue.c — i64-ABI wrappers for std.canvas (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=canvas:std.canvas` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "canvas.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static double i64_to_f64(int64_t i){double d;memcpy(&d,&i,sizeof(d));return d;}

int64_t tk_canvas_fillrect_w(int64_t c, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (!c) return 0;
    canvas_fill_rect((TkCanvas *)(intptr_t)c,
                     i64_to_f64(x), i64_to_f64(y),
                     i64_to_f64(w), i64_to_f64(h), NULL);
    return c;
}

int64_t tk_canvas_filltext_w(int64_t c, int64_t text, int64_t x, int64_t y) {
    if (!c) return 0;
    canvas_fill_text((TkCanvas *)(intptr_t)c,
                     (const char *)(intptr_t)text,
                     i64_to_f64(x), i64_to_f64(y), NULL, NULL);
    return c;
}

int64_t tk_canvas_new_w(int64_t w, int64_t h) {
    TkCanvas *c = canvas_new("canvas", (uint32_t)w, (uint32_t)h);
    return (int64_t)(intptr_t)c;
}

int64_t tk_canvas_tohtml_w(int64_t c) {
    if (!c) return 0;
    const char *s = canvas_to_html((TkCanvas *)(intptr_t)c);
    return (int64_t)(intptr_t)s;
}
