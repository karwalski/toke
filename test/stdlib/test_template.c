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
#include <unistd.h>     /* mkdtemp (PAR-09 layout test) */
#include <sys/stat.h>   /* mkdir */
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

/* 121.37: render src with one binding v through the escaping renderer. */
static void ctx_case(const char *src, const char *key, const char *val,
                     const char *want, const char *msg)
{
    TkTmpl *t = tmpl_compile(src);
    TkTmplVar vars[] = {{key, val}, {"x", "1"}};
    const char *got = t ? tmpl_renderhtml(t, vars, 2) : NULL;
    if (!got || strcmp(got, want) != 0)
        fprintf(stderr, "  got:  %s\n  want: %s\n", got ? got : "(null)", want);
    ASSERT_STREQ(got, want, msg);
    free((void *)got);
    tmpl_free(t);
}

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

    /* ---- 6b. 124.3 (ADR-0011): {{key|raw}} opts out of escaping ---- */
    {
        TkTmpl *t = tmpl_compile("{{content|raw}}");
        TkTmplVar vars[] = {{"content", "<b>bold</b>"}};
        const char *got = tmpl_renderhtml(t, vars, 1);
        ASSERT_STREQ(got, "<b>bold</b>",
                     "renderhtml: {{content|raw}} emits trusted HTML unescaped");
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

    /* ---- 34-39. 121.16 PAR-09 (121.41 (5)): bounded recursion, layout names ---- */
    {
        /* 34. a self-including partial fails the render instead of recursing
         *     until the stack is gone */
        tmpl_register_partial("par09self", "x{{>par09self}}");
        TkTmpl *t = tmpl_compile("{{>par09self}}");
        ASSERT(t != NULL, "PAR-09: self-partial template compiles");
        const char *got = tmpl_render(t, NULL, 0);
        ASSERT(got == NULL, "PAR-09: self-including partial fails the render");
        free((void *)got);
        tmpl_free(t);

        /* 35. a mutual cycle a -> b -> a fails too */
        tmpl_register_partial("par09a", "{{>par09b}}");
        tmpl_register_partial("par09b", "{{>par09a}}");
        t = tmpl_compile("{{>par09a}}");
        got = tmpl_render(t, NULL, 0);
        ASSERT(got == NULL, "PAR-09: partial cycle a->b->a fails the render");
        free((void *)got);
        tmpl_free(t);

        /* 36. a partial chain inside the cap still renders */
        tmpl_register_partial("par09c1", "1{{>par09c2}}");
        tmpl_register_partial("par09c2", "2{{>par09c3}}");
        tmpl_register_partial("par09c3", "3");
        t = tmpl_compile("{{>par09c1}}");
        got = tmpl_render(t, NULL, 0);
        ASSERT_STREQ(got, "123", "PAR-09: a 3-deep partial chain renders");
        free((void *)got);
        tmpl_free(t);
    }
    {
        /* 37. 100000 nested blocks: compile fails, does not overflow */
        const int N = 100000;
        size_t cap = (size_t)N * 20 + 1;
        char *src = (char *)malloc(cap);
        ASSERT(src != NULL, "PAR-09: alloc deep source");
        if (src) {
            size_t n = 0;
            for (int i = 0; i < N; i++) { memcpy(src + n, "{{#if a}}", 9); n += 9; }
            for (int i = 0; i < N; i++) { memcpy(src + n, "{{/if}}", 7); n += 7; }
            src[n] = '\0';
            TkTmpl *t = tmpl_compile(src);
            ASSERT(t == NULL, "PAR-09: 100000 nested blocks fail to compile");
            tmpl_free(t);
            free(src);
        }
        /* 38. 64 nested blocks (the cap) still compile and render */
        char buf[64 * 9 + 1 + 64 * 7 + 1]; size_t n = 0;   /* opens, "y", closes, NUL */
        for (int i = 0; i < 64; i++) { memcpy(buf + n, "{{#if a}}", 9); n += 9; }
        buf[n++] = 'y';
        for (int i = 0; i < 64; i++) { memcpy(buf + n, "{{/if}}", 7); n += 7; }
        buf[n] = '\0';
        TkTmpl *t = tmpl_compile(buf);
        ASSERT(t != NULL, "PAR-09: 64 nested blocks compile");
        TkTmplVar v[] = {{"a", "1"}};
        const char *got = tmpl_render(t, v, 1);
        ASSERT_STREQ(got, "y", "PAR-09: 64 nested blocks render");
        free((void *)got);
        tmpl_free(t);
    }
    {
        /* 39. layout("<name>") is a name inside templates_dir, not a path */
        char dir[] = "/tmp/tk_par09_XXXXXX";
        ASSERT(mkdtemp(dir) != NULL, "PAR-09: mkdtemp");
        char tdir[256], pth[320];
        snprintf(tdir, sizeof tdir, "%s/templates", dir);
        char cmd[512];
        ASSERT(mkdir(tdir, 0755) == 0, "PAR-09: mkdir templates");
        FILE *f;
        snprintf(pth, sizeof pth, "%s/secret.tkt", dir);        /* outside */
        f = fopen(pth, "w"); if (f) { fputs("SECRET", f); fclose(f); }
        snprintf(pth, sizeof pth, "%s/base.tkt", tdir);         /* inside */
        f = fopen(pth, "w"); if (f) { fputs("<{! yield(\"b\") !}>", f); fclose(f); }
        const char *pages[][2] = {
            { "{! layout(\"../secret\") !}", NULL },
            { "{! layout(\"/etc/hosts\") !}", NULL },
            { "{! layout(\"..\\\\secret\") !}", NULL },
            { "{! layout(\"\") !}", NULL },
            { "{! layout(\"base\") !}{! block(\"b\") !}ok{! end !}", "<ok>" },
        };
        for (size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) {
            snprintf(pth, sizeof pth, "%s/page%zu.tkt", dir, i);
            f = fopen(pth, "w"); if (f) { fputs(pages[i][0], f); fclose(f); }
            const char *got = tmpl_renderpage(pth, tdir, NULL, 0);
            char msg[160];
            snprintf(msg, sizeof msg, "PAR-09: layout page %zu -> %s", i,
                     pages[i][1] ? pages[i][1] : "refused (NULL)");
            if (pages[i][1]) ASSERT_STREQ(got, pages[i][1], msg);
            else ASSERT(got == NULL, msg);
            free((void *)got);
        }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        (void)system(cmd);
    }

    /* ---- 121.37 (ADR-0011 (b)): the escaper follows the slot's context ---- */
    ctx_case("<p>{{v}}</p>", "v", "<b>", "<p>&lt;b&gt;</p>", "ctx: body");
    ctx_case("<a title=\"{{v}}\">", "v", "\"><x", "<a title=\"&quot;&gt;&lt;x\">",
             "ctx: double-quoted attribute");
    ctx_case("<a title='{{v}}'>", "v", "'x", "<a title='&#39;x'>",
             "ctx: single-quoted attribute");
    ctx_case("<a title={{v}}>", "v", "a onmouseover=alert(1)",
             "<a title=a&#32;onmouseover&#61;alert&#40;1&#41;>",
             "ctx: unquoted attribute cannot add an attribute");
    ctx_case("<a href=\"{{v}}\">", "v", "javascript:alert(1)", "<a href=\"#ZtokeZ\">",
             "ctx: URL start refuses javascript:");
    ctx_case("<a href=\"{{v}}\">", "v", " JavaScript:alert(1)", "<a href=\"#ZtokeZ\">",
             "ctx: URL start refuses JavaScript: after whitespace");
    ctx_case("<a href=\"  {{v}}\">", "v", "javascript:alert(1)", "<a href=\"  #ZtokeZ\">",
             "ctx: literal leading whitespace is still the URL start");
    ctx_case("<img src=\"{{v}}\">", "v", "data:text/html,<script>", "<img src=\"#ZtokeZ\">",
             "ctx: URL start refuses data:");
    ctx_case("<a href={{v}}>", "v", "javascript:alert(1)", "<a href=&#35;ZtokeZ>",
             "ctx: unquoted URL refuses javascript:");
    ctx_case("<a href=\"{{v}}\">", "v", "https://x.test/a b\"c",
             "<a href=\"https://x.test/a%20b%22c\">", "ctx: https URL kept, normalised");
    ctx_case("<a href=\"{{v}}\">", "v", "/p/q?r=1#s", "<a href=\"/p/q?r=1#s\">",
             "ctx: relative URL kept");
    ctx_case("<a href=\"{{v}}\">", "v", "mailto:a@b.test", "<a href=\"mailto:a@b.test\">",
             "ctx: mailto kept");
    ctx_case("<a href=\"/s?q={{v}}\">", "v", "a&b c\"=d", "<a href=\"/s?q=a%26b%20c%22%3Dd\">",
             "ctx: URL query component encoded");
    ctx_case("<a href=\"/u/{{v}}\">", "v", "javascript:x", "<a href=\"/u/javascript:x\">",
             "ctx: URL path after the start is not a scheme");
    ctx_case("<a onclick=\"f('{{v}}')\">", "v", "');alert(1);//",
             "<a onclick=\"f('\\u0027\\u0029\\u003Balert\\u00281\\u0029\\u003B\\u002F\\u002F')\">",
             "ctx: on* attribute, JS string: no quote survives entity decoding");
    ctx_case("<a onclick=\"go({{v}})\">", "v", "1);alert(2",
             "<a onclick=\"go(&quot;1\\u0029\\u003Balert\\u00282&quot;)\">",
             "ctx: on* attribute, JS code: a quoted string literal");
    ctx_case("<script>var a={{v}};</script>", "v", "1;alert(1)",
             "<script>var a=\"1\\u003Balert\\u00281\\u0029\";</script>",
             "ctx: script code: a quoted string literal");
    ctx_case("<SCRIPT>var a='{{v}}';</SCRIPT>", "v", "</script><b>",
             "<SCRIPT>var a='\\u003C\\u002Fscript\\u003E\\u003Cb\\u003E';</SCRIPT>",
             "ctx: script (any case), JS string: no </script>");
    ctx_case("<script>// {{v}}\nx=1</script>", "v", "a\nalert(1)",
             "<script>// a\\u000Aalert\\u00281\\u0029\nx=1</script>",
             "ctx: JS line comment cannot be ended by a newline");
    ctx_case("<script>var a=\"{{v}}\";</script>", "v", "a\xE2\x80\xA8" "b",
             "<script>var a=\"a\\u2028b\";</script>", "ctx: U+2028 escaped in a JS string");
    ctx_case("<script>var a=\"</script>\";</script><p>{{v}}</p>", "v", "<i>",
             "<script>var a=\"</script>\";</script><p>&lt;i&gt;</p>",
             "ctx: </script> ends the element even inside a JS string");
    ctx_case("<script>x()</script><p title=\"{{v}}\">", "v", "\"",
             "<script>x()</script><p title=\"&quot;\">", "ctx: back to HTML after </script>");
    ctx_case("<style>p{color:{{v}}}</style>", "v", "#f00", "<style>p{color:#f00}</style>",
             "ctx: style code keeps a plain value");
    ctx_case("<style>p{color:{{v}}}</style>", "v", "red;background:url(javascript:x)",
             "<style>p{color:ZtokeZ}</style>", "ctx: style code refuses url(");
    ctx_case("<style>p{font-family:\"{{v}}\"}</style>", "v", "a\"}",
             "<style>p{font-family:\"a\\22 \\7D \"}</style>", "ctx: CSS string escaped");
    ctx_case("<p style=\"color:{{v}}\">", "v", "expression(alert(1))",
             "<p style=\"color:ZtokeZ\">", "ctx: style attribute refuses expression(");
    ctx_case("<div {{v}}>", "v", "hidden", "<div hidden>", "ctx: attribute name kept");
    ctx_case("<div {{v}}>", "v", "onclick=alert(1)", "<div ZtokeZ>",
             "ctx: attribute name position refuses markup");
    ctx_case("<div {{v}}=\"x\">", "v", "onload", "<div ZtokeZ=\"x\">",
             "ctx: attribute name position refuses an event handler");
    ctx_case("<!-- {{v}} -->", "v", "--><script>", "<!-- --&gt;&lt;script&gt; -->",
             "ctx: comment");
    ctx_case("<title>{{v}}</title>", "v", "</title><script>",
             "<title>&lt;/title&gt;&lt;script&gt;</title>", "ctx: title (RCDATA)");
    ctx_case("<a href=\"{{#if x}}{{v}}{{/if}}\">", "v", "javascript:alert(1)",
             "<a href=\"#ZtokeZ\">", "ctx: context carries into {{#if}}");
    ctx_case("{{v|raw}}<a href=\"{{x}}\">", "v", "<script>",
             "<script><a href=\"1\">", "ctx: |raw stays raw");
    ctx_case("<p>{{v|raw}}{{x}}</p>", "v", "<a href=\"",
             "<p><a href=\"1</p>", "ctx: the tracker follows a |raw value");
    {
        TkTmpl *t = tmpl_compile("<a href=\"{{u}}\">");
        TkTmplVar vars[] = {{"u", "javascript:x"}};
        const char *got = tmpl_render(t, vars, 1);
        ASSERT_STREQ(got, "<a href=\"javascript:x\">",
                     "ctx: tmpl_render (escaping off) is unchanged");
        free((void *)got);
        tmpl_free(t);
    }
    {
        const char *tmppath = "/tmp/test_tpl_renderfile_ctx.txt";
        FILE *fp = fopen(tmppath, "w");
        if (fp) {
            fprintf(fp, "<a href=\"{{u}}\">{{b}}</a>");
            fclose(fp);
            TkTmplVar vars[] = {{"u", "javascript:x"}, {"b", "<b>"}};
            const char *got = tmpl_renderfile(tmppath, vars, 2);
            ASSERT_STREQ(got, "<a href=\"#ZtokeZ\">&lt;b&gt;</a>",
                         "ctx: tmpl_renderfile escapes for context");
            free((void *)got);
            remove(tmppath);
        } else {
            ASSERT(0, "ctx: create renderfile temp file");
        }
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
