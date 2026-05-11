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
static const char *template_render_impl(int64_t tpl, int64_t data) {
    if (!tpl) return NULL;
    const char *source = (const char *)(intptr_t)tpl;
    TkTmpl *t = tmpl_compile(source);
    if (!t) return NULL;

    uint64_t nvar = 0;
    TkTmplVar *vars = NULL;
    if (data) {
        int64_t *ptr = (int64_t *)(intptr_t)data;
        int64_t count = ptr[-1];
        if (count > 0 && (count & 1) == 0) {
            nvar = (uint64_t)(count / 2);
            vars = (TkTmplVar *)malloc(nvar * sizeof(TkTmplVar));
            if (vars) {
                for (uint64_t i = 0; i < nvar; i++) {
                    vars[i].key   = (const char *)(intptr_t)ptr[i * 2];
                    vars[i].value = (const char *)(intptr_t)ptr[i * 2 + 1];
                }
            }
        }
    }

    const char *result = tmpl_render(t, vars, nvar);
    free(vars);
    tmpl_free(t);
    return result;
}

int64_t tk_template_render_w(int64_t tpl, int64_t data) {
    return (int64_t)(intptr_t)template_render_impl(tpl, data);
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
