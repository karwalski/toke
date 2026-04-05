#ifndef TK_STDLIB_SVG_H
#define TK_STDLIB_SVG_H

/*
 * svg.h — C interface for the std.svg standard library module.
 *
 * Type mappings:
 *   Str  = const char *  (null-terminated UTF-8)
 *   f64  = double
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 18.1.4
 */

#include <stdint.h>

typedef struct TkSvgDoc  TkSvgDoc;
typedef struct TkSvgElem TkSvgElem;

/* Gradient stop descriptor (Story 34.5.1) */
typedef struct {
    double      offset;  /* 0.0–1.0 */
    const char *color;
    double      opacity; /* 0.0–1.0 */
} SvgStop;

typedef struct {
    const char *fill;
    const char *stroke;
    double      stroke_width;
    double      opacity;      /* 0.0 = omit */
    double      font_size;    /* 0.0 = omit */
    const char *font_family;  /* NULL = omit */
} TkSvgStyle;

/* Document lifecycle */
TkSvgDoc  *svg_doc(double width, double height);
TkSvgDoc  *svg_append(TkSvgDoc *doc, TkSvgElem *elem);
const char *svg_render(TkSvgDoc *doc);
void        svg_free(TkSvgDoc *doc);

/* Style constructor */
TkSvgStyle  svg_style(const char *fill, const char *stroke, double stroke_width);

/* Element constructors */
TkSvgElem  *svg_rect(double x, double y, double w, double h, TkSvgStyle s);
TkSvgElem  *svg_circle(double cx, double cy, double r, TkSvgStyle s);
TkSvgElem  *svg_line(double x1, double y1, double x2, double y2, TkSvgStyle s);
TkSvgElem  *svg_path(const char *d, TkSvgStyle s);
TkSvgElem  *svg_text(double x, double y, const char *content, TkSvgStyle s);
TkSvgElem  *svg_group(TkSvgElem **elems, uint64_t n, const char *transform); /* transform may be NULL */
TkSvgElem  *svg_polyline(double *pts, uint64_t npts, TkSvgStyle s);  /* pts: x0,y0,x1,y1,... */
TkSvgElem  *svg_polygon(double *pts, uint64_t npts, TkSvgStyle s);
TkSvgElem  *svg_arrow(double x1, double y1, double x2, double y2, TkSvgStyle s);
void        svg_elem_free(TkSvgElem *elem);

/* -----------------------------------------------------------------------
 * Story 34.5.1 — gradients, animation, ellipse, defs, file output
 * ----------------------------------------------------------------------- */

/* New shape */
const char *svg_ellipse(TkSvgDoc *doc, double cx, double cy,
                        double rx, double ry, const char *style);

/* Gradients — added to <defs> section; return url(#id) reference string */
const char *svg_gradient_linear(TkSvgDoc *doc, const char *id,
                                const SvgStop *stops, uint64_t nstops,
                                double x1, double y1, double x2, double y2);
const char *svg_gradient_radial(TkSvgDoc *doc, const char *id,
                                const SvgStop *stops, uint64_t nstops,
                                double cx, double cy, double r);

/* Animation — attaches <animate> to the element with the given id */
const char *svg_animate(TkSvgDoc *doc, const char *target_id,
                        const char *attr, const char *from_val,
                        const char *to_val, double duration_s);

/* Defs — add arbitrary reusable SVG markup to the <defs> block */
void svg_defs(TkSvgDoc *doc, const char *defs_content);

/* File output — write svg_render() result to path; 1 = ok, 0 = error */
int svg_save_file(TkSvgDoc *doc, const char *path);

#endif /* TK_STDLIB_SVG_H */
