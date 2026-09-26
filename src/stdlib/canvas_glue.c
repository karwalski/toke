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

/* 136.70: fillrect takes the colour and filltext the font and colour that
 * docs/stdlib/canvas.md, test/stdlib/test_canvas.tk (archived by 136.68) and
 * the C core
 * (canvas_fill_rect / canvas_fill_text) all carry.  The wrappers dropped
 * them (passing NULL), so every documented call was an arity error. */
int64_t tk_canvas_fillrect_w(int64_t c, int64_t x, int64_t y, int64_t w, int64_t h,
                             int64_t color) {
    if (!c) return 0;
    canvas_fill_rect((TkCanvas *)(intptr_t)c,
                     i64_to_f64(x), i64_to_f64(y),
                     i64_to_f64(w), i64_to_f64(h),
                     color ? (const char *)(intptr_t)color : NULL);
    return c;
}

int64_t tk_canvas_filltext_w(int64_t c, int64_t text, int64_t x, int64_t y,
                             int64_t font, int64_t color) {
    if (!c) return 0;
    canvas_fill_text((TkCanvas *)(intptr_t)c,
                     (const char *)(intptr_t)text,
                     i64_to_f64(x), i64_to_f64(y),
                     font ? (const char *)(intptr_t)font : NULL,
                     color ? (const char *)(intptr_t)color : NULL);
    return c;
}

/* 136.70: canvas.new(id:str; w:u32; h:u32) -- the signature the docs, the
 * v0.3 spec, test/stdlib/test_canvas.tk (archived by 136.68) and
 * canvas_new() itself all use.
 * The wrapper took (w, h) and hard-coded the id "canvas" while the .tki
 * published (str, u32): no spelling of the call was right for both. */
int64_t tk_canvas_new_w(int64_t id, int64_t w, int64_t h) {
    const char *s = id ? (const char *)(intptr_t)id : "canvas";
    TkCanvas *c = canvas_new(s, (uint32_t)w, (uint32_t)h);
    return (int64_t)(intptr_t)c;
}

int64_t tk_canvas_tohtml_w(int64_t c) {
    if (!c) return 0;
    const char *s = canvas_to_html((TkCanvas *)(intptr_t)c);
    return (int64_t)(intptr_t)s;
}
