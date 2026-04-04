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

#include <stdint.h>

typedef struct TkCanvas TkCanvas;

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

#endif /* TK_STDLIB_CANVAS_H */
