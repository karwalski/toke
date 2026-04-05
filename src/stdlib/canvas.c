/*
 * canvas.c — Implementation of the std.canvas standard library module.
 *
 * Each drawing function appends a JS statement to an internal string buffer.
 * canvas_to_js() emits the full context-setup line followed by all buffered ops.
 * canvas_to_html() wraps the JS output in a <canvas> element and <script> block.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned const char * from to_js / to_html
 * only for the lifetime of the TkCanvas that produced them (the strings are
 * held inside the struct and freed with canvas_free).
 *
 * Story: 18.1.5
 */

#include "canvas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal buffer helper
 * ----------------------------------------------------------------------- */

#define BUF_INIT_CAP 1024

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} Buf;

static void buf_init(Buf *b)
{
    b->data = (char *)malloc(BUF_INIT_CAP);
    b->len  = 0;
    b->cap  = BUF_INIT_CAP;
    if (b->data) b->data[0] = '\0';
}

static void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void buf_append(Buf *b, const char *s)
{
    size_t slen = strlen(s);
    size_t need = b->len + slen + 1;
    if (need > b->cap) {
        size_t newcap = b->cap * 2;
        while (newcap < need) newcap *= 2;
        b->data = (char *)realloc(b->data, newcap);
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
}

/* -----------------------------------------------------------------------
 * TkCanvas struct
 * ----------------------------------------------------------------------- */

struct TkCanvas {
    char    *id;
    uint32_t width;
    uint32_t height;
    uint64_t op_count;
    Buf      ops;     /* raw drawing JS statements (no context preamble) */
    char    *js_out;  /* cached result of canvas_to_js  */
    char    *html_out;/* cached result of canvas_to_html */
};

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

TkCanvas *canvas_new(const char *id, uint32_t width, uint32_t height)
{
    TkCanvas *c = (TkCanvas *)malloc(sizeof(TkCanvas));
    if (!c) return NULL;
    c->id       = id ? strdup(id) : strdup("");
    c->width    = width;
    c->height   = height;
    c->op_count = 0;
    buf_init(&c->ops);
    c->js_out   = NULL;
    c->html_out = NULL;
    return c;
}

void canvas_free(TkCanvas *c)
{
    if (!c) return;
    free(c->id);
    buf_free(&c->ops);
    free(c->js_out);
    free(c->html_out);
    free(c);
}

/* -----------------------------------------------------------------------
 * Internal: append one JS line and increment op counter
 * ----------------------------------------------------------------------- */

static void append_op(TkCanvas *c, const char *stmt)
{
    buf_append(&c->ops, stmt);
    c->op_count++;
    /* invalidate cached outputs */
    free(c->js_out);   c->js_out   = NULL;
    free(c->html_out); c->html_out = NULL;
}

/* -----------------------------------------------------------------------
 * Drawing functions
 * ----------------------------------------------------------------------- */

TkCanvas *canvas_fill_rect(TkCanvas *c, double x, double y, double w, double h, const char *color)
{
    if (!c) return NULL;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "_c.fillStyle='%s'; _c.fillRect(%g,%g,%g,%g);\n",
             color, x, y, w, h);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_stroke_rect(TkCanvas *c, double x, double y, double w, double h, const char *color, double lw)
{
    if (!c) return NULL;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "_c.strokeStyle='%s'; _c.lineWidth=%g; _c.strokeRect(%g,%g,%g,%g);\n",
             color, lw, x, y, w, h);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_clear_rect(TkCanvas *c, double x, double y, double w, double h)
{
    if (!c) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.clearRect(%g,%g,%g,%g);\n", x, y, w, h);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_fill_text(TkCanvas *c, const char *text, double x, double y, const char *font, const char *color)
{
    if (!c) return NULL;
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "_c.font='%s'; _c.fillStyle='%s'; _c.fillText('%s',%g,%g);\n",
             font, color, text, x, y);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_begin_path(TkCanvas *c)
{
    if (!c) return NULL;
    append_op(c, "_c.beginPath();\n");
    return c;
}

TkCanvas *canvas_move_to(TkCanvas *c, double x, double y)
{
    if (!c) return NULL;
    char buf[128];
    snprintf(buf, sizeof(buf), "_c.moveTo(%g,%g);\n", x, y);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_line_to(TkCanvas *c, double x, double y)
{
    if (!c) return NULL;
    char buf[128];
    snprintf(buf, sizeof(buf), "_c.lineTo(%g,%g);\n", x, y);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_arc(TkCanvas *c, double x, double y, double r, double start, double end)
{
    if (!c) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.arc(%g,%g,%g,%g,%g);\n", x, y, r, start, end);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_close_path(TkCanvas *c)
{
    if (!c) return NULL;
    append_op(c, "_c.closePath();\n");
    return c;
}

TkCanvas *canvas_fill(TkCanvas *c, const char *color)
{
    if (!c) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.fillStyle='%s'; _c.fill();\n", color);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_stroke(TkCanvas *c, const char *color, double lw)
{
    if (!c) return NULL;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "_c.strokeStyle='%s'; _c.lineWidth=%g; _c.stroke();\n",
             color, lw);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_draw_image(TkCanvas *c, const char *src, double x, double y, double w, double h)
{
    if (!c) return NULL;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "_c.drawImage(new Image()/* src=%s */,%g,%g,%g,%g);\n",
             src, x, y, w, h);
    append_op(c, buf);
    return c;
}

TkCanvas *canvas_set_alpha(TkCanvas *c, double alpha)
{
    if (!c) return NULL;
    char buf[64];
    snprintf(buf, sizeof(buf), "_c.globalAlpha=%g;\n", alpha);
    append_op(c, buf);
    return c;
}

/* -----------------------------------------------------------------------
 * Output
 * ----------------------------------------------------------------------- */

const char *canvas_to_js(TkCanvas *c)
{
    if (!c) return NULL;
    if (c->js_out) return c->js_out;

    /* preamble: "const _c = document.getElementById('{id}').getContext('2d');\n" */
    char preamble[512];
    snprintf(preamble, sizeof(preamble),
             "const _c = document.getElementById('%s').getContext('2d');\n",
             c->id);

    size_t plen = strlen(preamble);
    size_t olen = c->ops.len;
    c->js_out = (char *)malloc(plen + olen + 1);
    if (!c->js_out) return NULL;
    memcpy(c->js_out, preamble, plen);
    memcpy(c->js_out + plen, c->ops.data, olen + 1); /* includes NUL */
    return c->js_out;
}

const char *canvas_to_html(TkCanvas *c)
{
    if (!c) return NULL;
    if (c->html_out) return c->html_out;

    const char *js = canvas_to_js(c);
    if (!js) return NULL;

    /* <canvas id="{id}" width="{w}" height="{h}"></canvas>\n<script>\n{js}\n</script> */
    char tag[256];
    snprintf(tag, sizeof(tag),
             "<canvas id=\"%s\" width=\"%u\" height=\"%u\"></canvas>\n<script>\n",
             c->id, c->width, c->height);

    const char *close = "\n</script>";
    size_t tlen  = strlen(tag);
    size_t jslen = strlen(js);
    size_t clen  = strlen(close);
    c->html_out = (char *)malloc(tlen + jslen + clen + 1);
    if (!c->html_out) return NULL;
    memcpy(c->html_out,               tag,   tlen);
    memcpy(c->html_out + tlen,        js,    jslen);
    memcpy(c->html_out + tlen + jslen, close, clen + 1);
    return c->html_out;
}

uint64_t canvas_op_count(TkCanvas *c)
{
    if (!c) return 0;
    return c->op_count;
}

/* -----------------------------------------------------------------------
 * Transforms, state stack, style setters, curves, gradients (Story 34.5.2)
 * ----------------------------------------------------------------------- */

void canvas_translate(TkCanvas *c, double dx, double dy)
{
    if (!c) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "_c.translate(%g,%g);\n", dx, dy);
    append_op(c, buf);
}

void canvas_rotate(TkCanvas *c, double angle)
{
    if (!c) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "_c.rotate(%g);\n", angle);
    append_op(c, buf);
}

void canvas_scale(TkCanvas *c, double sx, double sy)
{
    if (!c) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "_c.scale(%g,%g);\n", sx, sy);
    append_op(c, buf);
}

void canvas_save(TkCanvas *c)
{
    if (!c) return;
    append_op(c, "_c.save();\n");
}

void canvas_restore(TkCanvas *c)
{
    if (!c) return;
    append_op(c, "_c.restore();\n");
}

void canvas_fill_style(TkCanvas *c, const char *color)
{
    if (!c || !color) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.fillStyle='%s';\n", color);
    append_op(c, buf);
}

void canvas_stroke_style(TkCanvas *c, const char *color)
{
    if (!c || !color) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.strokeStyle='%s';\n", color);
    append_op(c, buf);
}

void canvas_line_width(TkCanvas *c, double width)
{
    if (!c) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "_c.lineWidth=%g;\n", width);
    append_op(c, buf);
}

void canvas_quadratic_to(TkCanvas *c, double cpx, double cpy, double x, double y)
{
    if (!c) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.quadraticCurveTo(%g,%g,%g,%g);\n", cpx, cpy, x, y);
    append_op(c, buf);
}

void canvas_bezier_to(TkCanvas *c, double cp1x, double cp1y,
                       double cp2x, double cp2y, double x, double y)
{
    if (!c) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "_c.bezierCurveTo(%g,%g,%g,%g,%g,%g);\n",
             cp1x, cp1y, cp2x, cp2y, x, y);
    append_op(c, buf);
}

const char *canvas_gradient_linear(TkCanvas *c, double x0, double y0,
                                    double x1, double y1)
{
    if (!c) return NULL;

    /* Build a unique gradient ID using the current op count */
    char id[32];
    snprintf(id, sizeof(id), "grad_%llu", (unsigned long long)c->op_count);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "var %s=_c.createLinearGradient(%g,%g,%g,%g);\n",
             id, x0, y0, x1, y1);
    append_op(c, buf);

    return strdup(id);
}
