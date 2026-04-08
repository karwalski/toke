/*
 * test_md.c — CommonMark conformance tests for the std.md module (Story 55.5.3).
 *
 * Build and run: make test-stdlib-md
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/md.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) != NULL, msg)
#define ASSERT_NOT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) == NULL, msg)

int main(void)
{
    const char *html;

    /* ── ATX headings ──────────────────────────────────────────────────── */

    /* 1: h1 */
    html = md_render("# Hello\n");
    ASSERT_CONTAINS(html, "<h1>", "h1: contains <h1>");
    ASSERT_CONTAINS(html, "Hello", "h1: contains text");
    free((void *)html);

    /* 2: h2 */
    html = md_render("## World\n");
    ASSERT_CONTAINS(html, "<h2>", "h2: contains <h2>");
    free((void *)html);

    /* 3: h3 */
    html = md_render("### Three\n");
    ASSERT_CONTAINS(html, "<h3>", "h3: contains <h3>");
    free((void *)html);

    /* 4: h4 */
    html = md_render("#### Four\n");
    ASSERT_CONTAINS(html, "<h4>", "h4: contains <h4>");
    free((void *)html);

    /* 5: h5 */
    html = md_render("##### Five\n");
    ASSERT_CONTAINS(html, "<h5>", "h5: contains <h5>");
    free((void *)html);

    /* 6: h6 */
    html = md_render("###### Six\n");
    ASSERT_CONTAINS(html, "<h6>", "h6: contains <h6>");
    free((void *)html);

    /* ── Paragraphs ────────────────────────────────────────────────────── */

    /* 7: plain paragraph */
    html = md_render("Just some plain text.\n");
    ASSERT_CONTAINS(html, "<p>", "paragraph: contains <p>");
    free((void *)html);

    /* 8: two paragraphs separated by blank line */
    html = md_render("First paragraph.\n\nSecond paragraph.\n");
    {
        const char *first  = strstr(html, "<p>");
        const char *second = first ? strstr(first + 1, "<p>") : NULL;
        ASSERT(second != NULL, "two paragraphs: two <p> tags present");
    }
    free((void *)html);

    /* ── Bold and italic ───────────────────────────────────────────────── */

    /* 9: **bold** */
    html = md_render("**bold**\n");
    ASSERT_CONTAINS(html, "<strong>", "bold(**): contains <strong>");
    free((void *)html);

    /* 10: *italic* */
    html = md_render("*italic*\n");
    ASSERT_CONTAINS(html, "<em>", "italic(*): contains <em>");
    free((void *)html);

    /* 11: _also italic_ */
    html = md_render("_also italic_\n");
    ASSERT_CONTAINS(html, "<em>", "italic(_): contains <em>");
    free((void *)html);

    /* 12: __also bold__ */
    html = md_render("__also bold__\n");
    ASSERT_CONTAINS(html, "<strong>", "bold(__): contains <strong>");
    free((void *)html);

    /* ── Inline code ───────────────────────────────────────────────────── */

    /* 13: `code` */
    html = md_render("`code`\n");
    ASSERT_CONTAINS(html, "<code>", "inline code: contains <code>");
    free((void *)html);

    /* ── Fenced code blocks ────────────────────────────────────────────── */

    /* 14: plain fenced block → <pre><code> */
    html = md_render("```\nsome code\n```\n");
    ASSERT_CONTAINS(html, "<pre>", "fenced block: contains <pre>");
    ASSERT_CONTAINS(html, "<code>", "fenced block: contains <code>");
    free((void *)html);

    /* 15: fenced block with language identifier */
    html = md_render("```c\nint x = 0;\n```\n");
    ASSERT_CONTAINS(html, "<code", "fenced block with lang: contains <code");
    free((void *)html);

    /* ── Links ─────────────────────────────────────────────────────────── */

    /* 16: [text](url) */
    html = md_render("[click here](https://example.com)\n");
    ASSERT_CONTAINS(html, "<a href=", "link: contains <a href=");
    ASSERT_CONTAINS(html, "click here", "link: contains link text");
    free((void *)html);

    /* 17: specific href value */
    html = md_render("[toke](https://tokelang.dev)\n");
    ASSERT_CONTAINS(html, "href=\"https://tokelang.dev\"", "link: correct href attribute");
    free((void *)html);

    /* ── Images ────────────────────────────────────────────────────────── */

    /* 18: ![alt](url.png) */
    html = md_render("![alt text](image.png)\n");
    ASSERT_CONTAINS(html, "<img", "image: contains <img");
    free((void *)html);

    /* ── Unordered lists ───────────────────────────────────────────────── */

    /* 19: dash items */
    html = md_render("- item one\n- item two\n");
    ASSERT_CONTAINS(html, "<ul>", "unordered list(-): contains <ul>");
    ASSERT_CONTAINS(html, "<li>", "unordered list(-): contains <li>");
    free((void *)html);

    /* 20: star items */
    html = md_render("* star item\n");
    ASSERT_CONTAINS(html, "<li>", "unordered list(*): contains <li>");
    free((void *)html);

    /* ── Ordered lists ─────────────────────────────────────────────────── */

    /* 21: numbered items */
    html = md_render("1. First\n2. Second\n");
    ASSERT_CONTAINS(html, "<ol>", "ordered list: contains <ol>");
    ASSERT_CONTAINS(html, "<li>", "ordered list: contains <li>");
    free((void *)html);

    /* ── Blockquotes ───────────────────────────────────────────────────── */

    /* 22: > quoted text */
    html = md_render("> quoted text\n");
    ASSERT_CONTAINS(html, "<blockquote>", "blockquote: contains <blockquote>");
    free((void *)html);

    /* ── Hard line break ───────────────────────────────────────────────── */

    /* 23: two trailing spaces before newline */
    html = md_render("line one  \nline two\n");
    ASSERT_CONTAINS(html, "<br", "hard line break: contains <br");
    free((void *)html);

    /* ── Horizontal rule ───────────────────────────────────────────────── */

    /* 24: --- alone on a line */
    html = md_render("---\n");
    ASSERT_CONTAINS(html, "<hr", "horizontal rule: contains <hr");
    free((void *)html);

    /* ── HTML passthrough (CMARK_OPT_UNSAFE) ──────────────────────────── */

    /* 25: raw HTML div is preserved */
    html = md_render("<div class=\"raw\">content</div>\n");
    ASSERT_CONTAINS(html, "<div", "raw HTML: <div passes through");
    free((void *)html);

    /* ── Nested structures ─────────────────────────────────────────────── */

    /* 26: list inside blockquote */
    html = md_render("> - nested item\n");
    ASSERT_CONTAINS(html, "<blockquote>", "nested list in blockquote: <blockquote> present");
    ASSERT_CONTAINS(html, "<li>", "nested list in blockquote: <li> present");
    free((void *)html);

    /* 27: bold inside heading */
    html = md_render("## **bold heading**\n");
    ASSERT_CONTAINS(html, "<h2>", "bold in heading: <h2> present");
    ASSERT_CONTAINS(html, "<strong>", "bold in heading: <strong> present");
    free((void *)html);

    /* ── Edge cases ────────────────────────────────────────────────────── */

    /* 28: empty string — must not crash and must return non-NULL */
    html = md_render("");
    ASSERT(html != NULL, "empty input: returns non-NULL");
    free((void *)html);

    /* 29: empty string returns empty-like HTML (no block elements) */
    html = md_render("");
    ASSERT_NOT_CONTAINS(html, "<p>", "empty input: no <p> tag in output");
    free((void *)html);

    /* 30: long input — 100 paragraphs, no crash, non-NULL */
    {
        /* Build 100 paragraphs into a heap buffer */
        const char *para = "This is paragraph text with enough words to be realistic.\n\n";
        size_t para_len  = strlen(para);
        size_t buf_len   = para_len * 100 + 1;
        char  *big       = malloc(buf_len);
        if (big) {
            big[0] = '\0';
            for (int i = 0; i < 100; i++) {
                strcat(big, para);
            }
            html = md_render(big);
            ASSERT(html != NULL, "long input (100 paragraphs): returns non-NULL");
            free((void *)html);
            free(big);
        } else {
            fprintf(stderr, "SKIP: long input test — malloc failed\n");
        }
    }

    /* 31: escaped asterisks — \*not italic\* → literal asterisks, no <em> */
    html = md_render("\\*not italic\\*\n");
    ASSERT_NOT_CONTAINS(html, "<em>", "escaped asterisks: no <em> tag");
    ASSERT_CONTAINS(html, "*", "escaped asterisks: literal * in output");
    free((void *)html);

    /* 32: inline HTML em tag passes through */
    html = md_render("<em>manual</em>\n");
    ASSERT_CONTAINS(html, "<em>", "inline HTML <em>: passes through");
    free((void *)html);

    /* ── md_render_file ────────────────────────────────────────────────── */

    /* 33: non-existent file → is_err == 1 */
    {
        MdFileResult r = md_render_file("/tmp/toke_md_no_such_file_xyz_99999.md");
        ASSERT(r.is_err == 1, "render_file missing: is_err == 1");
    }

    /* 34: valid temp file — render contains <h1> and <p> */
    {
        const char *tmp_path = "/tmp/test_md_toke_story55.md";
        FILE *f = fopen(tmp_path, "w");
        if (f) {
            fputs("# Title\n\nParagraph.\n", f);
            fclose(f);
            MdFileResult r = md_render_file(tmp_path);
            ASSERT(r.is_err == 0, "render_file valid: is_err == 0");
            ASSERT_CONTAINS(r.ok, "<h1>", "render_file valid: contains <h1>");
            ASSERT_CONTAINS(r.ok, "<p>",  "render_file valid: contains <p>");
            free((void *)r.ok);
            remove(tmp_path);
        } else {
            fprintf(stderr, "SKIP: render_file test — could not create temp file\n");
        }
    }

    printf("RESULT: %d failures\n", failures);
    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
