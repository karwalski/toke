/*
 * test_template_email_integration.c — Real-world integration test:
 * template-based email rendering with escaped user data (Story 21.1.4).
 *
 * Build: cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *          -o test_template_email_integration \
 *          test_template_email_integration.c ../../src/stdlib/template.c
 *
 * Scenarios:
 *   - Compile email template with {{name}}, {{email}}, {{message}} slots
 *   - Render with normal values
 *   - Render with HTML/XSS payloads via tmpl_renderhtml (safe output)
 *   - Missing variables produce empty string
 *   - Extra variables are ignored
 *   - Repeated placeholder in multi-slot template
 *   - Static template with no placeholders
 *   - tmpl_escape directly on XSS payloads
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

#define ASSERT_NULL_SUBSTR(haystack, needle, msg) \
    ASSERT((haystack) && !strstr((haystack), (needle)), msg)

/* Shared email template source. */
static const char *EMAIL_TMPL =
    "<html><body>"
    "<h1>Hello, {{name}}</h1>"
    "<p>From: {{email}}</p>"
    "<p>{{message}}</p>"
    "</body></html>";

int main(void)
{
    /* ---- 1. Normal rendering ---- */
    {
        TkTmpl *t = tmpl_compile(EMAIL_TMPL);
        ASSERT(t != NULL, "email: compile succeeds");

        TkTmplVar vars[] = {
            {"name",    "Alice"},
            {"email",   "alice@example.com"},
            {"message", "Thanks for signing up!"}
        };
        const char *got = tmpl_render(t, vars, 3);
        ASSERT_STREQ(got,
            "<html><body>"
            "<h1>Hello, Alice</h1>"
            "<p>From: alice@example.com</p>"
            "<p>Thanks for signing up!</p>"
            "</body></html>",
            "email: normal values render correctly");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 2. XSS payloads via tmpl_renderhtml ---- */
    {
        TkTmpl *t = tmpl_compile(EMAIL_TMPL);
        ASSERT(t != NULL, "email-xss: compile succeeds");

        TkTmplVar vars[] = {
            {"name",    "<script>alert(1)</script>"},
            {"email",   "\"quotes\"@example.com"},
            {"message", "<img onerror=alert(1) src=x>"}
        };
        const char *got = tmpl_renderhtml(t, vars, 3);
        ASSERT(got != NULL, "email-xss: renderhtml returns non-NULL");

        /* Literal HTML structure must survive (not escaped). */
        ASSERT(strstr(got, "<html><body>") != NULL,
               "email-xss: literal <html> preserved");
        ASSERT(strstr(got, "<h1>Hello, ") != NULL,
               "email-xss: literal <h1> preserved");

        /* Injected tags must be escaped away. */
        ASSERT_NULL_SUBSTR(got, "<script>",
            "email-xss: no unescaped <script> in output");
        ASSERT_NULL_SUBSTR(got, "</script>",
            "email-xss: no unescaped </script> in output");
        ASSERT_NULL_SUBSTR(got, "<img",
            "email-xss: no unescaped <img in output");
        ASSERT_NULL_SUBSTR(got, "onerror",
            "email-xss: no onerror attribute in output");

        /* Escaped forms must appear. */
        ASSERT(strstr(got, "&lt;script&gt;") != NULL,
               "email-xss: <script> escaped to &lt;script&gt;");
        ASSERT(strstr(got, "&quot;quotes&quot;") != NULL,
               "email-xss: quotes escaped to &quot;");

        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 3. Ampersand payload via tmpl_renderhtml ---- */
    {
        TkTmpl *t = tmpl_compile(
            "<p>{{message}}</p>");
        TkTmplVar vars[] = {{"message", "A & B < C > D"}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got,
            "<p>A &amp; B &lt; C &gt; D</p>",
            "email-amp: ampersand/angle brackets escaped");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 4. Missing variables produce empty string ---- */
    {
        TkTmpl *t = tmpl_compile(EMAIL_TMPL);
        /* Provide only name — email and message are missing. */
        TkTmplVar vars[] = {{"name", "Bob"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got,
            "<html><body>"
            "<h1>Hello, Bob</h1>"
            "<p>From: </p>"
            "<p></p>"
            "</body></html>",
            "email-missing: missing vars yield empty string");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 5. Extra variables are ignored ---- */
    {
        TkTmpl *t = tmpl_compile(EMAIL_TMPL);
        TkTmplVar vars[] = {
            {"name",    "Carol"},
            {"email",   "carol@example.com"},
            {"message", "Hi"},
            {"unused1", "should not appear"},
            {"unused2", "also ignored"}
        };
        const char *got = tmpl_render(t, vars, 5);
        ASSERT_STREQ(got,
            "<html><body>"
            "<h1>Hello, Carol</h1>"
            "<p>From: carol@example.com</p>"
            "<p>Hi</p>"
            "</body></html>",
            "email-extra: extra variables are silently ignored");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 6. Repeated placeholder ---- */
    {
        TkTmpl *t = tmpl_compile(
            "Dear {{name}}, welcome {{name}}. "
            "Your email is {{email}}, {{name}}.");
        ASSERT(t != NULL, "email-repeat: compile succeeds");

        TkTmplVar vars[] = {
            {"name",  "Dave"},
            {"email", "dave@example.com"}
        };
        const char *got = tmpl_render(t, vars, 2);
        ASSERT_STREQ(got,
            "Dear Dave, welcome Dave. "
            "Your email is dave@example.com, Dave.",
            "email-repeat: repeated placeholder substituted each time");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 7. Static content (no placeholders) ---- */
    {
        const char *static_tmpl =
            "<html><body><p>No dynamic content here.</p></body></html>";
        TkTmpl *t = tmpl_compile(static_tmpl);
        ASSERT(t != NULL, "email-static: compile succeeds");

        /* Render with no vars. */
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, static_tmpl,
            "email-static: static template passes through unchanged");
        free((void *)got);

        /* Also test renderhtml — literal HTML must NOT be escaped. */
        const char *got2 = tmpl_renderhtml(t, NULL, 0);
        ASSERT_STREQ(got2, static_tmpl,
            "email-static: renderhtml preserves literal HTML");
        free((void *)got2);

        tmpl_free(t);
    }

    /* ---- 8. tmpl_escape directly on XSS payloads ---- */
    {
        const char *e1 = tmpl_escape("<script>alert('xss')</script>");
        ASSERT_STREQ(e1,
            "&lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;",
            "escape-xss: script tag fully escaped");
        free((void *)e1);

        const char *e2 = tmpl_escape("<img src=x onerror=\"alert(1)\">");
        ASSERT_STREQ(e2,
            "&lt;img src=x onerror=&quot;alert(1)&quot;&gt;",
            "escape-xss: img onerror payload escaped");
        free((void *)e2);

        const char *e3 = tmpl_escape("&amp;");
        ASSERT_STREQ(e3, "&amp;amp;",
            "escape-xss: already-encoded ampersand double-escaped");
        free((void *)e3);

        const char *e4 = tmpl_escape("normal text");
        ASSERT_STREQ(e4, "normal text",
            "escape-xss: plain text passes through unchanged");
        free((void *)e4);

        const char *e5 = tmpl_escape("");
        ASSERT_STREQ(e5, "",
            "escape-xss: empty string returns empty");
        free((void *)e5);
    }

    /* ---- Summary ---- */
    if (failures == 0) {
        printf("\nAll email integration tests passed.\n");
    } else {
        fprintf(stderr, "\n%d email integration test(s) FAILED.\n", failures);
    }
    return failures ? 1 : 0;
}
