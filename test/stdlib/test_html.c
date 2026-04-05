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
#include <stdlib.h>
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
     * 17. html_escape: special chars in tag content (Story 20.1.8)
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_p("<script>alert('xss')</script>");
        const char *out  = html_node_render(node);
        /* content is NOT auto-escaped by the node constructor */
        ASSERT_CONTAINS(out, "<script>", "html_p preserves unescaped content as-is");
        free((void *)out);
        html_node_free(node);

        /* But html_escape handles it correctly */
        const char *esc = html_escape("<script>alert('xss')</script>");
        ASSERT(strstr(esc, "<") == NULL,  "html_escape: no raw < remains");
        ASSERT(strstr(esc, ">") == NULL,  "html_escape: no raw > remains");
        ASSERT_CONTAINS(esc, "&lt;script&gt;", "html_escape: script tag is escaped");
        free((void *)esc);
    }

    /* -------------------------------------------------------------------
     * 18. html_escape: special chars in attribute positions
     * ------------------------------------------------------------------- */
    {
        const char *esc = html_escape("val=\"x\"&y='z'");
        ASSERT_CONTAINS(esc, "&quot;", "html_escape: double quote escaped in attr context");
        ASSERT_CONTAINS(esc, "&#39;",  "html_escape: single quote escaped in attr context");
        ASSERT_CONTAINS(esc, "&amp;",  "html_escape: ampersand escaped in attr context");
        free((void *)esc);
    }

    /* -------------------------------------------------------------------
     * 19. Deeply nested HTML nodes (12 levels)
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *root = html_div("level-0", NULL);
        TkHtmlNode *cur  = root;
        int i;
        for (i = 1; i <= 11; i++) {
            TkHtmlNode *child = html_div(NULL, NULL);
            html_append_child(cur, child);
            cur = child;
        }
        /* Put content at the deepest level */
        TkHtmlNode *leaf = html_p("deep-leaf");
        html_append_child(cur, leaf);

        const char *out = html_node_render(root);
        ASSERT_CONTAINS(out, "deep-leaf",
                        "deeply nested (12 levels): leaf content appears in render");
        /* Count nesting depth by counting </div> occurrences */
        int div_count = 0;
        const char *search = out;
        while ((search = strstr(search, "</div>")) != NULL) {
            div_count++;
            search += 6;
        }
        ASSERT(div_count == 12, "deeply nested: 12 closing </div> tags");
        free((void *)out);
        html_node_free(root);
    }

    /* -------------------------------------------------------------------
     * 20. Empty element (no children, no attrs, no content)
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_div(NULL, NULL);
        const char *out  = html_node_render(node);
        ASSERT_CONTAINS(out, "<div></div>",
                        "empty div renders as <div></div>");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * 21. Self-closing (void) element: html_img with no attrs
     * ------------------------------------------------------------------- */
    {
        TkHtmlNode *node = html_img(NULL, NULL);
        const char *out  = html_node_render(node);
        ASSERT_CONTAINS(out, "<img>", "void element img with NULL attrs renders <img>");
        ASSERT(strstr(out, "</img>") == NULL,
               "void element img: no closing tag");
        free((void *)out);
        html_node_free(node);
    }

    /* -------------------------------------------------------------------
     * Story 34.1.1 tests
     * ------------------------------------------------------------------- */

    /* 22. html_form */
    {
        const char *children[2];
        children[0] = "<input type=\"text\" name=\"user\" value=\"\">";
        children[1] = "<button type=\"submit\">Go</button>";
        const char *out = html_form("/login", "POST", children, 2);
        ASSERT_CONTAINS(out, "<form",           "html_form: contains <form");
        ASSERT_CONTAINS(out, "action=\"/login\"","html_form: action attribute");
        ASSERT_CONTAINS(out, "method=\"POST\"",  "html_form: method attribute");
        ASSERT_CONTAINS(out, children[0],        "html_form: first child present");
        ASSERT_CONTAINS(out, "</form>",          "html_form: closing tag");
        free((void *)out);
    }

    /* 23. html_input */
    {
        const char *out = html_input("text", "username", "");
        ASSERT_STREQ(out, "<input type=\"text\" name=\"username\" value=\"\">",
                     "html_input: renders correctly");
        free((void *)out);
    }

    /* 24. html_select with selected option */
    {
        const char *opts[2] = {"red", "blue"};
        const char *out = html_select("color", opts, 2, "blue");
        ASSERT_CONTAINS(out, "<select",       "html_select: opening tag");
        ASSERT_CONTAINS(out, "name=\"color\"","html_select: name attribute");
        ASSERT_CONTAINS(out, "<option",       "html_select: option element");
        ASSERT_CONTAINS(out, "selected",      "html_select: selected attribute present");
        /* red should not be selected */
        {
            const char *red_opt = strstr(out, "value=\"red\"");
            ASSERT(red_opt && strstr(red_opt, "selected") > strstr(out, "value=\"blue\""),
                   "html_select: selected on blue not red");
        }
        free((void *)out);
    }

    /* 25. html_textarea */
    {
        const char *out = html_textarea("notes", "hello", 5, 40);
        ASSERT_CONTAINS(out, "<textarea",     "html_textarea: opening tag");
        ASSERT_CONTAINS(out, "name=\"notes\"","html_textarea: name attribute");
        ASSERT_CONTAINS(out, "rows=\"5\"",    "html_textarea: rows attribute");
        ASSERT_CONTAINS(out, "cols=\"40\"",   "html_textarea: cols attribute");
        ASSERT_CONTAINS(out, "hello",         "html_textarea: content");
        ASSERT_CONTAINS(out, "</textarea>",   "html_textarea: closing tag");
        free((void *)out);
    }

    /* 26. html_button */
    {
        const char *out = html_button("Submit", "submit");
        ASSERT_STREQ(out, "<button type=\"submit\">Submit</button>",
                     "html_button: renders correctly");
        free((void *)out);
    }

    /* 27. html_label */
    {
        const char *out = html_label("email", "Email address");
        ASSERT_CONTAINS(out, "for=\"email\"",   "html_label: for attribute");
        ASSERT_CONTAINS(out, "Email address",   "html_label: text content");
        ASSERT_CONTAINS(out, "</label>",         "html_label: closing tag");
        free((void *)out);
    }

    /* 28. html_ul */
    {
        const char *items[3] = {"a", "b", "c"};
        const char *out = html_ul(items, 3);
        ASSERT_CONTAINS(out, "<ul>",      "html_ul: opening tag");
        ASSERT_CONTAINS(out, "<li>a</li>","html_ul: first item");
        ASSERT_CONTAINS(out, "<li>b</li>","html_ul: second item");
        ASSERT_CONTAINS(out, "<li>c</li>","html_ul: third item");
        ASSERT_CONTAINS(out, "</ul>",     "html_ul: closing tag");
        free((void *)out);
    }

    /* 29. html_ol */
    {
        const char *items[2] = {"first", "second"};
        const char *out = html_ol(items, 2);
        ASSERT_CONTAINS(out, "<ol>",           "html_ol: opening tag");
        ASSERT_CONTAINS(out, "<li>first</li>", "html_ol: first item");
        ASSERT_CONTAINS(out, "</ol>",          "html_ol: closing tag");
        free((void *)out);
    }

    /* 30. html_br */
    {
        const char *out = html_br();
        ASSERT_STREQ(out, "<br>", "html_br: renders as <br>");
        free((void *)out);
    }

    /* 31. html_hr */
    {
        const char *out = html_hr();
        ASSERT_STREQ(out, "<hr>", "html_hr: renders as <hr>");
        free((void *)out);
    }

    /* 32. html_pre */
    {
        const char *out = html_pre("code here");
        ASSERT_STREQ(out, "<pre>code here</pre>",
                     "html_pre: renders correctly");
        free((void *)out);
    }

    /* 33. html_code */
    {
        const char *out = html_code("x = 1");
        ASSERT_STREQ(out, "<code>x = 1</code>",
                     "html_code: renders correctly");
        free((void *)out);
    }

    /* -------------------------------------------------------------------
     * Story 34.1.2 tests
     * ------------------------------------------------------------------- */

    /* 34. html_attr: add attribute to existing element string */
    {
        const char *out = html_attr("<div class=\"foo\">content</div>",
                                    "id", "bar");
        ASSERT_CONTAINS(out, "id=\"bar\"",    "html_attr: id attribute added");
        ASSERT_CONTAINS(out, "class=\"foo\"", "html_attr: existing class preserved");
        ASSERT_CONTAINS(out, "content</div>", "html_attr: content preserved");
        free((void *)out);
    }

    /* 35. html_class_add: append to existing class */
    {
        const char *out = html_class_add("<div class=\"foo\">content</div>", "bar");
        ASSERT_CONTAINS(out, "class=\"foo bar\"", "html_class_add: class becomes foo bar");
        ASSERT_CONTAINS(out, "content</div>",     "html_class_add: content preserved");
        free((void *)out);
    }

    /* 36. html_class_add: add class when none exists */
    {
        const char *out = html_class_add("<div>content</div>", "newclass");
        ASSERT_CONTAINS(out, "class=\"newclass\"","html_class_add: class added when absent");
        free((void *)out);
    }

    /* 37. html_id: set id attribute */
    {
        const char *out = html_id("<p>text</p>", "main");
        ASSERT_CONTAINS(out, "id=\"main\"", "html_id: id attribute set");
        ASSERT_CONTAINS(out, "text</p>",    "html_id: content preserved");
        free((void *)out);
    }

    /* 38. html_meta */
    {
        const char *out = html_meta("viewport", "width=device-width");
        ASSERT_STREQ(out,
                     "<meta name=\"viewport\" content=\"width=device-width\">",
                     "html_meta: renders correctly");
        free((void *)out);
    }

    /* 39. html_link_stylesheet */
    {
        const char *out = html_link_stylesheet("/style.css");
        ASSERT_CONTAINS(out, "rel=\"stylesheet\"",  "html_link_stylesheet: rel attribute");
        ASSERT_CONTAINS(out, "href=\"/style.css\"", "html_link_stylesheet: href attribute");
        ASSERT_CONTAINS(out, "<link",               "html_link_stylesheet: link tag");
        free((void *)out);
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
