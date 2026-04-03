/*
 * test_i18n.c — Unit tests for std.i18n (i18n.h / i18n.c).
 *
 * Story: 6.3.5
 *
 * Build: cc -I../../src/stdlib test_i18n.c ../../src/stdlib/i18n.c -o test_i18n
 */

#include "i18n.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label) \
    do { if (cond) { printf("PASS  %s\n", label); g_pass++; } \
         else      { printf("FAIL  %s\n", label); g_fail++; } } while(0)

/* Write a temporary file */
static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

int main(void) {
    /* Create test bundle files in /tmp */
    write_file("/tmp/test_strings.en.yaml",
        "greeting: Hello\n"
        "farewell: Goodbye\n"
        "welcome: \"Welcome, {name}!\"\n"
    );

    write_file("/tmp/test_strings.fr.yaml",
        "greeting: Bonjour\n"
        "farewell: Au revoir\n"
        "welcome: \"Bienvenue, {name} !\"\n"
    );

    write_file("/tmp/test_strings.en.toon",
        "strings[3]{key,value}:\n"
        "hello|Hello World\n"
        "bye|See you later\n"
        "msg|Hi {user}, you have {count} items\n"
    );

    write_file("/tmp/test_strings.en.json",
        "{\"greeting\":\"Hello JSON\",\"farewell\":\"Bye JSON\"}\n"
    );

    /* --- Load TOON bundle (highest priority) --- */
    I18nBundleResult tr = i18n_load("/tmp/test_strings", "en");
    CHECK(!tr.is_err, "i18n_load TOON bundle");

    /* --- Get string from TOON --- */
    const char *hello = i18n_get(tr.ok, "hello");
    CHECK(hello && strcmp(hello, "Hello World") == 0, "i18n_get TOON hello");

    const char *bye = i18n_get(tr.ok, "bye");
    CHECK(bye && strcmp(bye, "See you later") == 0, "i18n_get TOON bye");

    /* --- Missing key returns key itself --- */
    const char *missing = i18n_get(tr.ok, "nonexistent");
    CHECK(missing && strcmp(missing, "nonexistent") == 0, "i18n_get missing key returns key");

    /* --- Format with placeholders --- */
    const char *formatted = i18n_fmt(tr.ok, "msg", "user=Alice|count=5");
    CHECK(formatted && strcmp(formatted, "Hi Alice, you have 5 items") == 0,
          "i18n_fmt TOON placeholder substitution");

    /* --- Load YAML bundle (remove TOON to test fallback) --- */
    remove("/tmp/test_strings.en.toon");
    I18nBundleResult yr = i18n_load("/tmp/test_strings", "en");
    CHECK(!yr.is_err, "i18n_load YAML bundle (TOON removed)");

    const char *yg = i18n_get(yr.ok, "greeting");
    CHECK(yg && strcmp(yg, "Hello") == 0, "i18n_get YAML greeting");

    /* --- French locale --- */
    I18nBundleResult fr = i18n_load("/tmp/test_strings", "fr");
    CHECK(!fr.is_err, "i18n_load French locale");

    const char *fg = i18n_get(fr.ok, "greeting");
    CHECK(fg && strcmp(fg, "Bonjour") == 0, "i18n_get French greeting");

    /* --- Format with French template --- */
    const char *fw = i18n_fmt(fr.ok, "welcome", "name=Pierre");
    CHECK(fw && strstr(fw, "Pierre"), "i18n_fmt French welcome has name");

    /* --- Load JSON bundle (remove YAML) --- */
    remove("/tmp/test_strings.en.yaml");
    I18nBundleResult jr = i18n_load("/tmp/test_strings", "en");
    CHECK(!jr.is_err, "i18n_load JSON bundle (YAML removed)");

    const char *jg = i18n_get(jr.ok, "greeting");
    CHECK(jg && strcmp(jg, "Hello JSON") == 0, "i18n_get JSON greeting");

    /* --- File not found --- */
    I18nBundleResult nr = i18n_load("/tmp/nonexistent_bundle", "en");
    CHECK(nr.is_err && nr.err.kind == I18N_ERR_NOT_FOUND, "i18n_load not found");

    /* --- i18n_locale returns something --- */
    const char *loc = i18n_locale();
    CHECK(loc && strlen(loc) > 0, "i18n_locale returns non-empty");

    /* --- Format with missing key returns key --- */
    const char *fmissing = i18n_fmt(tr.ok, "no_such_key", "x=1");
    CHECK(fmissing && strcmp(fmissing, "no_such_key") == 0, "i18n_fmt missing key returns key");

    /* Cleanup */
    remove("/tmp/test_strings.fr.yaml");
    remove("/tmp/test_strings.en.json");

    printf("\n---\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
