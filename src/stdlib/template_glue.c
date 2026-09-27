/*
 * template_glue.c — i64-ABI wrappers for std.template module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.template.
 */

#include "template.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * tk_template_render_w — compile the template source string (tpl) and
 * render it with the variable bindings packed in data.
 *
 * data layout (toke ABI):  ptr[-1] = count (i64, must be even)
 *                           ptr[0..count-1] = alternating key/value i64 ptrs
 * Returns a heap-allocated rendered string, or 0 on failure.
 */
/* A toke `@($str:$str)` map is a tk_map object, NOT a flat array: it is the
 * TkMapImpl built by tk_map_new/tk_map_put (see collections_glue.c). Mirror its
 * layout here so we can iterate its entries. (The previous flat-array unpack
 * read garbage, so template variables never actually bound — a latent bug.) */
typedef struct { int64_t key; int64_t val; } TkMapEntryT;
typedef struct { TkMapEntryT *entries; int len; int cap; } TkMapImplT;

/* Unpack a toke string→string map into a TkTmplVar[]. Caller frees the
 * returned array. */
static TkTmplVar *unpack_map(int64_t data, uint64_t *nout) {
    *nout = 0;
    if (!data) return NULL;
    const TkMapImplT *m = (const TkMapImplT *)(intptr_t)data;
    if (m->len <= 0 || !m->entries) return NULL;
    uint64_t nvar = (uint64_t)m->len;
    TkTmplVar *vars = (TkTmplVar *)malloc(nvar * sizeof(TkTmplVar));
    if (!vars) return NULL;
    for (uint64_t i = 0; i < nvar; i++) {
        vars[i].key   = (const char *)(intptr_t)m->entries[i].key;
        vars[i].value = (const char *)(intptr_t)m->entries[i].val;
    }
    *nout = nvar;
    return vars;
}

/* 135.30: a $tmplvars is a TkTmplVarsRec; its entries is the map. */
static TkTmplVar *unpack_tmplvars(int64_t data, uint64_t *nout) {
    *nout = 0;
    if (!data) return NULL;
    return unpack_map(((const TkTmplVarsRec *)(intptr_t)data)->entries, nout);
}

/* render `source` with the map `map` (both already unpacked from records) */
static const char *template_render_src(const char *source, TkTmplVar *vars,
                                       uint64_t nvar) {
    if (!source) return NULL;
    TkTmpl *t = tmpl_compile(source);
    if (!t) return NULL;

    /* 124.3 (ADR-0011): escape slot values by default. `tpl.render` previously
     * mapped to tmpl_render (escape OFF), so `{{name}}` interpolated untrusted
     * data raw into the HTML output — a stored/reflected XSS sink. Point it at
     * tmpl_renderhtml (escape ON): only interpolated *values* are HTML-escaped;
     * the template's own literal markup is preserved. Raw output must be an
     * explicit opt-in (follow-up: a `|raw` helper / tpl.renderraw). */
    const char *result = tmpl_renderhtml(t, vars, nvar);
    tmpl_free(t);
    return result;
}

/* tpl.render($tmpl; $tmplvars) -- 135.30: both are records (TkTmplRec,
 * TkTmplVarsRec); the template is compiled from its src. */
int64_t tk_template_render_w(int64_t tpl, int64_t data) {
    if (!tpl) return 0;
    uint64_t nvar = 0;
    TkTmplVar *vars = unpack_tmplvars(data, &nvar);
    const char *r = template_render_src(((const TkTmplRec *)(intptr_t)tpl)->src,
                                        vars, nvar);
    free(vars);
    return (int64_t)(intptr_t)r;
}

/* tpl.vars(@($str:$str)) -> $tmplvars, a TkTmplVarsRec holding the map. */
int64_t tk_template_vars_w(int64_t map) {
    TkTmplVarsRec *v = (TkTmplVarsRec *)malloc(sizeof *v);
    if (!v) return 0;
    v->entries = map;
    return (int64_t)(intptr_t)v;
}

/* tpl.renderfile(path; $tmplvars) -> $str!$tmplerr — read+compile+render the
 * file with the bundle. Returns the rendered string, or 0 (err sentinel). */
int64_t tk_template_renderfile_w(int64_t path, int64_t data) {
    if (!path) return 0;
    uint64_t nvar = 0;
    TkTmplVar *vars = unpack_tmplvars(data, &nvar);
    const char *res = tmpl_renderfile((const char *)(intptr_t)path, vars, nvar);
    free(vars);
    return (int64_t)(intptr_t)res;
}

/* tpl.compile(str) -> $tmpl!$tmplerr -- 135.30: a TkTmplRec {id; src}
 * (0 on error).  The source is compiled here to check it, and again by
 * tpl.render, so a $tmpl literal behaves as a compiled one. */
int64_t tk_template_compile_w(int64_t src) {
    static int64_t next_id = 0;
    if (!src) return 0;
    const char *s = (const char *)(intptr_t)src;
    TkTmpl *t = tmpl_compile(s);
    if (!t) return 0;
    tmpl_free(t);
    TkTmplRec *rec = (TkTmplRec *)malloc(sizeof *rec);
    char *copy = strdup(s);
    if (!rec || !copy) { free(rec); free(copy); return 0; }
    rec->id  = ++next_id;
    rec->src = copy;
    return (int64_t)(intptr_t)rec;
}

/* tpl.escape(str) -> str — HTML-escape. */
int64_t tk_template_escape_w(int64_t s) {
    if (!s) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)tmpl_escape((const char *)(intptr_t)s);
}

/*
 * tk_template_load_w — load a template file from disk and return the
 * compiled TkTmpl handle as an opaque pointer.
 * The toke runtime can later pass this to render.
 * For simplicity, we compile and return the handle.
 */
int64_t tk_template_load_w(int64_t path) {
    if (!path) return 0;
    const char *p = (const char *)(intptr_t)path;
    /* Read file contents, compile, and return handle */
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    /* 135.3c: every step checked.  The seeks were unchecked (rewind() can
     * only report through errno, which nothing read -- clang unix.Errno),
     * and a short fread left buf[nread..sz) uninitialised but compiled. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);
    TkTmpl *t = tmpl_compile(buf);
    free(buf);
    return (int64_t)(intptr_t)t;
}

/* tpl_ aliases (i64 ABI, not in template.tki): source text and a bare map,
 * as they always took them. */
int64_t tk_tpl_render_w(int64_t tpl, int64_t data) {
    uint64_t nvar = 0;
    TkTmplVar *vars = unpack_map(data, &nvar);
    const char *r = template_render_src((const char *)(intptr_t)tpl, vars, nvar);
    free(vars);
    return (int64_t)(intptr_t)r;
}

int64_t tk_tpl_load_w(int64_t path) {
    return tk_template_load_w(path);
}
