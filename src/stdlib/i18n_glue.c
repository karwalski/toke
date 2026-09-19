/*
 * i18n_glue.c — i64-ABI wrappers for std.i18n (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=i18n:std.i18n` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "i18n.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int64_t tk_array_append_w(int64_t,int64_t);
extern int64_t tk_str_slice_w(int64_t,int64_t,int64_t);

/* 114.53/114.54/127.67: report failure through this flag and still return the
 * real value; defined in tk_runtime.c. */
extern int64_t tk_current_error;

/*
 * Story 136.19 — the bundle and the locale are arguments, not process state.
 *
 * stdlib/i18n.tki, docs/stdlib/i18n.md and i18n.h all declare
 * i18n.load(base; locale), i18n.str/get(bundle; key) and
 * i18n.fmt(bundle; key; args), and i18n_load/i18n_get/i18n_fmt in i18n.c have
 * always taken them.  The glue dropped every one: load ignored the locale the
 * caller asked for and used i18n_locale(), which reads $LC_ALL/$LANG, and
 * stashed the result in `static I18nBundle g_i18n_bundle` -- so str, get and
 * fmt answered from whichever bundle was loaded LAST, anywhere in the
 * process.  Two locales at once, which is the entire point of loading a
 * bundle per locale, could not be expressed: a second load silently
 * repointed the first one's handle.
 *
 * The handle that crosses the ABI is the bundle's `data` pointer, which is
 * what tk_i18n_load_w already returned; I18nBundle has exactly one field, so
 * reconstructing it here is faithful rather than an offset-0 accident.
 */
static I18nBundle bundle_of(int64_t h) {
    I18nBundle b;
    b.data = (const char *)(intptr_t)h;
    return b;
}

int64_t tk_i18n_empty_w(void) {
    return 0; /* an empty bundle: i18n_get falls back to returning the key */
}

int64_t tk_i18n_str_w(int64_t bundle, int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(bundle_of(bundle), (const char *)(intptr_t)key);
}

int64_t tk_i18n_load_w(int64_t path, int64_t locale) {
    if (!path) { tk_current_error = 1; return 0; }
    const char *p = (const char *)(intptr_t)path;
    const char *loc = locale ? (const char *)(intptr_t)locale : NULL;
    if (!loc || !*loc) loc = i18n_locale();       /* only when none was asked for */
    I18nBundleResult r = i18n_load(p, loc ? loc : "en");
    if (r.is_err) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok.data;
}

int64_t tk_i18n_get_w(int64_t bundle, int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(bundle_of(bundle), (const char *)(intptr_t)key);
}

int64_t tk_i18n_fmt_w(int64_t bundle, int64_t key, int64_t args) {
    if (!key) return 0;
    const char *k = (const char *)(intptr_t)key;
    const char *a = args ? (const char *)(intptr_t)args : "";
    return (int64_t)(intptr_t)i18n_fmt(bundle_of(bundle), k, a);
}

int64_t tk_i18n_locale_w(void) {
    return (int64_t)(intptr_t)i18n_locale();
}

int64_t tk_i18n_newstrarray_w(int64_t dummy) {
    (void)dummy;
    return 0; /* No standalone StrArray constructor in i18n.h */
}

int64_t tk_i18n_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }

int64_t tk_i18n_substr_w(int64_t s, int64_t start, int64_t end_) { return tk_str_slice_w(s, start, end_); }

int64_t tk_i18n_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}

/* localize(bundle; key) / translate(bundle; key) — undocumented aliases of
 * i18n.get kept for existing callers. They too read the global bundle and
 * discarded their second argument; they now take the bundle like everything
 * else. */
int64_t tk_i18n_localize_w(int64_t bundle, int64_t key) {
    return tk_i18n_get_w(bundle, key);
}

int64_t tk_i18n_translate_w(int64_t bundle, int64_t key) {
    return tk_i18n_get_w(bundle, key);
}
