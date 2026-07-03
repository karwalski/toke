/*
 * tk_runtime.c — Lightweight runtime for toke benchmark programs.
 *
 * Provides JSON I/O, argv access, and array allocation so compiled
 * toke programs can read JSON input from argv and print JSON output.
 *
 * Story: 2.8.2
 */

#include "tk_runtime.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 114.41: side channel carrying a T!$E error's typed sum-type payload.
 * An error return stores the box here and returns the 0 ok/err sentinel; the
 * matching $err arm loads it. (Plain global — adequate for the single-threaded
 * common case; revisit for cross-thread error propagation.) */
int64_t tk_current_error = 0;

/* ── Global argv storage ─────────────────────────────────────────── */

static int    g_argc;
static char **g_argv;

void tk_runtime_init(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    args_init(argc, argv);
}

const char *tk_str_argv(int64_t index) {
    if (index < 0 || index >= g_argc) return "";
    return g_argv[index];
}

/* ── JSON parser (minimal, for benchmark I/O) ────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Parse an integer from JSON, advancing *pp past it. */
static int64_t parse_int(const char **pp) {
    const char *p = *pp;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -v : v;
}

/* Allocate a toke-runtime array: [len | data...].
 * Returns pointer to data[0]; len stored at ptr[-1]. */
static int64_t *alloc_array(int64_t len) {
    int64_t h = tk_arr_alloc(len, len);
    return (int64_t *)(intptr_t)h;  /* pointer to first data element (or NULL on OOM) */
}

/* Parse a JSON array of integers: [1, 2, 3] */
static int64_t *parse_int_array(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p != '[') { *pp = p; return alloc_array(0); }
    p++;

    /* First pass: count elements */
    const char *scan = p;
    int64_t count = 0;
    scan = skip_ws(scan);
    if (*scan != ']') {
        count = 1;
        int depth = 0;
        while (*scan) {
            if (*scan == '[') depth++;
            else if (*scan == ']') { if (depth == 0) break; depth--; }
            else if (*scan == ',' && depth == 0) count++;
            scan++;
        }
    }

    int64_t *data = alloc_array(count);

    /* Second pass: parse values (handles nested arrays, strings, bools) */
    p = skip_ws(p);
    for (int64_t i = 0; i < count; i++) {
        p = skip_ws(p);
        if (*p == '[') {
            /* Nested array: recursively parse and store pointer as i64 */
            int64_t *sub = parse_int_array(&p);
            data[i] = (int64_t)(intptr_t)sub;
        } else if (*p == '"') {
            /* String element */
            p++;
            const char *start = p;
            while (*p && *p != '"') p++;
            size_t len = (size_t)(p - start);
            char *s = (char *)malloc(len + 1);
            memcpy(s, start, len);
            s[len] = '\0';
            if (*p == '"') p++;
            data[i] = (int64_t)(intptr_t)s;
        } else if (strncmp(p, "true", 4) == 0) {
            data[i] = 1; p += 4;
        } else if (strncmp(p, "false", 5) == 0) {
            data[i] = 0; p += 5;
        } else {
            data[i] = parse_int(&p);
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    p = skip_ws(p);
    if (*p == ']') p++;
    *pp = p;
    return data;
}

int64_t tk_json_parse(const char *json) {
    const char *p = skip_ws(json);

    /* Array */
    if (*p == '[') {
        int64_t *arr = parse_int_array(&p);
        return (int64_t)(intptr_t)arr;
    }

    /* Boolean */
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;

    /* String — return pointer to the unquoted content */
    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        size_t len = (size_t)(p - start);
        char *s = (char *)malloc(len + 1);
        memcpy(s, start, len);
        s[len] = '\0';
        return (int64_t)(intptr_t)s;
    }

    /* Integer (including negative) */
    return parse_int(&p);
}

/* ── JSON printers ───────────────────────────────────────────────── */

void tk_json_print_i64(int64_t val) {
    printf("%lld\n", (long long)val);
}

void tk_json_print_bool(int64_t val) {
    printf("%s\n", val ? "true" : "false");
}

void tk_json_print_str(const char *val) {
    if (!val) { printf("null\n"); return; }
    putchar('"');
    while (*val) {
        if (*val == '"') fputs("\\\"", stdout);
        else if (*val == '\\') fputs("\\\\", stdout);
        else putchar(*val);
        val++;
    }
    putchar('"');
    putchar('\n');
}

void tk_json_print_arr(int64_t *data) {
    int64_t len = data[-1];
    putchar('[');
    for (int64_t i = 0; i < len; i++) {
        if (i) putchar(',');
        printf("%lld", (long long)data[i]);
    }
    putchar(']');
    putchar('\n');
}

