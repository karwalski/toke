/*
 * test_template.c — Unit tests for the std.template C library (Story 15.1.4).
 *
 * Build and run: make test-stdlib-template
 *
 * Tests cover:
 *   - no slots (literal passthrough)
 *   - single slot substitution
 *   - multiple slots
 *   - missing key → empty string, no crash
 *   - repeated slot
 *   - HTML escaping of values in tmpl_renderhtml
 *   - literal text left unescaped in tmpl_renderhtml
 *   - tmpl_escape standalone function
 *   - invalid {{ treated as literal
 *   - empty template
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/template.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* ---- 1. No slots: literal passthrough ---- */
    {
        TkTmpl *t = tmpl_compile("hello");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "hello", "no slots: literal passthrough");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 2. Single slot substitution ---- */
    {
        TkTmpl *t = tmpl_compile("Hello, {{name}}!");
        TkTmplVar vars[] = {{"name", "Alice"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "Hello, Alice!", "single slot: {{name}} -> Alice");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 3. Multiple slots ---- */
    {
        TkTmpl *t = tmpl_compile("{{greeting}}, {{name}}!");
        TkTmplVar vars[] = {{"greeting", "Hello"}, {"name", "Bob"}};
        const char *got = tmpl_render(t, vars, 2);
        ASSERT_STREQ(got, "Hello, Bob!", "multiple slots: greeting + name");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 4. Missing key → empty string, no crash ---- */
    {
        TkTmpl *t = tmpl_compile("Value: {{missing}}.");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "Value: .", "missing key: replaced with empty string");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 5. Repeated slot ---- */
    {
        TkTmpl *t = tmpl_compile("{{x}} and {{x}}");
        TkTmplVar vars[] = {{"x", "y"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "y and y", "repeated slot: {{x}} and {{x}} with x=y");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 6. tmpl_renderhtml: HTML-escapes variable values ---- */
    {
        TkTmpl *t = tmpl_compile("{{content}}");
        TkTmplVar vars[] = {{"content", "<b>bold</b>"}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got, "&lt;b&gt;bold&lt;/b&gt;",
                     "renderhtml: <b>bold</b> -> &lt;b&gt;bold&lt;/b&gt;");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 7. tmpl_renderhtml: literal text left unescaped ---- */
    {
        TkTmpl *t = tmpl_compile("<p>{{name}}</p>");
        TkTmplVar vars[] = {{"name", "Alice"}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got, "<p>Alice</p>",
                     "renderhtml: literal text not escaped, value safe");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 8. tmpl_escape standalone: all five characters ---- */
    {
        const char *got = tmpl_escape("<>&\"'");
        ASSERT_STREQ(got, "&lt;&gt;&amp;&quot;&#39;",
                     "tmpl_escape: <>&\"' -> &lt;&gt;&amp;&quot;&#39;");
        free((void *)got);
    }

    /* ---- 9. Invalid {{ treated as literal ---- */
    {
        TkTmpl *t = tmpl_compile("price: {{ not valid }}");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "price: {{ not valid }}",
                     "invalid {{ treated as literal");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 10. Empty template ---- */
    {
        TkTmpl *t = tmpl_compile("");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "", "empty template renders to empty string");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 11. tmpl_renderhtml: ampersand and quote in value ---- */
    {
        TkTmpl *t = tmpl_compile("{{v}}");
        TkTmplVar vars[] = {{"v", "a&b \"c\""}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got, "a&amp;b &quot;c&quot;",
                     "renderhtml: & and \" escaped in value");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 12. Slot at start and end ---- */
    {
        TkTmpl *t = tmpl_compile("{{a}} middle {{b}}");
        TkTmplVar vars[] = {{"a", "START"}, {"b", "END"}};
        const char *got = tmpl_render(t, vars, 2);
        ASSERT_STREQ(got, "START middle END",
                     "slots at start and end of template");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- report ---- */
    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
}
