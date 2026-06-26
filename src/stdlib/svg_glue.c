/*
 * svg_glue.c — i64-ABI wrappers for std.svg (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=svg:std.svg` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "svg.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static double i64_to_f64(int64_t i){double d;memcpy(&d,&i,sizeof(d));return d;}

int64_t tk_svg_doc_w(int64_t w, int64_t h) {
    TkSvgDoc *doc = svg_doc(i64_to_f64(w), i64_to_f64(h));
    return (int64_t)(intptr_t)doc;
}

int64_t tk_svg_rect_w(int64_t x, int64_t y, int64_t w, int64_t h) {
    TkSvgStyle s = svg_style(NULL, NULL, 0);
    TkSvgElem *elem = svg_rect(i64_to_f64(x), i64_to_f64(y),
                                 i64_to_f64(w), i64_to_f64(h), s);
    return (int64_t)(intptr_t)elem;
}

int64_t tk_svg_circle_w(int64_t cx, int64_t cy, int64_t r) {
    TkSvgStyle s = svg_style(NULL, NULL, 0);
    TkSvgElem *elem = svg_circle(i64_to_f64(cx), i64_to_f64(cy),
                                   i64_to_f64(r), s);
    return (int64_t)(intptr_t)elem;
}

int64_t tk_svg_append_w(int64_t doc, int64_t elem) {
    if (!doc || !elem) return doc;
    svg_append((TkSvgDoc *)(intptr_t)doc, (TkSvgElem *)(intptr_t)elem);
    return doc;
}

int64_t tk_svg_style_w(int64_t elem, int64_t css) {
    if (!elem || !css) return elem;
    svg_elem_set_style((TkSvgElem *)(intptr_t)elem,
                       (const char *)(intptr_t)css);
    return elem;
}

int64_t tk_svg_render_w(int64_t doc) {
    if (!doc) return 0;
    const char *s = svg_render((TkSvgDoc *)(intptr_t)doc);
    return (int64_t)(intptr_t)s;
}