void tk_json_print_arr_bool(int64_t *data) {
    int64_t len = data[-1];
    putchar('[');
    for (int64_t i = 0; i < len; i++) {
        if (i) putchar(',');
        fputs(data[i] ? "true" : "false", stdout);
    }
    putchar(']');
    putchar('\n');
}

void tk_json_print_f64(double val) {
    printf("%g\n", val);
}

void tk_json_print_arr_str(const char **data, int64_t len) {
    putchar('[');
    for (int64_t i = 0; i < len; i++) {
        if (i) putchar(',');
        putchar('"');
        const char *s = data[i];
        while (*s) {
            if (*s == '"') fputs("\\\"", stdout);
            else putchar(*s);
            s++;
        }
        putchar('"');
    }
    putchar(']');
    putchar('\n');
}

/* ── Array / string concat ──────────────────────────────────────── */

/* Concatenate two length-prefixed i64 arrays.
 * Returns pointer to data[0] of a new array. */
int64_t *tk_array_concat(int64_t *a, int64_t *b) {
    int64_t la = a ? a[-1] : 0;
    int64_t lb = b ? b[-1] : 0;
    int64_t total = la + lb;
    int64_t h = tk_arr_alloc(total, total);
    if (!h) return NULL;
    int64_t *data = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < la; i++) data[i] = a[i];
    for (int64_t i = 0; i < lb; i++) data[la + i] = b[i];
    return data;
}

/* Concatenate two C strings. Returns new malloc'd string. */
char *tk_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *r = (char *)malloc(la + lb + 1);
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return r;
}

/* String length for C strings. */
int64_t tk_str_len(const char *s) {
    return s ? (int64_t)strlen(s) : 0;
}

/* Character at index for C strings (returns the char code as i64). */
int64_t tk_str_char_at(const char *s, int64_t idx) {
    if (!s || idx < 0 || idx >= (int64_t)strlen(s)) return 0;
    return (int64_t)(unsigned char)s[idx];
}

/* Generic print: detect type heuristically from the i64 value.
 * - Heap pointers (from json_parse returning strings/arrays) are large values
 *   outside the typical integer range on 64-bit systems.
 * - Small values (fitting in 32 bits) are treated as integers.
 * - Pointer-range values are checked: if they look like an int array
 *   (first element is the length, stored by parse_int_array), print as array.
 *   Otherwise try as string. */
void tk_json_print(int64_t val) {
    /* Small integers and booleans: print directly */
    if (val >= -1000000000LL && val <= 1000000000LL) {
        printf("%lld\n", (long long)val);
        return;
    }

    /* Likely a pointer — determine if it's a string or array.
     *
     * Heuristic: strings are char pointers where the first bytes are
     * printable ASCII. Arrays are int64_t pointers where arr[-1] holds
     * the length (set by alloc_array).
     *
     * Check string first: if the first char is printable ASCII (32-126),
     * it's almost certainly a string from json_parse. */
    const char *s = (const char *)(intptr_t)val;
    unsigned char first = (unsigned char)s[0];
    if (first >= 32 && first <= 126) {
        /* Looks like a string */
        printf("\"%s\"\n", s);
        return;
    }

    /* Otherwise try as array (alloc_array format: length at arr[-1]) */
    int64_t *arr = (int64_t *)(intptr_t)val;
    int64_t maybe_len = arr[-1];
    if (maybe_len >= 0 && maybe_len < 100000) {
        printf("[");
        for (int64_t i = 0; i < maybe_len; i++) {
            if (i > 0) printf(",");
            printf("%lld", (long long)arr[i]);
        }
        printf("]\n");
        return;
    }

    /* Fallback */
    printf("%lld\n", (long long)val);
}

/* ── Checked arithmetic trap (D2=E) ────────────────────────────────── */

void tk_overflow_trap(int32_t op_code) {
    static const char *ops[] = {"addition", "subtraction", "multiplication"};
    const char *name = (op_code >= 0 && op_code <= 2) ? ops[op_code] : "arithmetic";
    fprintf(stderr, "RT002: integer overflow in %s\n", name);
    exit(1);
}

/* 124.2b: RT004 — division/remainder by zero. `a/0` and `a%%0` are undefined
 * behaviour at the machine level; toke traps deterministically instead. */
void tk_div_trap(int32_t op_code) {
    fprintf(stderr, "RT004: %s by zero\n", op_code == 1 ? "remainder" : "division");
    exit(1);
}

/* 124.2a: RT003 — array subscript out of bounds. `arr[i]`/`arr.get(i)` with i
 * outside [0, len) is undefined at the machine level; toke traps instead. */
void tk_bounds_trap(int64_t idx, int64_t len) {
    fprintf(stderr, "RT003: index %lld out of bounds for length %lld\n",
            (long long)idx, (long long)len);
    exit(1);
}
