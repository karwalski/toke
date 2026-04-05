/*
 * svg.c — Implementation of the std.svg standard library module.
 *
 * Produces well-formed SVG 1.1 markup from a simple element tree.
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 18.1.4
 */

#include "svg.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* System <math.h> is shadowed by the local stdlib/math.h when compiling
 * with -Isrc/stdlib.  Declare what we need directly. */
extern double sqrt(double);


/* -----------------------------------------------------------------------
 * Internal buffer helpers
 * ----------------------------------------------------------------------- */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Buf;

static void buf_init(Buf *b)
{
    b->cap  = 256;
    b->data = (char *)malloc(b->cap);
    b->len  = 0;
    if (b->data) b->data[0] = '\0';
}

static void buf_ensure(Buf *b, size_t extra)
{
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->data = (char *)realloc(b->data, b->cap);
    }
}

static void buf_append(Buf *b, const char *s)
{
    size_t n = strlen(s);
    buf_ensure(b, n);
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
}

static void buf_appendf(Buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_append(b, tmp);
}

/* -----------------------------------------------------------------------
 * Style serialisation
 * ----------------------------------------------------------------------- */

static void buf_append_style(Buf *b, TkSvgStyle s)
{
    int any = 0;
    buf_append(b, " style=\"");
    if (s.fill) {
        buf_appendf(b, "fill:%s", s.fill);
        any = 1;
    }
    if (s.stroke) {
        if (any) buf_append(b, ";");
        buf_appendf(b, "stroke:%s", s.stroke);
        any = 1;
    }
    if (s.stroke_width != 0.0) {
        if (any) buf_append(b, ";");
        buf_appendf(b, "stroke-width:%.2f", s.stroke_width);
        any = 1;
    }
    if (s.opacity != 0.0) {
        if (any) buf_append(b, ";");
        buf_appendf(b, "opacity:%.2f", s.opacity);
        any = 1;
    }
    if (s.font_size != 0.0) {
        if (any) buf_append(b, ";");
        buf_appendf(b, "font-size:%.2f", s.font_size);
        any = 1;
    }
    if (s.font_family) {
        if (any) buf_append(b, ";");
        buf_appendf(b, "font-family:%s", s.font_family);
        /* any = 1; */
    }
    buf_append(b, "\"");
}

/* -----------------------------------------------------------------------
 * Points helper (polyline / polygon)
 * ----------------------------------------------------------------------- */

static void buf_append_points(Buf *b, double *pts, uint64_t npts)
{
    buf_append(b, " points=\"");
    for (uint64_t i = 0; i + 1 < npts; i += 2) {
        if (i > 0) buf_append(b, " ");
        buf_appendf(b, "%.2f,%.2f", pts[i], pts[i + 1]);
    }
    buf_append(b, "\"");
}

/* -----------------------------------------------------------------------
 * Element kinds
 * ----------------------------------------------------------------------- */

typedef enum {
    ELEM_RECT,
    ELEM_CIRCLE,
    ELEM_LINE,
    ELEM_PATH,
    ELEM_TEXT,
    ELEM_GROUP,
    ELEM_POLYLINE,
    ELEM_POLYGON,
    ELEM_ARROW
} ElemKind;

struct TkSvgElem {
    ElemKind kind;

    /* geometry */
    double     x, y, w, h;        /* rect / text */
    double     cx, cy, r;         /* circle */
    double     x1, y1, x2, y2;    /* line / arrow */
    char      *path_d;            /* path */
    char      *text_content;      /* text */
    double    *pts;               /* polyline / polygon */
    uint64_t   npts;

    /* group */
    TkSvgElem **children;
    uint64_t    nchildren;
    char       *transform;        /* may be NULL */

    TkSvgStyle style;
};

/* -----------------------------------------------------------------------
 * Element allocation helpers
 * ----------------------------------------------------------------------- */

static TkSvgElem *elem_alloc(ElemKind kind)
{
    TkSvgElem *e = (TkSvgElem *)calloc(1, sizeof(TkSvgElem));
    if (e) e->kind = kind;
    return e;
}

/* -----------------------------------------------------------------------
 * Public element constructors
 * ----------------------------------------------------------------------- */

TkSvgStyle svg_style(const char *fill, const char *stroke, double stroke_width)
{
    TkSvgStyle s;
    s.fill         = fill;
    s.stroke       = stroke;
    s.stroke_width = stroke_width;
    s.opacity      = 0.0;
    s.font_size    = 0.0;
    s.font_family  = NULL;
    return s;
}

TkSvgElem *svg_rect(double x, double y, double w, double h, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_RECT);
    if (!e) return NULL;
    e->x = x; e->y = y; e->w = w; e->h = h;
    e->style = s;
    return e;
}

