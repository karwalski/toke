/* SPDX-License-Identifier: MIT */
/*
 * svg_glue.c — i64-ABI wrappers for std.svg.
 *
 * Story 114.35 extracted these from tk_web_glue.c so a standalone
 * `i=svg:std.svg` import links them. Story 114.51 brings them in line with
 * svg.tki: the previous wrappers implemented a *simplified* API (rect/circle
 * with no style, a style(elem,css) that didn't match svg.style(fill;stroke;
 * width)) and had no line/text/path/group/polyline/polygon/arrow.
 *
 * Style representation: both `svg.style(fill;stroke;width)` and a toke
 * `$svgstyle{fill;stroke;strokewidth;opacity;fontsize;fontfamily}` literal
 * produce the SAME 6-slot heap block (each slot an i64; f64 fields hold the
 * double bit pattern), so element wrappers read a style handle uniformly via
 * read_svgstyle() regardless of which built it.
 */
#include "svg.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "tk_runtime.h"   /* 138.45: the handle registry */

/* 138.45: an svgelem and an svgdoc are looked up in the runtime's handle
 * registry (tk_runtime.c) before they are read.  Every element and document
 * std.svg hands out is registered; any other value -- an integer, `n as
 * $svgelem`, a $svgdoc record literal, whose C document has no element list
 * behind its four slots -- is answered as the null element or document is:
 * svg.append returns the document unchanged, svg.group leaves the value out,
 * svg.render answers the null string.  It used to be read as a TkSvgElem /
 * TkSvgDoc and the program died with SIGSEGV (svg.append(d;5), svg.render(5)).
 * Elements and documents are never freed from toke, so an entry lives as long
 * as its object. */
static int64_t svg_elem_out(TkSvgElem *e) { return (int64_t)(intptr_t)tk_handle_adopt(TK_HK_SVGELEM, e); }
static TkSvgElem *svg_elem_in(int64_t v) { return (TkSvgElem *)tk_handle_addr(TK_HK_SVGELEM, v); }
static TkSvgDoc *svg_doc_in(int64_t v)   { return (TkSvgDoc *)tk_handle_addr(TK_HK_SVGDOC, v); }

static double i64_to_f64(int64_t i){ double d; memcpy(&d, &i, sizeof d); return d; }

/* Decode a $svgstyle (TkSvgStyleRec, 135.30) into a C TkSvgStyle. */
static TkSvgStyle read_svgstyle(int64_t s) {
    TkSvgStyle st; st.fill = NULL; st.stroke = NULL; st.stroke_width = 0.0;
    st.opacity = 0.0; st.font_size = 0.0; st.font_family = NULL;
    if (!s) return st;
    const TkSvgStyleRec *p = (const TkSvgStyleRec *)(intptr_t)s;
    st.fill         = p->fill;
    st.stroke       = p->stroke;
    st.stroke_width = p->strokewidth;
    st.opacity      = p->opacity;
    st.font_size    = p->fontsize;
    st.font_family  = p->fontfamily;
    /* Empty strings mean "omit" for the optional text fields. */
    if (st.font_family && st.font_family[0] == '\0') st.font_family = NULL;
    return st;
}

/* svg.style(fill:str; stroke:str; width:f64) -> svgstyle, a TkSvgStyleRec. */
int64_t tk_svg_style_w(int64_t fill, int64_t stroke, int64_t width) {
    TkSvgStyleRec *b = (TkSvgStyleRec *)malloc(sizeof *b);
    if (!b) return 0;
    b->fill        = (const char *)(intptr_t)fill;
    b->stroke      = (const char *)(intptr_t)stroke;
    b->strokewidth = i64_to_f64(width);   /* an f64 bit pattern (i64 ABI) */
    b->opacity     = 0.0;                 /* omit */
    b->fontsize    = 0.0;                 /* omit */
    b->fontfamily  = "";                  /* omit */
    return (int64_t)(intptr_t)b;
}

int64_t tk_svg_doc_w(int64_t w, int64_t h) {
    return (int64_t)(intptr_t)tk_handle_adopt(TK_HK_SVGDOC, svg_doc(i64_to_f64(w), i64_to_f64(h)));
}

int64_t tk_svg_rect_w(int64_t x, int64_t y, int64_t w, int64_t h, int64_t style) {
    return svg_elem_out(svg_rect(i64_to_f64(x), i64_to_f64(y),
                                       i64_to_f64(w), i64_to_f64(h),
                                       read_svgstyle(style)));
}

int64_t tk_svg_circle_w(int64_t cx, int64_t cy, int64_t r, int64_t style) {
    return svg_elem_out(svg_circle(i64_to_f64(cx), i64_to_f64(cy),
                                         i64_to_f64(r), read_svgstyle(style)));
}

