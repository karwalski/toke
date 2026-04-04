#ifndef TK_STDLIB_TEMPLATE_H
#define TK_STDLIB_TEMPLATE_H

/*
 * template.h — C interface for the std.template standard library module.
 *
 * Provides {{IDENT}} slot-based string templating with optional HTML escaping.
 *
 * Type mappings:
 *   Str     = const char *  (null-terminated UTF-8)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * malloc is permitted; callers own returned strings.
 *
 * Story: 15.1.4
 */

#include <stdint.h>

/* Opaque compiled template handle. */
typedef struct TkTmpl TkTmpl;

/* A single key/value variable binding. */
typedef struct {
    const char *key;
    const char *value;
} TkTmplVar;

/* tmpl_compile — parse source for {{IDENT}} slots and return a compiled
 * template handle.  Returns NULL on allocation failure.
 * Caller must free with tmpl_free(). */
TkTmpl     *tmpl_compile(const char *source);

/* tmpl_free — release all resources owned by a compiled template. */
void        tmpl_free(TkTmpl *t);

/* tmpl_render — render template t substituting {{name}} with the matching
 * value from vars[0..nvar-1].  Missing keys are replaced with "".
 * Returns a heap-allocated string; caller owns it. */
const char *tmpl_render(TkTmpl *t, const TkTmplVar *vars, uint64_t nvar);

/* tmpl_renderhtml — same as tmpl_render but HTML-escapes each variable value
 * before substitution.  Literal text segments are NOT escaped.
 * Returns a heap-allocated string; caller owns it. */
const char *tmpl_renderhtml(TkTmpl *t, const TkTmplVar *vars, uint64_t nvar);

/* tmpl_escape — HTML-escape a string:
 *   & -> &amp;   < -> &lt;   > -> &gt;   " -> &quot;   ' -> &#39;
 * Returns a heap-allocated string; caller owns it. */
const char *tmpl_escape(const char *s);

#endif /* TK_STDLIB_TEMPLATE_H */
