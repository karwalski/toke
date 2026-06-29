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

/* Unpack a toke string→string map / $tmplvars bundle into a TkTmplVar[].
 * tpl.vars is the identity on the map (see tk_template_vars_w), so the same
 * unpack serves render and renderfile. Caller frees the returned array. */
static TkTmplVar *unpack_tmplvars(int64_t data, uint64_t *nout) {
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

static const char *template_render_impl(int64_t tpl, int64_t data) {
    if (!tpl) return NULL;
    const char *source = (const char *)(intptr_t)tpl;
    TkTmpl *t = tmpl_compile(source);
    if (!t) return NULL;

    uint64_t nvar = 0;
    TkTmplVar *vars = unpack_tmplvars(data, &nvar);

    const char *result = tmpl_render(t, vars, nvar);
    free(vars);
    tmpl_free(t);
    return result;
}

int64_t tk_template_render_w(int64_t tpl, int64_t data) {
    return (int64_t)(intptr_t)template_render_impl(tpl, data);
}

/* tpl.vars(@($str:$str)) -> $tmplvars — the map already carries the key/value
 * pairs in the layout render/renderfile expect, so the bundle is the map. */
int64_t tk_template_vars_w(int64_t map) { return map; }

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

/* tpl.compile(str) -> $tmpl!$tmplerr — compile to a handle (0 on error). */
int64_t tk_template_compile_w(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)tmpl_compile((const char *)(intptr_t)src);
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
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    TkTmpl *t = tmpl_compile(buf);
    free(buf);
    return (int64_t)(intptr_t)t;
}

/* tpl_ aliases call the same implementations */
int64_t tk_tpl_render_w(int64_t tpl, int64_t data) {
    return (int64_t)(intptr_t)template_render_impl(tpl, data);
}

int64_t tk_tpl_load_w(int64_t path) {
    return tk_template_load_w(path);
}