int64_t tk_svg_line_w(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t style) {
    return svg_elem_out(svg_line(i64_to_f64(x1), i64_to_f64(y1),
                                       i64_to_f64(x2), i64_to_f64(y2),
                                       read_svgstyle(style)));
}

int64_t tk_svg_text_w(int64_t x, int64_t y, int64_t content, int64_t style) {
    return svg_elem_out(svg_text(i64_to_f64(x), i64_to_f64(y),
                                       (const char *)(intptr_t)content,
                                       read_svgstyle(style)));
}

int64_t tk_svg_path_w(int64_t d, int64_t style) {
    return svg_elem_out(svg_path((const char *)(intptr_t)d, read_svgstyle(style)));
}

int64_t tk_svg_arrow_w(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t style) {
    return svg_elem_out(svg_arrow(i64_to_f64(x1), i64_to_f64(y1),
                                        i64_to_f64(x2), i64_to_f64(y2),
                                        read_svgstyle(style)));
}

/* svg.group([svgelem]; transform:str) -> svgelem.
 * The toke array handle: ptr[-1] = len, ptr[0..len-1] = elem pointers. */
int64_t tk_svg_group_w(int64_t elems, int64_t transform) {
    if (!elems) return 0;
    const int64_t *a = (const int64_t *)(intptr_t)elems;
    int64_t n = a[-1];
    if (n < 0) n = 0;
    TkSvgElem **arr = (TkSvgElem **)malloc((size_t)(n > 0 ? n : 1) * sizeof(TkSvgElem *));
    if (!arr) return 0;
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {   /* 138.45: a value svg did not hand out is left out */
        TkSvgElem *e = svg_elem_in(a[i]);
        if (e) arr[k++] = e;
    }
    n = k;
    const char *tr = transform ? (const char *)(intptr_t)transform : NULL;
    if (tr && tr[0] == '\0') tr = NULL;
    TkSvgElem *g = svg_group(arr, (uint64_t)n, tr);
    free(arr);
    return svg_elem_out(g);
}

/* Flatten a toke [[f64]] (array of [x,y]) into a packed double[] x0,y0,x1,y1,…
 * Returns the double count via the return value; caller frees *out. */
static uint64_t flatten_points(int64_t pts, double **out) {
    *out = NULL;
    if (!pts) return 0;
    const int64_t *a = (const int64_t *)(intptr_t)pts;
    int64_t npairs = a[-1];
    if (npairs <= 0) return 0;
    double *buf = (double *)malloc((size_t)npairs * 2 * sizeof(double));
    if (!buf) return 0;
    uint64_t k = 0;
    for (int64_t i = 0; i < npairs; i++) {
        const int64_t *pt = (const int64_t *)(intptr_t)a[i];
        if (!pt) { buf[k++] = 0.0; buf[k++] = 0.0; continue; }
        int64_t plen = pt[-1];
        buf[k++] = plen >= 1 ? i64_to_f64(pt[0]) : 0.0;
        buf[k++] = plen >= 2 ? i64_to_f64(pt[1]) : 0.0;
    }
    *out = buf;
    return k;
}

int64_t tk_svg_polyline_w(int64_t pts, int64_t style) {
    double *flat = NULL; uint64_t ndbl = flatten_points(pts, &flat);
    TkSvgElem *e = svg_polyline(flat, ndbl, read_svgstyle(style));
    free(flat);
    return svg_elem_out(e);
}

int64_t tk_svg_polygon_w(int64_t pts, int64_t style) {
    double *flat = NULL; uint64_t ndbl = flatten_points(pts, &flat);
    TkSvgElem *e = svg_polygon(flat, ndbl, read_svgstyle(style));
    free(flat);
    return svg_elem_out(e);
}

int64_t tk_svg_append_w(int64_t doc, int64_t elem) {
    TkSvgDoc *d = svg_doc_in(doc);     /* 138.45 */
    TkSvgElem *e = svg_elem_in(elem);
    if (!d || !e) return doc;
    svg_append(d, e);
    return doc;
}

/* 127.316: svg_render() returns the document's cached buffer, which the
 * NEXT render of the same document frees -- so a toke string from an earlier
 * svg.render(d) dangled once d was rendered again.  toke strings are never
 * freed (runtime-abi §3), so the wrapper hands back its own copy: a sealed
 * string (127.295, §3.1), whose length is then O(1). */
int64_t tk_svg_render_w(int64_t doc) {
    TkSvgDoc *d = svg_doc_in(doc);     /* 138.45 */
    if (!d) return 0;
    const char *s = svg_render(d);
    return s ? (int64_t)(intptr_t)tks_strdup(s) : 0;
}
