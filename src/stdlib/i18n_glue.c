/* SPDX-License-Identifier: MIT */
/*
 * i18n_glue.c — i64-ABI wrappers for std.i18n (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=i18n:std.i18n` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "i18n.h"
#include "capabilities.h"   /* 121.42: i18n.load is an fs.read sink */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern int64_t tk_array_append_w(int64_t,int64_t);
extern int64_t tk_str_slice_w(int64_t,int64_t,int64_t);

/* 114.53/114.54/127.67: report failure through this flag and still return the
 * real value; defined in tk_runtime.c. */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

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
 * 135.30: the handle that crosses the ABI is a pointer to an I18nBundle --
 * the $I18nBundle record, whose `.data` toke reads as slot 0 -- whether
 * i18n.load or a toke literal built it.  It used to be the bundle's `data`
 * pointer itself, so `b.data` read the text as a pointer (SIGSEGV).  0 (the
 * i64-ABI i18n.empty) is the empty bundle.
 */
static I18nBundle bundle_of(int64_t h) {
    I18nBundle b;
    b.data = h ? ((const I18nBundle *)(intptr_t)h)->data : NULL;
    return b;
}

int64_t tk_i18n_empty_w(void) {
    return 0; /* an empty bundle: i18n_get falls back to returning the key */
}

int64_t tk_i18n_str_w(int64_t bundle, int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(bundle_of(bundle), (const char *)(intptr_t)key);
}

/* 138.19: the directory holding "<base>[.<locale>].x" (dirname semantics:
 * "msgs" -> ".", "/a/msgs" -> "/a", "/msgs" -> "/"). */
static const char *i18n_scope_dir(const char *base, const char *loc, char *out, size_t outsz) {
    if (!base) return NULL;
    int w = loc ? snprintf(out, outsz, "%s.%s", base, loc) : snprintf(out, outsz, "%s", base);
    if (w < 0 || (size_t)w >= outsz) return NULL;
    char *sl = strrchr(out, '/');
    if (!sl) snprintf(out, outsz, ".");
    else if (sl == out) out[1] = '\0';
    else *sl = '\0';
    return out;
}

/* 138.19: a locale holding a '/' names files in another directory, which
 * the scope must cover too.  (A helper, so the entry's own gate is the one
 * C674's negative control removes.) */
static void i18n_require_locale_dir(const char *base, const char *loc) {
    char b[4096];
    if (base && loc && strchr(loc, '/'))
        TK_REQUIRE_PATH(TK_CAP_FS_READ, i18n_scope_dir(base, loc, b, sizeof b));
}

/* 121.42: reads the files named by the program -- fs.read, like file.read. */
int64_t tk_i18n_load_w(int64_t path, int64_t locale) {
    const char *p = (const char *)(intptr_t)path;
    const char *loc = locale ? (const char *)(intptr_t)locale : NULL;
    if (p && (!loc || !*loc)) loc = i18n_locale();   /* only when none was asked for */
    {   /* 138.19: i18n_load reads <path>.<locale>.<ext> and <path>.<ext>,
         * siblings of `path`, so the scope check is on the directory that
         * holds them -- and on the locale's, since a locale may hold a '/'. */
        char a[4096];
        TK_REQUIRE_PATH(TK_CAP_FS_READ, i18n_scope_dir(p, NULL, a, sizeof a));
        i18n_require_locale_dir(p, loc);
    }
    if (!path) { tk_current_error = 1; return 0; }
    I18nBundleResult r = i18n_load(p, loc ? loc : "en");
    if (r.is_err) { tk_current_error = 1; return 0; }
    I18nBundle *b = (I18nBundle *)malloc(sizeof *b);
    if (!b) { tk_current_error = 1; return 0; }
    *b = r.ok;
    tk_current_error = 0;
    return (int64_t)(intptr_t)b;
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

int64_t tk_i18n_newstrarray_w(void) {
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