TkSvgElem *svg_circle(double cx, double cy, double r, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_CIRCLE);
    if (!e) return NULL;
    e->cx = cx; e->cy = cy; e->r = r;
    e->style = s;
    return e;
}

TkSvgElem *svg_line(double x1, double y1, double x2, double y2, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_LINE);
    if (!e) return NULL;
    e->x1 = x1; e->y1 = y1; e->x2 = x2; e->y2 = y2;
    e->style = s;
    return e;
}

TkSvgElem *svg_path(const char *d, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_PATH);
    if (!e) return NULL;
    e->path_d = d ? strdup(d) : NULL;
    e->style  = s;
    return e;
}

TkSvgElem *svg_text(double x, double y, const char *content, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_TEXT);
    if (!e) return NULL;
    e->x = x; e->y = y;
    e->text_content = content ? strdup(content) : NULL;
    e->style = s;
    return e;
}

TkSvgElem *svg_group(TkSvgElem **elems, uint64_t n, const char *transform)
{
    TkSvgElem *e = elem_alloc(ELEM_GROUP);
    if (!e) return NULL;
    e->nchildren = n;
    if (n > 0 && elems) {
        e->children = (TkSvgElem **)malloc(n * sizeof(TkSvgElem *));
        if (e->children) memcpy(e->children, elems, n * sizeof(TkSvgElem *));
    }
    e->transform = transform ? strdup(transform) : NULL;
    return e;
}

TkSvgElem *svg_polyline(double *pts, uint64_t npts, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_POLYLINE);
    if (!e) return NULL;
    e->npts = npts;
    if (npts > 0 && pts) {
        e->pts = (double *)malloc(npts * sizeof(double));
        if (e->pts) memcpy(e->pts, pts, npts * sizeof(double));
    }
    e->style = s;
    return e;
}

TkSvgElem *svg_polygon(double *pts, uint64_t npts, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_POLYGON);
    if (!e) return NULL;
    e->npts = npts;
    if (npts > 0 && pts) {
        e->pts = (double *)malloc(npts * sizeof(double));
        if (e->pts) memcpy(e->pts, pts, npts * sizeof(double));
    }
    e->style = s;
    return e;
}

TkSvgElem *svg_arrow(double x1, double y1, double x2, double y2, TkSvgStyle s)
{
    TkSvgElem *e = elem_alloc(ELEM_ARROW);
    if (!e) return NULL;
    e->x1 = x1; e->y1 = y1; e->x2 = x2; e->y2 = y2;
    e->style = s;
    return e;
}

/* -----------------------------------------------------------------------
 * Element free
 * ----------------------------------------------------------------------- */

void svg_elem_free(TkSvgElem *elem)
{
    if (!elem) return;
    free(elem->path_d);
    free(elem->text_content);
    free(elem->pts);
    free(elem->transform);
    if (elem->children) {
        for (uint64_t i = 0; i < elem->nchildren; i++) {
            svg_elem_free(elem->children[i]);
        }
        free(elem->children);
    }
    free(elem);
}

/* -----------------------------------------------------------------------
 * Document
 * ----------------------------------------------------------------------- */

struct TkSvgDoc {
    double      width;
    double      height;
    TkSvgElem **elems;
    uint64_t    nelems;
    uint64_t    cap;
    char       *rendered; /* cached last render; freed on next render / svg_free */
};

TkSvgDoc *svg_doc(double width, double height)
{
    TkSvgDoc *doc = (TkSvgDoc *)calloc(1, sizeof(TkSvgDoc));
    if (!doc) return NULL;
    doc->width  = width;
    doc->height = height;
    doc->cap    = 8;
    doc->elems  = (TkSvgElem **)malloc(doc->cap * sizeof(TkSvgElem *));
    return doc;
}

TkSvgDoc *svg_append(TkSvgDoc *doc, TkSvgElem *elem)
{
    if (!doc || !elem) return doc;
    if (doc->nelems >= doc->cap) {
        doc->cap *= 2;
        doc->elems = (TkSvgElem **)realloc(doc->elems,
                                            doc->cap * sizeof(TkSvgElem *));
    }
    doc->elems[doc->nelems++] = elem;
    return doc;
}

/* -----------------------------------------------------------------------
 * Render helpers
 * ----------------------------------------------------------------------- */

static void render_elem(Buf *b, TkSvgElem *e);

