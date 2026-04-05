/*
 * test_svg.c — Unit tests for the std.svg C library (Story 18.1.4).
 *
 * Build and run: make test-stdlib-svg
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/svg.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) != NULL, msg)

/* -----------------------------------------------------------------------
 * Test 1: empty doc — opening tag attributes
 * ----------------------------------------------------------------------- */
static void test_empty_doc(void)
{
    TkSvgDoc   *doc = svg_doc(100.0, 200.0);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<svg",        "empty-doc: contains <svg");
    ASSERT_CONTAINS(out, "width=\"100", "empty-doc: contains width=\"100");
    ASSERT_CONTAINS(out, "height=\"200","empty-doc: contains height=\"200");
    ASSERT_CONTAINS(out, "</svg>",      "empty-doc: contains </svg>");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 2: rect element
 * ----------------------------------------------------------------------- */
static void test_rect(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("red", "black", 1.0);
    TkSvgElem *elem = svg_rect(10.0, 20.0, 50.0, 60.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<rect",     "rect: contains <rect");
    ASSERT_CONTAINS(out, "x=\"10",   "rect: contains x=\"10");
    ASSERT_CONTAINS(out, "width=\"50","rect: contains width=\"50");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 3: circle element
 * ----------------------------------------------------------------------- */
static void test_circle(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("blue", "none", 0.0);
    TkSvgElem *elem = svg_circle(80.0, 90.0, 30.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<circle", "circle: contains <circle");
    ASSERT_CONTAINS(out, "cx=",     "circle: contains cx=");
    ASSERT_CONTAINS(out, "r=",      "circle: contains r=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 4: line element
 * ----------------------------------------------------------------------- */
static void test_line(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("none", "green", 2.0);
    TkSvgElem *elem = svg_line(0.0, 0.0, 100.0, 150.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<line", "line: contains <line");
    ASSERT_CONTAINS(out, "x1=",  "line: contains x1=");
    ASSERT_CONTAINS(out, "y2=",  "line: contains y2=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 5: path element
 * ----------------------------------------------------------------------- */
static void test_path(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("none", "purple", 1.5);
    TkSvgElem *elem = svg_path("M 10 20 L 80 90 Z", s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<path", "path: contains <path");
    ASSERT_CONTAINS(out, "d=",    "path: contains d=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 6: text element
 * ----------------------------------------------------------------------- */
static void test_text(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("black", "none", 0.0);
    TkSvgElem *elem = svg_text(15.0, 30.0, "Hello SVG", s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<text",     "text: contains <text");
    ASSERT_CONTAINS(out, "Hello SVG", "text: contains content string");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 7: group with transform
 * ----------------------------------------------------------------------- */
static void test_group_with_transform(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("orange", "none", 0.0);
    TkSvgElem *r    = svg_rect(0.0, 0.0, 10.0, 10.0, s);
    TkSvgElem *children[1] = { r };
    TkSvgElem *g    = svg_group(children, 1, "translate(5,5)");
    svg_append(doc, g);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<g transform=", "group-transform: contains <g transform=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 8: group without transform
 * ----------------------------------------------------------------------- */
static void test_group_no_transform(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("orange", "none", 0.0);
    TkSvgElem *r    = svg_rect(0.0, 0.0, 10.0, 10.0, s);
    TkSvgElem *children[1] = { r };
    TkSvgElem *g    = svg_group(children, 1, NULL);
    svg_append(doc, g);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<g>", "group-no-transform: contains <g>");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 9: polyline
 * ----------------------------------------------------------------------- */
static void test_polyline(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("none", "teal", 1.0);
    double pts[]    = { 0.0, 0.0, 50.0, 50.0, 100.0, 0.0 };
    TkSvgElem *elem = svg_polyline(pts, 6, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<polyline", "polyline: contains <polyline");
    ASSERT_CONTAINS(out, "points=",   "polyline: contains points=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 10: polygon
 * ----------------------------------------------------------------------- */
static void test_polygon(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("yellow", "black", 1.0);
    double pts[]    = { 50.0, 0.0, 100.0, 100.0, 0.0, 100.0 };
    TkSvgElem *elem = svg_polygon(pts, 6, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<polygon", "polygon: contains <polygon");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 11: arrow — shaft (line) and head (polygon) both present
 * ----------------------------------------------------------------------- */
static void test_arrow(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("none", "black", 1.5);
    TkSvgElem *elem = svg_arrow(10.0, 10.0, 100.0, 100.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<line",    "arrow: contains <line (shaft)");
    ASSERT_CONTAINS(out, "<polygon", "arrow: contains <polygon (head)");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 12: style — fill and stroke appear in style attribute
 * ----------------------------------------------------------------------- */
static void test_style_attrs(void)
{
    TkSvgDoc  *doc  = svg_doc(200.0, 200.0);
    TkSvgStyle s    = svg_style("crimson", "navy", 3.0);
    TkSvgElem *elem = svg_rect(0.0, 0.0, 100.0, 100.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "fill:crimson",   "style: fill:crimson present");
    ASSERT_CONTAINS(out, "stroke:navy",    "style: stroke:navy present");
    ASSERT_CONTAINS(out, "stroke-width:",  "style: stroke-width present");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 13: append multiple elements — all appear in render output
 * ----------------------------------------------------------------------- */
static void test_multiple_elements(void)
{
    TkSvgDoc  *doc = svg_doc(400.0, 400.0);
    TkSvgStyle s   = svg_style("red", "black", 1.0);

    svg_append(doc, svg_rect(0.0,   0.0,  50.0, 50.0, s));
    svg_append(doc, svg_circle(100.0, 100.0, 25.0, s));
    svg_append(doc, svg_line(0.0, 0.0, 200.0, 200.0, s));

    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<rect",   "multi: rect present");
    ASSERT_CONTAINS(out, "<circle", "multi: circle present");
    ASSERT_CONTAINS(out, "<line",   "multi: line present");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 14: SVG coordinate precision — very small values (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_small_coordinates(void)
{
    TkSvgDoc  *doc  = svg_doc(10.0, 10.0);
    TkSvgStyle s    = svg_style("red", "none", 0.0);
    TkSvgElem *elem = svg_circle(0.001, 0.002, 0.0005, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "cx=\"0.00\"",  "small-coords: cx renders for tiny value");
    ASSERT_CONTAINS(out, "<circle",      "small-coords: circle element present");
    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 15: SVG coordinate precision — very large values (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_large_coordinates(void)
{
    TkSvgDoc  *doc  = svg_doc(100000.0, 100000.0);
    TkSvgStyle s    = svg_style("blue", "none", 0.0);
    TkSvgElem *elem = svg_rect(99999.0, 99999.0, 50000.0, 50000.0, s);
    svg_append(doc, elem);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "99999",  "large-coords: large coordinate value present");
    ASSERT_CONTAINS(out, "50000",  "large-coords: large dimension value present");
    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 16: SVG with many elements (120+) (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_many_elements(void)
{
    TkSvgDoc  *doc = svg_doc(1000.0, 1000.0);
    TkSvgStyle s   = svg_style("green", "none", 0.0);
    int i;

    for (i = 0; i < 120; i++) {
        svg_append(doc, svg_rect((double)i, (double)i, 5.0, 5.0, s));
    }

    const char *out = svg_render(doc);
    ASSERT(out != NULL, "many-elements: render returns non-NULL for 120 elements");
    ASSERT_CONTAINS(out, "<svg", "many-elements: opening <svg present");
    ASSERT_CONTAINS(out, "</svg>", "many-elements: closing </svg> present");

    /* Count <rect occurrences */
    int rect_count = 0;
    const char *p = out;
    while ((p = strstr(p, "<rect")) != NULL) {
        rect_count++;
        p += 5;
    }
    ASSERT(rect_count == 120, "many-elements: exactly 120 rect elements rendered");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Test 17: Empty SVG document (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_empty_svg_document(void)
{
    TkSvgDoc   *doc = svg_doc(0.0, 0.0);
    const char *out = svg_render(doc);

    ASSERT(out != NULL,          "empty-svg: render returns non-NULL");
    ASSERT_CONTAINS(out, "<svg", "empty-svg: contains <svg");
    ASSERT_CONTAINS(out, "</svg>", "empty-svg: contains </svg>");
    /* No element tags between <svg> and </svg> */
    ASSERT(strstr(out, "<rect") == NULL,   "empty-svg: no rect elements");
    ASSERT(strstr(out, "<circle") == NULL, "empty-svg: no circle elements");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 18: svg_ellipse
 * ----------------------------------------------------------------------- */
static void test_ellipse(void)
{
    TkSvgDoc   *doc = svg_doc(200.0, 200.0);
    const char *el  = svg_ellipse(doc, 100.0, 80.0, 50.0, 30.0,
                                  "fill:blue;stroke:none");
    const char *out = svg_render(doc);

    ASSERT(el != NULL,                       "ellipse: constructor returns non-NULL");
    ASSERT_CONTAINS(out, "ellipse",          "ellipse: output contains 'ellipse'");
    ASSERT_CONTAINS(out, "cx=",             "ellipse: output contains 'cx='");
    ASSERT_CONTAINS(out, "rx=",             "ellipse: output contains 'rx='");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 19: svg_gradient_linear
 * ----------------------------------------------------------------------- */
static void test_gradient_linear(void)
{
    TkSvgDoc *doc = svg_doc(200.0, 200.0);
    SvgStop stops[2];
    stops[0].offset  = 0.0; stops[0].color = "#ff0000"; stops[0].opacity = 1.0;
    stops[1].offset  = 1.0; stops[1].color = "#0000ff"; stops[1].opacity = 1.0;

    const char *ref = svg_gradient_linear(doc, "grad1", stops, 2,
                                          0.0, 0.0, 1.0, 0.0);
    const char *out = svg_render(doc);

    ASSERT(ref != NULL,                          "linear-grad: ref non-NULL");
    ASSERT_CONTAINS(ref, "url(#grad1)",          "linear-grad: ref is url(#grad1)");
    ASSERT_CONTAINS(out, "linearGradient",       "linear-grad: output contains linearGradient");
    ASSERT_CONTAINS(out, "grad1",                "linear-grad: output contains id 'grad1'");
    ASSERT_CONTAINS(out, "<defs>",               "linear-grad: output contains <defs>");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 20: svg_gradient_radial
 * ----------------------------------------------------------------------- */
static void test_gradient_radial(void)
{
    TkSvgDoc *doc = svg_doc(200.0, 200.0);
    SvgStop stops[2];
    stops[0].offset  = 0.0; stops[0].color = "white"; stops[0].opacity = 1.0;
    stops[1].offset  = 1.0; stops[1].color = "black"; stops[1].opacity = 0.0;

    const char *ref = svg_gradient_radial(doc, "rgrad1", stops, 2,
                                          0.5, 0.5, 0.5);
    const char *out = svg_render(doc);

    ASSERT(ref != NULL,                    "radial-grad: ref non-NULL");
    ASSERT_CONTAINS(out, "radialGradient", "radial-grad: output contains radialGradient");
    ASSERT_CONTAINS(out, "rgrad1",         "radial-grad: output contains id 'rgrad1'");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 21: svg_defs
 * ----------------------------------------------------------------------- */
static void test_defs(void)
{
    TkSvgDoc *doc = svg_doc(200.0, 200.0);
    svg_defs(doc, "<marker id=\"arrow\" markerWidth=\"10\" markerHeight=\"10\"/>");
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "<defs>",  "defs: output contains <defs>");
    ASSERT_CONTAINS(out, "marker",  "defs: output contains defs content");
    ASSERT_CONTAINS(out, "</defs>", "defs: output contains </defs>");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 22: svg_save_file
 * ----------------------------------------------------------------------- */
static void test_save_file(void)
{
    TkSvgDoc *doc  = svg_doc(100.0, 100.0);
    TkSvgStyle s   = svg_style("green", "none", 0.0);
    svg_append(doc, svg_rect(10.0, 10.0, 80.0, 80.0, s));

    const char *path = "/tmp/test_svg_save_34_5_1.svg";
    int ok = svg_save_file(doc, path);

    ASSERT(ok == 1, "save-file: svg_save_file returns 1 on success");

    /* Verify file exists and contains <svg */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL, "save-file: file exists after save");
    if (f) {
        char buf[64];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        ASSERT(strstr(buf, "<svg") != NULL, "save-file: file starts with <svg");
    }

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * Story 34.5.1 — Test 23: svg_animate
 * ----------------------------------------------------------------------- */
static void test_animate(void)
{
    TkSvgDoc *doc = svg_doc(200.0, 200.0);

    /* Register an animation (no matching element — animate appended before </svg>) */
    svg_animate(doc, "box1", "opacity", "1", "0", 2.0);
    const char *out = svg_render(doc);

    ASSERT_CONTAINS(out, "animate",     "animate: output contains 'animate'");
    ASSERT_CONTAINS(out, "opacity",     "animate: output contains attribute name");
    ASSERT_CONTAINS(out, "dur=",        "animate: output contains dur=");

    svg_free(doc);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(void)
{
    test_empty_doc();
    test_rect();
    test_circle();
    test_line();
    test_path();
    test_text();
    test_group_with_transform();
    test_group_no_transform();
    test_polyline();
    test_polygon();
    test_arrow();
    test_style_attrs();
    test_multiple_elements();
    test_small_coordinates();
    test_large_coordinates();
    test_many_elements();
    test_empty_svg_document();
    /* Story 34.5.1 */
    test_ellipse();
    test_gradient_linear();
    test_gradient_radial();
    test_defs();
    test_save_file();
    test_animate();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}
