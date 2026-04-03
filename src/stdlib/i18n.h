#ifndef TK_STDLIB_I18N_H
#define TK_STDLIB_I18N_H

/*
 * i18n.h — C interface for the std.i18n standard library module.
 *
 * Provides string externalisation and internationalisation support.
 * String bundles are loaded from TOON, YAML, or JSON files based on
 * a base path and locale identifier.
 *
 * File resolution order for i18n.load("strings", "en"):
 *   1. strings.en.toon
 *   2. strings.en.yaml
 *   3. strings.en.json
 *   4. strings.toon  (fallback to default)
 *   5. strings.yaml
 *   6. strings.json
 *
 * Bundle format (TOON example — strings.en.toon):
 *   strings[3]{key,value}:
 *   greeting|Hello
 *   farewell|Goodbye
 *   welcome|Welcome, {name}!
 *
 * Bundle format (YAML example — strings.en.yaml):
 *   greeting: Hello
 *   farewell: Goodbye
 *   welcome: "Welcome, {name}!"
 *
 * Bundle format (JSON example — strings.en.json):
 *   {"greeting":"Hello","farewell":"Goodbye","welcome":"Welcome, {name}!"}
 *
 * Type mappings:
 *   I18nBundle       = struct { const char *data; }
 *   I18nErr          = struct { I18nErrKind kind; const char *msg; }
 *
 * Story: 6.3.5
 */

#include <stdint.h>

typedef struct { const char *data; } I18nBundle;

typedef enum {
    I18N_ERR_NOT_FOUND = 0,
    I18N_ERR_PARSE     = 1
} I18nErrKind;

typedef struct { I18nErrKind kind; const char *msg; } I18nErr;
typedef struct { I18nBundle ok; int is_err; I18nErr err; } I18nBundleResult;

/*
 * i18n_load — load a string bundle for the given locale.
 * base_path: file path prefix (e.g., "strings" or "lang/messages")
 * locale:    locale code (e.g., "en", "fr", "ja")
 * Searches for base_path.locale.{toon,yaml,json}, falling back to
 * base_path.{toon,yaml,json} if locale-specific file not found.
 */
I18nBundleResult i18n_load(const char *base_path, const char *locale);

/*
 * i18n_get — look up a string key in the bundle.
 * Returns the string value, or the key itself if not found (safe fallback).
 */
const char *i18n_get(I18nBundle bundle, const char *key);

/*
 * i18n_fmt — look up a key and substitute {placeholder} tokens.
 * args: pipe-delimited "name=value|name2=value2" string.
 * Returns formatted string with placeholders replaced.
 */
const char *i18n_fmt(I18nBundle bundle, const char *key, const char *args);

/*
 * i18n_locale — return the system locale string (from LANG/LC_ALL env).
 */
const char *i18n_locale(void);

#endif /* TK_STDLIB_I18N_H */
