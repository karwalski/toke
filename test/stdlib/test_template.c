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
#include <ctype.h>
#include "../../src/stdlib/template.h"

static int failures = 0;

/* Helper used in test 31: uppercases its input. */
static const char *helper_shout(const char *value)
{
    if (!value) return "";
    size_t len = strlen(value);
    char  *out = (char *)malloc(len + 1);
    if (!out) return value;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)toupper((unsigned char)value[i]);
    out[len] = '\0';
    return out;
}

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

    /* ---- 13. tmpl_vars: construct variable bindings ---- */
    {
        const char *keys[]   = {"greeting", "name"};
        const char *values[] = {"Hi",       "Carol"};
        TkTmplVar *vars = tmpl_vars(keys, values, 2);
        ASSERT(vars != NULL, "tmpl_vars: non-NULL result");
        ASSERT_STREQ(vars[0].key, "greeting", "tmpl_vars: key[0] = greeting");
        ASSERT_STREQ(vars[0].value, "Hi",     "tmpl_vars: value[0] = Hi");
        ASSERT_STREQ(vars[1].key, "name",     "tmpl_vars: key[1] = name");
        ASSERT_STREQ(vars[1].value, "Carol",  "tmpl_vars: value[1] = Carol");

        /* use the constructed vars in a render */
        TkTmpl *t = tmpl_compile("{{greeting}}, {{name}}!");
        const char *got = tmpl_render(t, vars, 2);
        ASSERT_STREQ(got, "Hi, Carol!", "tmpl_vars: render with constructed vars");
        free((void *)got);
        tmpl_free(t);
        free(vars);
    }

    /* ---- 14. tmpl_vars: zero count returns NULL ---- */
    {
        TkTmplVar *vars = tmpl_vars(NULL, NULL, 0);
        ASSERT(vars == NULL, "tmpl_vars: nvar=0 returns NULL");
    }

    /* ---- 15. tmpl_html: simple element with attributes and children ---- */
    {
        const char *ak[] = {"class", "id"};
        const char *av[] = {"main",  "box"};
        const char *ch[] = {"hello"};
        const char *got = tmpl_html("div", ak, av, 2, ch, 1);
        ASSERT_STREQ(got, "<div class=\"main\" id=\"box\">hello</div>",
                     "tmpl_html: div with attrs and child");
        free((void *)got);
    }

    /* ---- 16. tmpl_html: no attributes, no children ---- */
    {
        const char *got = tmpl_html("br", NULL, NULL, 0, NULL, 0);
        ASSERT_STREQ(got, "<br></br>", "tmpl_html: empty element");
        free((void *)got);
    }

    /* ---- 17. tmpl_html: attribute values are HTML-escaped ---- */
    {
        const char *ak[] = {"title"};
        const char *av[] = {"a&b<c\"d"};
        const char *got = tmpl_html("span", ak, av, 1, NULL, 0);
        ASSERT_STREQ(got, "<span title=\"a&amp;b&lt;c&quot;d\"></span>",
                     "tmpl_html: attribute values escaped");
        free((void *)got);
    }

    /* ---- 18. tmpl_html: multiple children concatenated ---- */
    {
        const char *ch[] = {"<b>one</b>", "<i>two</i>"};
        const char *got = tmpl_html("p", NULL, NULL, 0, ch, 2);
        ASSERT_STREQ(got, "<p><b>one</b><i>two</i></p>",
                     "tmpl_html: multiple children");
        free((void *)got);
    }

    /* ---- 19. tmpl_renderfile: load, compile, render ---- */
    {
        /* write a temporary template file */
        const char *tmppath = "/tmp/test_tpl_renderfile.txt";
        FILE *fp = fopen(tmppath, "w");
        ASSERT(fp != NULL, "tmpl_renderfile: create temp file");
        if (fp) {
            fprintf(fp, "Hello, {{who}}!");
            fclose(fp);

            TkTmplVar vars[] = {{"who", "World"}};
            const char *got = tmpl_renderfile(tmppath, vars, 1);
            ASSERT_STREQ(got, "Hello, World!",
                         "tmpl_renderfile: renders from file");
            free((void *)got);
            remove(tmppath);
        }
    }

    /* ---- 20. tmpl_renderfile: non-existent file returns NULL ---- */
    {
        const char *got = tmpl_renderfile("/tmp/no_such_file_ever.txt",
                                          NULL, 0);
        ASSERT(got == NULL, "tmpl_renderfile: missing file returns NULL");
    }

    /* ---- 21. {{#if}} with non-empty key renders block ---- */
    {
        TkTmpl *t = tmpl_compile("{{#if name}}Hello {{name}}!{{/if}}");
        TkTmplVar vars[] = {{"name", "World"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "Hello World!",
                     "if: non-empty key renders block with slot");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 22. {{#if}} with empty value suppresses block ---- */
    {
        TkTmpl *t = tmpl_compile("{{#if name}}Hello{{/if}}");
        TkTmplVar vars[] = {{"name", ""}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "", "if: empty value suppresses block");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 23. {{#unless}} with missing key renders block ---- */
    {
        TkTmpl *t = tmpl_compile("{{#unless missing}}Default{{/unless}}");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "Default",
                     "unless: missing key renders block");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 24. {{#unless}} with non-empty key suppresses block ---- */
    {
        TkTmpl *t = tmpl_compile("{{#unless name}}No name{{/unless}}");
        TkTmplVar vars[] = {{"name", "Alice"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "", "unless: non-empty value suppresses block");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 25. {{#each}} basic iteration with {{.}} ---- */
    {
        TkTmpl *t = tmpl_compile("{{#each items}}{{.}} {{/each}}");
        TkTmplVar vars[] = {{"items", "[\"a\",\"b\",\"c\"]"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "a b c ", "each: basic iteration with {{.}}");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 26. {{#each}} with {{@index}} ---- */
    {
        TkTmpl *t = tmpl_compile("{{#each nums}}{{@index}}:{{.}} {{/each}}");
        TkTmplVar vars[] = {{"nums", "[\"x\",\"y\"]"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "0:x 1:y ", "each: {{@index}} gives 0-based index");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 27. {{#each}} with empty array renders nothing ---- */
    {
        TkTmpl *t = tmpl_compile("{{#each empty}}item{{/each}}");
        TkTmplVar vars[] = {{"empty", "[]"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "", "each: empty array renders nothing");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 28. {{#each}} HTML-escapes {{.}} in renderhtml ---- */
    {
        TkTmpl *t = tmpl_compile("{{#each items}}{{.}}{{/each}}");
        TkTmplVar vars[] = {{"items", "[\"<b>\"]"}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got, "&lt;b&gt;",
                     "each: renderhtml escapes {{.}}");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- Story 33.1.2: partials and helpers ---- */

    /* ---- 29. Partial: {{>greeting}} renders registered partial ---- */
    {
        tmpl_register_partial("greeting", "Hello, {{name}}!");
        TkTmpl *t = tmpl_compile("{{>greeting}}");
        TkTmplVar vars[] = {{"name", "World"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "Hello, World!",
                     "partial: {{>greeting}} with name=World");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 30. Partial: used inside outer template with HTML tag ---- */
    {
        tmpl_register_partial("header", "<h1>{{title}}</h1>");
        TkTmpl *t = tmpl_compile("{{>header}}<p>body</p>");
        TkTmplVar vars[] = {{"title", "Welcome"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "<h1>Welcome</h1><p>body</p>",
                     "partial: outer template with header partial");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 31. Helper: {{message|shout}} uppercases value ---- */
    {
        tmpl_register_helper("shout", helper_shout);
        TkTmpl *t = tmpl_compile("{{message|shout}}");
        TkTmplVar vars[] = {{"message", "hello"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "HELLO", "helper: {{message|shout}} -> HELLO");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 32. Unregistered partial renders as empty string ---- */
    {
        TkTmpl *t = tmpl_compile("{{>no_such_partial}}");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "",
                     "partial: unregistered partial renders as empty string");
        free((void *)got);
        tmpl_free(t);
    }

    /* ---- 33. Unregistered helper passes through original value ---- */
    {
        TkTmpl *t = tmpl_compile("{{value|no_such_helper}}");
        TkTmplVar vars[] = {{"value", "original"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "original",
                     "helper: unregistered helper passes through value");
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
