/*
 * test_canvas.c — Unit tests for the std.canvas C library (Story 18.1.5).
 *
 * Build and run: make test-stdlib-canvas
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/canvas.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) != NULL, msg)

int main(void)
{
    /* --- Test 1: canvas_new produces op_count == 0 --- */
    TkCanvas *c = canvas_new("myc", 800, 600);
    ASSERT(c != NULL, "canvas_new returns non-NULL");
    ASSERT(canvas_op_count(c) == 0, "canvas_new: op_count == 0");

    /* --- Test 2: single fill_rect increments op_count to 1 --- */
    TkCanvas *r = canvas_fill_rect(c, 0, 0, 10, 10, "red");
    ASSERT(canvas_op_count(c) == 1, "after fill_rect: op_count == 1");

    /* --- Test 3: chaining — fill_rect returns same pointer --- */
    ASSERT(r == c, "canvas_fill_rect returns same pointer (chaining)");

    /* --- Test 4: five drawing calls → op_count == 5 --- */
    /* (already have 1 from above; add 4 more) */
    canvas_stroke_rect(c, 5, 5, 20, 20, "blue", 2.0);
    canvas_clear_rect(c, 0, 0, 5, 5);
    canvas_move_to(c, 10, 10);
    canvas_line_to(c, 50, 50);
    ASSERT(canvas_op_count(c) == 5, "after 5 drawing calls: op_count == 5");

    /* --- Test 5: canvas_to_js contains getElementById('myc') --- */
    const char *js = canvas_to_js(c);
    ASSERT_CONTAINS(js, "getElementById('myc')", "to_js contains getElementById('myc')");

    /* --- Test 6: canvas_to_js contains fillRect --- */
    ASSERT_CONTAINS(js, "fillRect", "to_js contains fillRect");

    /* --- Test 7: canvas_to_js contains fillStyle for fill_rect --- */
    ASSERT_CONTAINS(js, "fillStyle", "to_js contains fillStyle");

    /* --- Test 8: canvas_to_html contains <canvas and id="myc" and width="800" --- */
    const char *html = canvas_to_html(c);
    ASSERT_CONTAINS(html, "<canvas", "to_html contains <canvas");
    ASSERT_CONTAINS(html, "id=\"myc\"", "to_html contains id=\"myc\"");
    ASSERT_CONTAINS(html, "width=\"800\"", "to_html contains width=\"800\"");

    /* --- Test 9: canvas_to_html contains <script> and </script> --- */
    ASSERT_CONTAINS(html, "<script>", "to_html contains <script>");
    ASSERT_CONTAINS(html, "</script>", "to_html contains </script>");

    canvas_free(c);

    /* --- Test 10: fill_text — to_js contains fillText and the text content --- */
    TkCanvas *c2 = canvas_new("txt", 400, 300);
    canvas_fill_text(c2, "Hello world", 10, 20, "16px sans-serif", "black");
    const char *js2 = canvas_to_js(c2);
    ASSERT_CONTAINS(js2, "fillText", "fill_text: to_js contains fillText");
    ASSERT_CONTAINS(js2, "Hello world", "fill_text: to_js contains text content");
    canvas_free(c2);

    /* --- Test 11: arc — to_js contains arc( --- */
    TkCanvas *c3 = canvas_new("arc", 200, 200);
    canvas_arc(c3, 100, 100, 50, 0, 3.14159);
    const char *js3 = canvas_to_js(c3);
    ASSERT_CONTAINS(js3, "arc(", "arc: to_js contains arc(");
    canvas_free(c3);

    /* --- Test 12: begin_path + line_to + stroke — to_js contains all three --- */
    TkCanvas *c4 = canvas_new("path", 300, 300);
    canvas_begin_path(c4);
    canvas_move_to(c4, 0, 0);
    canvas_line_to(c4, 100, 100);
    canvas_stroke(c4, "green", 1.5);
    const char *js4 = canvas_to_js(c4);
    ASSERT_CONTAINS(js4, "beginPath", "path: to_js contains beginPath");
    ASSERT_CONTAINS(js4, "lineTo", "path: to_js contains lineTo");
    ASSERT_CONTAINS(js4, "stroke()", "path: to_js contains stroke()");
    canvas_free(c4);

    /* --- Test 13: set_alpha — to_js contains globalAlpha= --- */
    TkCanvas *c5 = canvas_new("alpha", 100, 100);
    canvas_set_alpha(c5, 0.5);
    const char *js5 = canvas_to_js(c5);
    ASSERT_CONTAINS(js5, "globalAlpha=", "set_alpha: to_js contains globalAlpha=");
    canvas_free(c5);

    /* --- Test 14: chaining returns same pointer for every drawing function --- */
    TkCanvas *c6 = canvas_new("chain", 640, 480);
    ASSERT(canvas_begin_path(c6)                    == c6, "begin_path returns same pointer");
    ASSERT(canvas_move_to(c6, 0, 0)                 == c6, "move_to returns same pointer");
    ASSERT(canvas_line_to(c6, 1, 1)                 == c6, "line_to returns same pointer");
    ASSERT(canvas_arc(c6, 0, 0, 5, 0, 1)            == c6, "arc returns same pointer");
    ASSERT(canvas_close_path(c6)                    == c6, "close_path returns same pointer");
    ASSERT(canvas_fill(c6, "blue")                  == c6, "fill returns same pointer");
    ASSERT(canvas_stroke(c6, "red", 1.0)            == c6, "stroke returns same pointer");
    ASSERT(canvas_clear_rect(c6, 0, 0, 1, 1)        == c6, "clear_rect returns same pointer");
    ASSERT(canvas_stroke_rect(c6,0,0,1,1,"#000",1)  == c6, "stroke_rect returns same pointer");
    ASSERT(canvas_draw_image(c6,"a.png",0,0,10,10)  == c6, "draw_image returns same pointer");
    ASSERT(canvas_set_alpha(c6, 1.0)                == c6, "set_alpha returns same pointer");
    canvas_free(c6);

    /* --- Summary --- */
    if (failures == 0) {
        printf("All canvas tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
}
