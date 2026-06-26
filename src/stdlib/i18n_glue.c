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
static I18nBundle g_i18n_bundle = {NULL};

int64_t tk_i18n_empty_w(int64_t dummy) {
    (void)dummy;
    return 0; /* No parameterless i18n constructor in i18n.h */
}

int64_t tk_i18n_str_w(int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(g_i18n_bundle, (const char *)(intptr_t)key);
}

int64_t tk_i18n_load_w(int64_t path) {
    if (!path) return 0;
    const char *p = (const char *)(intptr_t)path;
    const char *locale = i18n_locale();
    I18nBundleResult r = i18n_load(p, locale ? locale : "en");
    if (r.is_err) return 0;
    g_i18n_bundle = r.ok;
    return (int64_t)(intptr_t)g_i18n_bundle.data;
}

int64_t tk_i18n_get_w(int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(g_i18n_bundle, (const char *)(intptr_t)key);
}

int64_t tk_i18n_fmt_w(int64_t key, int64_t args) {
    if (!key) return 0;
    const char *k = (const char *)(intptr_t)key;
    const char *a = args ? (const char *)(intptr_t)args : "";
    return (int64_t)(intptr_t)i18n_fmt(g_i18n_bundle, k, a);
}

int64_t tk_i18n_locale_w(int64_t loc) {
    (void)loc;
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

int64_t tk_i18n_localize_w(int64_t key, int64_t locale) {
    (void)locale;
    return tk_i18n_get_w(key);
}

int64_t tk_i18n_translate_w(int64_t key, int64_t lang) {
    (void)lang;
    return tk_i18n_get_w(key);
}
