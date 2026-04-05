/*
 * test_svg_diagram_integration.c — Integration test: SVG diagram generation
 * from structured CSV data.
 *
 * Parses inline CSV datasets of nodes and edges, builds an SVG network
 * diagram using the std.svg and std.csv APIs, and verifies the rendered
 * output contains the expected elements.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror -lm \
 *      -I../../src/stdlib \
 *      ../../src/stdlib/svg.c ../../src/stdlib/csv.c ../../src/stdlib/str.c \
 *      test_svg_diagram_integration.c -o test_svg_diagram_integration
 *
 * Story: 21.1.8
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/svg.h"
#include "../../src/stdlib/csv.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Count non-overlapping occurrences of needle in haystack. */
static int count_occurrences(const char *haystack, const char *needle)
{
    int count = 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

/* Find a column index by name in a header row.  Returns -1 if not found. */
static int col_index(StrArray header, const char *name)
{
    for (uint64_t i = 0; i < header.len; i++) {
        if (strcmp(header.data[i], name) == 0) return (int)i;
    }
    return -1;
}

/* Free a StrArray (each field + the data pointer). */
static void free_strarray(StrArray *sa)
{
    for (uint64_t i = 0; i < sa->len; i++)
        free((void *)sa->data[i]);
    free((void *)sa->data);
    sa->data = NULL;
    sa->len  = 0;
}

/* -------------------------------------------------------------------------
 * Test 1: Full network diagram — 4 nodes, 4 edges
 * ------------------------------------------------------------------------- */

static void test_full_diagram(void)
{
    /* --- Node CSV: id, label, x, y --- */
    const char *node_csv =
        "id,label,x,y\n"
        "1,Alpha,100,100\n"
        "2,Beta,300,100\n"
        "3,Gamma,100,300\n"
        "4,Delta,300,300\n";

    /* --- Edge CSV: from, to --- */
    const char *edge_csv =
        "from,to\n"
        "1,2\n"
        "2,4\n"
        "4,3\n"
        "3,1\n";

    /* Parse nodes */
    uint64_t node_nrows = 0;
    StrArray *node_rows = csv_parse(node_csv, (uint64_t)strlen(node_csv),
                                    &node_nrows);
    ASSERT(node_rows != NULL, "full: node CSV parsed");
    ASSERT(node_nrows == 5, "full: 5 rows (1 header + 4 data)");

    /* Header indices */
    StrArray hdr = node_rows[0];
    int ci_id    = col_index(hdr, "id");
    int ci_label = col_index(hdr, "label");
    int ci_x     = col_index(hdr, "x");
    int ci_y     = col_index(hdr, "y");
    ASSERT(ci_id >= 0 && ci_label >= 0 && ci_x >= 0 && ci_y >= 0,
           "full: node header columns found");

    /* Collect node positions for edge lookup (indexed by id 1..4). */
    double nx[5] = {0}, ny[5] = {0};
    const char *nlabel[5] = {NULL};
    for (uint64_t r = 1; r < node_nrows; r++) {
        int id = atoi(node_rows[r].data[ci_id]);
        nx[id] = atof(node_rows[r].data[ci_x]);
        ny[id] = atof(node_rows[r].data[ci_y]);
        nlabel[id] = node_rows[r].data[ci_label];
    }

    /* Build SVG document */
    TkSvgDoc *doc = svg_doc(500.0, 500.0);
    ASSERT(doc != NULL, "full: svg_doc created");

    TkSvgStyle node_style = svg_style("#4a90d9", "#333", 2.0);
    TkSvgStyle edge_style = svg_style(NULL, "#666", 1.5);
    TkSvgStyle text_style = svg_style("#000", NULL, 0.0);
    text_style.font_size   = 14.0;
    text_style.font_family = "sans-serif";

    /* Parse edges and add lines */
    uint64_t edge_nrows = 0;
    StrArray *edge_rows = csv_parse(edge_csv, (uint64_t)strlen(edge_csv),
                                    &edge_nrows);
    ASSERT(edge_rows != NULL, "full: edge CSV parsed");
    ASSERT(edge_nrows == 5, "full: 5 edge rows (1 header + 4 data)");

    StrArray ehdr = edge_rows[0];
    int ci_from = col_index(ehdr, "from");
    int ci_to   = col_index(ehdr, "to");
    ASSERT(ci_from >= 0 && ci_to >= 0, "full: edge header columns found");

    for (uint64_t r = 1; r < edge_nrows; r++) {
        int from = atoi(edge_rows[r].data[ci_from]);
        int to   = atoi(edge_rows[r].data[ci_to]);
        svg_append(doc, svg_line(nx[from], ny[from], nx[to], ny[to],
                                 edge_style));
    }

    /* Add circles and text labels for each node */
    for (uint64_t r = 1; r < node_nrows; r++) {
        int id = atoi(node_rows[r].data[ci_id]);
        svg_append(doc, svg_circle(nx[id], ny[id], 20.0, node_style));
        svg_append(doc, svg_text(nx[id], ny[id] - 25.0, nlabel[id],
                                 text_style));
    }

    /* Render */
    const char *svg = svg_render(doc);
    ASSERT(svg != NULL, "full: svg_render returned non-NULL");

    /* Verify element counts */
    int circles = count_occurrences(svg, "<circle");
    int lines   = count_occurrences(svg, "<line");
    int texts   = count_occurrences(svg, "<text");

    ASSERT(circles == 4, "full: 4 <circle> elements");
    ASSERT(lines == 4,   "full: 4 <line> elements");
    ASSERT(texts == 4,   "full: 4 <text> elements");

    /* Verify labels appear */
    ASSERT(strstr(svg, "Alpha") != NULL, "full: label 'Alpha' present");
    ASSERT(strstr(svg, "Beta")  != NULL, "full: label 'Beta' present");
    ASSERT(strstr(svg, "Gamma") != NULL, "full: label 'Gamma' present");
    ASSERT(strstr(svg, "Delta") != NULL, "full: label 'Delta' present");

    /* Verify SVG wrapper */
    ASSERT(strstr(svg, "<svg") != NULL,  "full: opening <svg> tag");
    ASSERT(strstr(svg, "</svg>") != NULL, "full: closing </svg> tag");

    /* Clean up */
    svg_free(doc);
    for (uint64_t r = 0; r < node_nrows; r++) free_strarray(&node_rows[r]);
    free(node_rows);
    for (uint64_t r = 0; r < edge_nrows; r++) free_strarray(&edge_rows[r]);
    free(edge_rows);
}

/* -------------------------------------------------------------------------
 * Test 2: Empty dataset — 0 nodes, 0 edges
 * ------------------------------------------------------------------------- */

static void test_empty_dataset(void)
{
    /* Header-only CSVs */
    const char *node_csv = "id,label,x,y\n";
    const char *edge_csv = "from,to\n";

    uint64_t node_nrows = 0;
    StrArray *node_rows = csv_parse(node_csv, (uint64_t)strlen(node_csv),
                                    &node_nrows);
    /* A header-only CSV still yields 1 row (the header). */
    ASSERT(node_rows != NULL, "empty: node CSV parsed");
    ASSERT(node_nrows == 1,   "empty: 1 row (header only)");

    uint64_t edge_nrows = 0;
    StrArray *edge_rows = csv_parse(edge_csv, (uint64_t)strlen(edge_csv),
                                    &edge_nrows);
    ASSERT(edge_rows != NULL, "empty: edge CSV parsed");

    /* Build SVG with no elements appended */
    TkSvgDoc *doc = svg_doc(400.0, 400.0);
    ASSERT(doc != NULL, "empty: svg_doc created");

    const char *svg = svg_render(doc);
    ASSERT(svg != NULL, "empty: svg_render returned non-NULL");

    int circles = count_occurrences(svg, "<circle");
    int lines   = count_occurrences(svg, "<line");
    int texts   = count_occurrences(svg, "<text");

    ASSERT(circles == 0, "empty: 0 <circle> elements");
    ASSERT(lines == 0,   "empty: 0 <line> elements");
    ASSERT(texts == 0,   "empty: 0 <text> elements");

    /* Still a valid SVG document */
    ASSERT(strstr(svg, "<svg") != NULL,  "empty: opening <svg> tag");
    ASSERT(strstr(svg, "</svg>") != NULL, "empty: closing </svg> tag");

    svg_free(doc);
    for (uint64_t r = 0; r < node_nrows; r++) free_strarray(&node_rows[r]);
    free(node_rows);
    for (uint64_t r = 0; r < edge_nrows; r++) free_strarray(&edge_rows[r]);
    free(edge_rows);
}

/* -------------------------------------------------------------------------
 * Test 3: Single node, no edges
 * ------------------------------------------------------------------------- */

static void test_single_node(void)
{
    const char *node_csv =
        "id,label,x,y\n"
        "1,Singleton,200,200\n";

    const char *edge_csv = "from,to\n";

    uint64_t node_nrows = 0;
    StrArray *node_rows = csv_parse(node_csv, (uint64_t)strlen(node_csv),
                                    &node_nrows);
    ASSERT(node_rows != NULL, "single: node CSV parsed");
    ASSERT(node_nrows == 2,   "single: 2 rows (1 header + 1 data)");

    uint64_t edge_nrows = 0;
    StrArray *edge_rows = csv_parse(edge_csv, (uint64_t)strlen(edge_csv),
                                    &edge_nrows);

    StrArray hdr = node_rows[0];
    int ci_x     = col_index(hdr, "x");
    int ci_y     = col_index(hdr, "y");
    int ci_label = col_index(hdr, "label");

    double x = atof(node_rows[1].data[ci_x]);
    double y = atof(node_rows[1].data[ci_y]);
    const char *label = node_rows[1].data[ci_label];

    TkSvgDoc *doc = svg_doc(400.0, 400.0);
    ASSERT(doc != NULL, "single: svg_doc created");

    TkSvgStyle ns = svg_style("#4a90d9", "#333", 2.0);
    TkSvgStyle ts = svg_style("#000", NULL, 0.0);
    ts.font_size   = 14.0;
    ts.font_family = "sans-serif";

    svg_append(doc, svg_circle(x, y, 20.0, ns));
    svg_append(doc, svg_text(x, y - 25.0, label, ts));

    const char *svg = svg_render(doc);
    ASSERT(svg != NULL, "single: svg_render returned non-NULL");

    int circles = count_occurrences(svg, "<circle");
    int lines   = count_occurrences(svg, "<line");
    int texts   = count_occurrences(svg, "<text");

    ASSERT(circles == 1, "single: 1 <circle> element");
    ASSERT(lines == 0,   "single: 0 <line> elements");
    ASSERT(texts == 1,   "single: 1 <text> element");

    ASSERT(strstr(svg, "Singleton") != NULL, "single: label 'Singleton' present");

    svg_free(doc);
    for (uint64_t r = 0; r < node_nrows; r++) free_strarray(&node_rows[r]);
    free(node_rows);
    for (uint64_t r = 0; r < edge_nrows; r++) free_strarray(&edge_rows[r]);
    free(edge_rows);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    printf("=== test_svg_diagram_integration ===\n");

    test_full_diagram();
    test_empty_dataset();
    test_single_node();

    printf("\n%s (%d failure%s)\n",
           failures ? "FAIL" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
