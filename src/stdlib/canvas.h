/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_CANVAS_H
#define TK_STDLIB_CANVAS_H

/*
 * canvas.h — C interface for the std.canvas standard library module.
 *
 * Type mappings:
 *   Str   = const char *  (null-terminated UTF-8)
 *   f64   = double
 *   u32   = uint32_t
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 18.1.5
 */

#include <stddef.h>
#include <stdint.h>
#include "strbuf.h"

/*
 * TkCanvas is handed to toke as the `$canvas` record, and tkc reads `c.f`
 * as 8-byte slot <index of f in stdlib/canvas.tki>: {id; width; height;
 * ops}.  So those four come first, in that order, each a whole slot (u32
 * width/height included), and ops is a real toke array of $canvasop; the
 * rest is private to canvas.c (135.30).  Before, width and height were two
 * uint32_t sharing slot 1, `c.height` read the op count and `c.ops` the
 * JS buffer.  `make check-tki-layout` keeps the prefix in step.
 */
/* tki-record-prefix: canvas.canvas */
typedef struct TkCanvas {
    char    *id;
    int64_t  width;     /* u32 */
    int64_t  height;    /* u32 */
    int64_t  ops;       /* [canvasop]: toke array handle of TkCanvasOpRec * */
    /* private */
    uint64_t op_count;
    size_t   op_start;  /* js.len when the current op began */
    TkStrBuf js;        /* raw drawing JS statements (no context preamble) */
    char    *js_out;    /* cached result of canvas_to_js  */
    char    *html_out;  /* cached result of canvas_to_html */
} TkCanvas;

/*
 * One element of $canvas.ops: tag is the Canvas 2D member the operation
 * ends with ("fillRect", "fillText", "globalAlpha", ...), data the
 * JavaScript statement it adds to canvas.tojs's output.
 */
/* tki-record: canvas.canvasop */
typedef struct {
    const char *tag;
    const char *data;
} TkCanvasOpRec;

TkCanvas   *canvas_new(const char *id, uint32_t width, uint32_t height);
void        canvas_free(TkCanvas *c);

/* All drawing functions return the same canvas pointer for chaining */
TkCanvas   *canvas_fill_rect(TkCanvas *c, double x, double y, double w, double h, const char *color);
TkCanvas   *canvas_stroke_rect(TkCanvas *c, double x, double y, double w, double h, const char *color, double lw);
TkCanvas   *canvas_clear_rect(TkCanvas *c, double x, double y, double w, double h);
TkCanvas   *canvas_fill_text(TkCanvas *c, const char *text, double x, double y, const char *font, const char *color);
TkCanvas   *canvas_begin_path(TkCanvas *c);
TkCanvas   *canvas_move_to(TkCanvas *c, double x, double y);
TkCanvas   *canvas_line_to(TkCanvas *c, double x, double y);
TkCanvas   *canvas_arc(TkCanvas *c, double x, double y, double r, double start, double end);
TkCanvas   *canvas_close_path(TkCanvas *c);
TkCanvas   *canvas_fill(TkCanvas *c, const char *color);
TkCanvas   *canvas_stroke(TkCanvas *c, const char *color, double lw);
TkCanvas   *canvas_draw_image(TkCanvas *c, const char *src, double x, double y, double w, double h);
TkCanvas   *canvas_set_alpha(TkCanvas *c, double alpha);
const char *canvas_to_js(TkCanvas *c);    /* JS commands string */
const char *canvas_to_html(TkCanvas *c);  /* <canvas> + <script> block */
uint64_t    canvas_op_count(TkCanvas *c); /* number of drawing ops accumulated */

/* Transform operations (Story 34.5.2) */
void        canvas_translate(TkCanvas *c, double dx, double dy);
void        canvas_rotate(TkCanvas *c, double angle);    /* radians */
void        canvas_scale(TkCanvas *c, double sx, double sy);

/* State stack (Story 34.5.2) */
void        canvas_save(TkCanvas *c);
void        canvas_restore(TkCanvas *c);

/* Style setters (Story 34.5.2) */
void        canvas_fill_style(TkCanvas *c, const char *color);
void        canvas_stroke_style(TkCanvas *c, const char *color);
void        canvas_line_width(TkCanvas *c, double width);

/* Curve commands (Story 34.5.2) */
void        canvas_quadratic_to(TkCanvas *c, double cpx, double cpy, double x, double y);
void        canvas_bezier_to(TkCanvas *c, double cp1x, double cp1y,
                              double cp2x, double cp2y, double x, double y);

/* Gradient (Story 34.5.2) */
const char *canvas_gradient_linear(TkCanvas *c, double x0, double y0,
                                    double x1, double y1); /* returns heap-alloc'd gradient ID */

#endif /* TK_STDLIB_CANVAS_H */
