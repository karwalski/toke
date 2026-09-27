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

#include "tk_runtime.h"   /* 127.238: tk_current_error + tk_err_box */

/* 127.238: the error slot (runtime-abi.md §7.2).  These `T!tmplerr` wrappers
 * returned a bare 0 on failure, so only the value sentinel saw it and the
 * $err arm bound nil (`e.msg` was RT005).  The failure value is unchanged
 * (0, every T here is a handle or pointer); it now also stores a tmplerr
 * record {msg; …} — the other fields 0 — and every ok return clears the
 * slot, so the wrapper is a slot wrapper (llvm.c, derived from the .tki). */
static int64_t tpl_slot(int64_t v, const char *msg) {
    if (v) { tk_current_error = 0; return v; }
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

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

    /* 124.3 (ADR-0011): escape slot values by default. `tpl.render` previously
     * mapped to tmpl_render (escape OFF), so `{{name}}` interpolated untrusted
     * data raw into the HTML output — a stored/reflected XSS sink. Point it at
     * tmpl_renderhtml (escape ON): only interpolated *values* are HTML-escaped;
     * the template's own literal markup is preserved. Raw output must be an
     * explicit opt-in (follow-up: a `|raw` helper / tpl.renderraw). */
    const char *result = tmpl_renderhtml(t, vars, nvar);
    free(vars);
    tmpl_free(t);
    return result;
}

static int64_t tk_template_render_impl(int64_t tpl, int64_t data) {
    return (int64_t)(intptr_t)template_render_impl(tpl, data);
}
int64_t tk_template_render_w(int64_t tpl, int64_t data) {
    return tpl_slot(tk_template_render_impl(tpl, data),
        "tpl.render: the template is missing or could not be compiled");
}

/* tpl.vars(@($str:$str)) -> $tmplvars — the map already carries the key/value
 * pairs in the layout render/renderfile expect, so the bundle is the map. */
int64_t tk_template_vars_w(int64_t map) { return map; }

/* tpl.renderfile(path; $tmplvars) -> $str!$tmplerr — read+compile+render the
 * file with the bundle. Returns the rendered string, or 0 (err sentinel). */
static int64_t tk_template_renderfile_impl(int64_t path, int64_t data) {
    if (!path) return 0;
    uint64_t nvar = 0;
    TkTmplVar *vars = unpack_tmplvars(data, &nvar);
    const char *res = tmpl_renderfile((const char *)(intptr_t)path, vars, nvar);
    free(vars);
    return (int64_t)(intptr_t)res;
}
int64_t tk_template_renderfile_w(int64_t path, int64_t data) {
    return tpl_slot(tk_template_renderfile_impl(path, data),
        "tpl.renderfile: the template file could not be read or rendered");
}

/* tpl.compile(str) -> $tmpl!$tmplerr — compile to a handle (0 on error). */
static int64_t tk_template_compile_impl(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)tmpl_compile((const char *)(intptr_t)src);
}
int64_t tk_template_compile_w(int64_t src) {
    return tpl_slot(tk_template_compile_impl(src),
        "tpl.compile: the template could not be compiled");
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

/* tpl_ aliases call the same implementations */
static int64_t tk_tpl_render_impl(int64_t tpl, int64_t data) {
    return (int64_t)(intptr_t)template_render_impl(tpl, data);
}
int64_t tk_tpl_render_w(int64_t tpl, int64_t data) {
    return tpl_slot(tk_tpl_render_impl(tpl, data),
        "tpl.render: the template is missing or could not be compiled");
}

int64_t tk_tpl_load_w(int64_t path) {
    return tk_template_load_w(path);
}
