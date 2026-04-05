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

/* tmpl_vars — construct a TkTmplVar array from parallel key/value arrays.
 * nvar is the number of entries.  The returned array is heap-allocated;
 * caller owns it and must free() it.  The key/value pointers are borrowed
 * from the caller's arrays and must remain valid while the result is in use.
 * Returns NULL on allocation failure. */
TkTmplVar  *tmpl_vars(const char *const *keys, const char *const *values,
                       uint64_t nvar);

/* tmpl_html — construct an HTML element string.
 * tag is the element name (e.g. "div").
 * attr_keys/attr_values are parallel arrays of attribute name/value pairs
 * with nattr entries.  children is an array of nchild already-rendered
 * child strings that are concatenated inside the element.
 * Returns a heap-allocated string; caller owns it.  Returns NULL on
 * allocation failure. */
const char *tmpl_html(const char *tag,
                      const char *const *attr_keys,
                      const char *const *attr_values,
                      uint64_t nattr,
                      const char *const *children,
                      uint64_t nchild);

/* tmpl_renderfile — load a template from the file at path, compile it,
 * render with the given variable bindings, and return the result.
 * Returns a heap-allocated string on success; caller owns it.
 * Returns NULL if the file cannot be read or compilation/rendering fails. */
const char *tmpl_renderfile(const char *path, const TkTmplVar *vars,
                            uint64_t nvar);

/* -----------------------------------------------------------------------
 * Partials — Story 33.1.2
 * ----------------------------------------------------------------------- */

/* tmpl_register_partial — compile source and store it in the global partial
 * registry under name.  A subsequent call with the same name replaces the
 * previous entry.  At most 64 partials may be registered. */
void tmpl_register_partial(const char *name, const char *source);

/* -----------------------------------------------------------------------
 * Helpers — Story 33.1.2
 * ----------------------------------------------------------------------- */

/* Helper function type: receives a variable value string and returns a
 * transformed string.  The returned pointer must remain valid until the
 * render call that produced it completes; returning a heap-allocated string
 * is the safe choice.  Returning the same pointer (no transform) is also
 * valid. */
typedef const char *(*TkTmplHelperFn)(const char *value);

/* tmpl_register_helper — register a helper function under name.
 * A subsequent call with the same name replaces the previous entry.
 * At most 64 helpers may be registered. */
void tmpl_register_helper(const char *name, TkTmplHelperFn fn);

/* .tki compatibility aliases (tpl.* → tpl_*) */
#define tpl_compile            tmpl_compile
#define tpl_free               tmpl_free
#define tpl_render             tmpl_render
#define tpl_renderhtml         tmpl_renderhtml
#define tpl_vars               tmpl_vars
#define tpl_html               tmpl_html
#define tpl_escape             tmpl_escape
#define tpl_renderfile         tmpl_renderfile
#define tpl_register_partial   tmpl_register_partial
#define tpl_register_helper    tmpl_register_helper

#endif /* TK_STDLIB_TEMPLATE_H */