static void render_arrow(Buf *b, TkSvgElem *e)
{
    /* Shaft */
    buf_append(b, "<line");
    buf_appendf(b, " x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\"",
                e->x1, e->y1, e->x2, e->y2);
    buf_append_style(b, e->style);
    buf_append(b, "/>");

    /* Arrowhead: small filled triangle at (x2,y2) pointing away from (x1,y1) */
    double dx   = e->x2 - e->x1;
    double dy   = e->y2 - e->y1;
    double len  = sqrt(dx * dx + dy * dy);
    if (len < 1e-9) return;
    double ux   = dx / len;
    double uy   = dy / len;
    double hw   = 6.0;  /* half-width of arrowhead base */
    double hl   = 10.0; /* length of arrowhead */

    /* tip = (x2, y2); base centre = tip - hl*(ux,uy) */
    double bx   = e->x2 - hl * ux;
    double by   = e->y2 - hl * uy;
    /* base corners: perpendicular offsets */
    double p1x  = bx + hw * (-uy);
    double p1y  = by + hw * ux;
    double p2x  = bx - hw * (-uy);
    double p2y  = by - hw * ux;

    double head_pts[6] = { e->x2, e->y2, p1x, p1y, p2x, p2y };

    /* Build a filled-polygon style derived from the stroke colour */
    TkSvgStyle hs = e->style;
    if (e->style.stroke) hs.fill = e->style.stroke;

    buf_append(b, "<polygon");
    buf_append(b, " points=\"");
    buf_appendf(b, "%.2f,%.2f %.2f,%.2f %.2f,%.2f",
                head_pts[0], head_pts[1],
                head_pts[2], head_pts[3],
                head_pts[4], head_pts[5]);
    buf_append(b, "\"");
    buf_append_style(b, hs);
    buf_append(b, "/>");
}

static void render_elem(Buf *b, TkSvgElem *e)
{
    if (!e) return;
    switch (e->kind) {
    case ELEM_RECT:
        buf_append(b, "<rect");
        buf_appendf(b, " x=\"%.2f\" y=\"%.2f\" width=\"%.2f\" height=\"%.2f\"",
                    e->x, e->y, e->w, e->h);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_CIRCLE:
        buf_append(b, "<circle");
        buf_appendf(b, " cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\"",
                    e->cx, e->cy, e->r);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_LINE:
        buf_append(b, "<line");
        buf_appendf(b, " x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\"",
                    e->x1, e->y1, e->x2, e->y2);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_PATH:
        buf_append(b, "<path");
        if (e->path_d) buf_appendf(b, " d=\"%s\"", e->path_d);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_TEXT:
        buf_append(b, "<text");
        buf_appendf(b, " x=\"%.2f\" y=\"%.2f\"", e->x, e->y);
        buf_append_style(b, e->style);
        buf_append(b, ">");
        if (e->text_content) buf_append(b, e->text_content);
        buf_append(b, "</text>");
        break;

    case ELEM_GROUP:
        if (e->transform) {
            buf_append(b, "<g");
            buf_appendf(b, " transform=\"%s\"", e->transform);
            buf_append(b, ">");
        } else {
            buf_append(b, "<g>");
        }
        for (uint64_t i = 0; i < e->nchildren; i++) {
            render_elem(b, e->children[i]);
        }
        buf_append(b, "</g>");
        break;

    case ELEM_POLYLINE:
        buf_append(b, "<polyline");
        buf_append_points(b, e->pts, e->npts);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_POLYGON:
        buf_append(b, "<polygon");
        buf_append_points(b, e->pts, e->npts);
        buf_append_style(b, e->style);
        buf_append(b, "/>");
        break;

    case ELEM_ARROW:
        render_arrow(b, e);
        break;
    }
}

/* -----------------------------------------------------------------------
 * svg_render
 * ----------------------------------------------------------------------- */

const char *svg_render(TkSvgDoc *doc)
{
    if (!doc) return NULL;
    free(doc->rendered);
    doc->rendered = NULL;

    Buf b;
    buf_init(&b);

    /* Opening tag */
    buf_appendf(&b,
        "<svg xmlns=\"http://www.w3.org/2000/svg\""
        " width=\"%.2f\" height=\"%.2f\""
        " viewBox=\"0 0 %.2f %.2f\">",
        doc->width, doc->height,
        doc->width, doc->height);

    /* Elements */
    for (uint64_t i = 0; i < doc->nelems; i++) {
        render_elem(&b, doc->elems[i]);
    }

    buf_append(&b, "</svg>");

    doc->rendered = b.data;
    return doc->rendered;
}

/* -----------------------------------------------------------------------
 * svg_free
 * ----------------------------------------------------------------------- */

void svg_free(TkSvgDoc *doc)
{
    if (!doc) return;
    for (uint64_t i = 0; i < doc->nelems; i++) {
        svg_elem_free(doc->elems[i]);
    }
    free(doc->elems);
    free(doc->rendered);
    free(doc);
}
