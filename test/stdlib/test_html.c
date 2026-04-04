/*
 * test_html.c — Unit tests for the std.html C library (Story 18.1.2).
 *
 * Build and run: make test-stdlib-html
 *
 * Tests verify:
 *   - Empty document structure (DOCTYPE, <html>, </html>)
 *   - html_title inserts <title>
 *   - html_style inserts <style>
 *   - html_script inserts <script>
 *   - html_div renders with class attribute and content
 *   - html_p renders correctly
 *   - html_h1 renders correctly
 *   - html_h2 renders correctly
 *   - html_span renders with class attribute
 *   - html_a renders with href and link text
 *   - html_img renders as void element with src and alt
 *   - html_table renders thead, th, td
 *   - html_escape handles &, <, >, ", '
 *   - html_append adds node to rendered body
 *   - html_node_render renders a standalone node
 */

#include <stdio.h>
#include <string.h>
#include "../../src/stdlib/html.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) != NULL, msg)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* -------------------------------------------------------------------
     * 1. Empty doc: DOCTYPE, <html>, </html>
     * ------------------------------------------------------------------- */
    {
        TkHtmlDoc *doc = html_doc();
        const char *out = html_render(doc);
        ASSERT_CONTAINS(out, "<!DOCTYPE html>", "empty doc contains <!DOCTYPE html>");
        ASSERT_CONTAINS(out, "<html>",          "empty doc contains <html>");
        ASSERT_CONTAINS(out, "</html>",         "empty doc contains </html>");
        html_free(doc);
    }

    /* -------------------------------------------------------------------
     * 2. html_title
     * ------------------------------------------------------------------- */
    {
        TkHtmlDoc *doc = html_doc();
        html_title(doc, "Test");
        const char *out = html_render(doc);
        ASSERT_CONTAINS(out, "<title>Test</title>", "title renders as <title>Test</title>");
        html_free(doc);
    }

    /* -------------------------------------------------------------------
     * 3. html_style
     * ------------------------------------------------------------------- */
    {
        TkHtmlDoc *doc = html_doc();
        html_style(doc, "body{}");
        const char *out = html_render(doc);
        ASSERT_CONTAINS(out, "<style>", "style renders containing <style>");
        ASSERT_CONTAINS(out, "body{}",  "style content is present");
        html_free(doc);
    }

    /* -------------------------------------------------------------------
     * 4. html_script
     * ------------------------------------------------------------------- */
    {
        TkHtmlDoc *doc = html_doc();
        html_script(doc, "alert(1)");
        const char *out = html_render(doc);
        ASSERT_CONTAINS(out, "<script>",  "script renders containing <script>");
        ASSERT_CONTAINS(out, "alert(1)",  "script content is present");
        html_free(doc);
    }

    /* -------------------------------------------------------------------
     * 5. html_div with class and content
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_div("foo", "bar");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<div class=\"foo\">bar</div>",
                     "html_div renders as <div class=\"foo\">bar</div>");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 6. html_p
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_p("hello");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<p>hello</p>", "html_p renders as <p>hello</p>");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 7. html_h1
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_h1("Title");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<h1>Title</h1>", "html_h1 renders as <h1>Title</h1>");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 8. html_h2
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_h2("Section");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<h2>Section</h2>", "html_h2 renders as <h2>Section</h2>");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 9. html_span with class
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_span("highlight", "text");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<span class=\"highlight\">text</span>",
                     "html_span renders with class and content");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 10. html_a: href and link text
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_a("http://x.com", "click");
        const char *out  = html_node_render(node);
        ASSERT_CONTAINS(out, "href=\"http://x.com\"", "html_a contains href attribute");
        ASSERT_CONTAINS(out, ">click</a>",            "html_a contains link text and closing tag");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 11. html_img: void element with src and alt
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_img("x.png", "alt text");
        const char *out  = html_node_render(node);
        ASSERT_CONTAINS(out, "src=\"x.png\"",    "html_img contains src attribute");
        ASSERT_CONTAINS(out, "alt=\"alt text\"", "html_img contains alt attribute");
        /* Void element must NOT have a closing tag. */
        ASSERT(strstr(out, "</img>") == NULL, "html_img has no closing </img> tag");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 12. html_table: 2 headers, 4 cells (2 rows x 2 cols)
     * ------------------------------------------------------------------- */
    {
        const char *headers[] = {"Name", "Age"};
        const char *cells[]   = {"Alice", "30", "Bob", "25"};
        TkHtmlNode *node = html_table(headers, 2, cells, 2);
        const char *out  = html_node_render(node);
        ASSERT_CONTAINS(out, "<thead>",      "html_table contains <thead>");
        ASSERT_CONTAINS(out, "<th>",         "html_table contains <th>");
        ASSERT_CONTAINS(out, "<td>",         "html_table contains <td>");
        ASSERT_CONTAINS(out, "<tbody>",      "html_table contains <tbody>");
        ASSERT_CONTAINS(out, "Name",         "html_table contains first header text");
        ASSERT_CONTAINS(out, "Alice",        "html_table contains first cell text");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 13. html_escape: &, <, >, ", '
     * ------------------------------------------------------------------- */
    {
        const char *out = html_escape("<b>&\"'");
        ASSERT_STREQ(out, "&lt;b&gt;&amp;&quot;&#39;",
                     "html_escape encodes &, <, >, \", ' correctly");
        free((void *)out);
    }

    /* -------------------------------------------------------------------
     * 14. html_append adds node to body in rendered output
     * ------------------------------------------------------------------- */
    {
        TkHtmlDoc  *doc  = html_doc();
        TkHtmlNode *node = html_p("body content");
        html_append(doc, node);
        const char *out = html_render(doc);
        ASSERT_CONTAINS(out, "<body>",         "rendered doc contains <body>");
        ASSERT_CONTAINS(out, "<p>body content</p>",
                        "appended node appears in rendered body");
        html_free(doc);
        /* node is owned by doc and freed by html_free */
    }

    /* -------------------------------------------------------------------
     * 15. html_div with NULL class renders without class attribute
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_div(NULL, "content");
        const char *out  = html_node_render(node);
        ASSERT_STREQ(out, "<div>content</div>",
                     "html_div with NULL class renders without class attribute");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 16. html_append_child nests nodes in render
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *parent = html_div("outer", NULL);
        TkHtmlNode *child  = html_p("nested");
        html_append_child(parent, child);
        const char *out = html_node_render(parent);
        ASSERT_CONTAINS(out, "<p>nested</p>",
                        "html_append_child: child appears in parent render");
        free((void *)out);
        html_node_free(parent);
        /* child is owned by parent and freed with it */
    }

    /* -------------------------------------------------------------------
     * Summary
     * ------------------------------------------------------------------- */
    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
}
