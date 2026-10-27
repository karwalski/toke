/* SPDX-License-Identifier: MIT */
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
#include "tk_strhdr.h"  /* 127.170: length-carrying strings (tk_strlen) */
#include "args.h"
#include "capabilities.h"  /* 124.4a: capability broker init */
#include "strbuf.h"        /* 127.229: JSON string escape/unescape */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* 114.41: side channel carrying a T!$E error's typed payload.  An error
 * return stores the box here and returns the 0 ok/err sentinel; the matching
 * $err arm loads it.  runtime-abi.md §7.
 *
 * 127.101: thread-local, as llvm.c always claimed it was and as this file
 * always denied.  std.task's pool_worker invokes a toke function pointer on a
 * worker thread, so a shared slot really did let two threads overwrite each
 * other's error. */
TK_TLS int64_t tk_current_error = 0;

/* ── 127.109/127.49: the per-thread error box ────────────────────────────────
 * One buffer per thread, grown on demand, reused by every raise.  See the
 * ownership rule on tk_err_box() in tk_runtime.h: a box is valid until the
 * next raise on the same thread, which is the window §7.5 restriction 1
 * already documents for the $err binding.
 *
 * `err_buf` is deliberately NOT freed on grow-in-place failure: realloc
 * leaves the old block valid, so keeping it is correct and the raise falls
 * back to the payload-less flag. */
/* One TLS object, not two: on Darwin every distinct thread-local reference
 * goes through a tlv_get_addr thunk, so folding the buffer and its capacity
 * into one struct halves the lookups on the raise path. */
/* The buffer starts as an INLINE thread-local array, so the overwhelmingly
 * common error box — a 2-slot sum (16 bytes) or a record of up to 8 fields —
 * needs no allocation at all and, more importantly, CANNOT FAIL.  A failed
 * allocation here would hand codegen a null box, which it would store into the
 * slot as 0 — and 0 is the SUCCESS value (§7.2), so an out-of-memory error
 * return would have been read back as a successful one.  That hazard existed
 * with the old per-error malloc() too; sizing the common case inline removes
 * it rather than moving it.  Only an error type wider than this reaches the
 * heap, and the null it can still return is handled by the caller below. */
#define TK_ERR_INLINE 64
typedef struct { void *buf; int64_t cap; } TkErrBuf;
static TK_TLS TkErrBuf       err = { NULL, 0 };
static TK_TLS unsigned char  err_inline[TK_ERR_INLINE];
#define err_buf err.buf
#define err_cap err.cap

void tk_err_release(void)
{
    if (err_buf && err_buf != (void *)err_inline) free(err_buf);
    err_buf = NULL;
    err_cap = 0;
}

void *tk_err_box(int64_t nbytes)
{
    if (nbytes <= 0) nbytes = 16;
    if (!err_buf) { err_buf = err_inline; err_cap = TK_ERR_INLINE; }
    if (nbytes > err_cap) {
        /* Round up so a mixed program of narrow and wide error types reaches a
         * steady state instead of reallocating on every alternation. */
        int64_t want = err_cap;
        while (want < nbytes) {
            if (want > (int64_t)1 << 20) { want = nbytes; break; }  /* no doubling runaway */
            want *= 2;
        }
        /* The inline array is not realloc'able. */
        void *grown = (err_buf == (void *)err_inline) ? malloc((size_t)want)
                                                      : realloc(err_buf, (size_t)want);
        if (!grown) {
            /* Out of memory for an oversized error type.  Return the inline
             * buffer rather than null: a short box risks a truncated payload,
             * but a null box would be stored as 0 and read back as SUCCESS,
             * turning a failure into a silent success. */
            err_buf = err_inline; err_cap = TK_ERR_INLINE;
            memset(err_buf, 0, TK_ERR_INLINE);
            return err_buf;
        }
        err_buf = grown;
        err_cap = want;
    }
    memset(err_buf, 0, (size_t)nbytes);
    return err_buf;
}

/* ── 127.103: a let-bound error keeps its own copy of the box ────────────
 * The slot holds a pointer into the ONE reused per-thread box (127.109), so a
 * binding that spilled the pointer (135.16) captured nothing: the next raise
 * on the thread rewrote the box, and `let a=f(0); let b=f(-1); mt a{…}` read
 * b's payload.  The binding now copies the box into a buffer of its own at
 * the `let` (tk_err_keep), and the `$err` arm copies it back into the thread
 * box (tk_err_rebox) so the bound `e` has exactly the lifetime the direct
 * spelling's has — valid until the next raise — and never points into a
 * frame.  0 (ok) and 1 (payload-less) are not pointers and pass through. */
int64_t tk_err_keep(int64_t v, void *keep, int64_t nbytes)
{
    if (v == 0 || v == 1 || !keep || nbytes <= 0) return v;
    memcpy(keep, (const void *)(intptr_t)v, (size_t)nbytes);
    return (int64_t)(intptr_t)keep;
}

int64_t tk_err_rebox(int64_t v, int64_t nbytes)
{
    if (v == 0 || v == 1 || nbytes <= 0) return v;
    if ((void *)(intptr_t)v == err_buf) return v;   /* already the thread box */
    void *b = tk_err_box(nbytes);
    memmove(b, (const void *)(intptr_t)v, (size_t)nbytes);
    return (int64_t)(intptr_t)b;
}

/* ── 127.102/127.241: a raised message that outlives the next call ────────
 * A stdlib error box holds a POINTER to its message.  Several C cores keep
 * their last message in one static buffer that the next call overwrites
 * (file.c, csv.c, ocr.c, pdf.c), and a let-bound error keeps a copy of its
 * box, not of the string (127.103) — so a message pointing into that buffer
 * would change under a binding the caller still holds.  tk_err_msg formats
 * into the next slot of a small per-thread ring instead: a message stays
 * exact across the next TK_ERR_MSG_RING - 1 raises on its thread, with no
 * allocation per error (127.109).  A message that is a string literal needs
 * none of this and should be stored directly. */
#define TK_ERR_MSG_RING 8
#define TK_ERR_MSG_LEN  1024
static TK_TLS char err_msg_ring[TK_ERR_MSG_RING][TK_ERR_MSG_LEN];
static TK_TLS int  err_msg_next;

const char *tk_err_msg(const char *fmt, ...)
{
    char *m = err_msg_ring[err_msg_next];
    err_msg_next = (err_msg_next + 1) % TK_ERR_MSG_RING;
    va_list ap;
    if (!fmt) { m[0] = '\0'; return m; }
    va_start(ap, fmt);
    vsnprintf(m, TK_ERR_MSG_LEN, fmt, ap);
    va_end(ap);
    return m;
}

/* ── Global argv storage ─────────────────────────────────────────── */

static int    g_argc;
static char **g_argv;

/* AMB-03: snapshot of PATH captured at startup (before any program code can
 * env.set/dotenv-load a hostile PATH), used by process exec so command lookup
 * never trusts the live, mutable environment. */
static char *g_path_snapshot;

const char *tk_path_snapshot(void) { return g_path_snapshot; }

void tk_runtime_init(int argc, char **argv) {
    /* 127.109: release the main thread's error box at exit.  Registered HERE,
     * not lazily from tk_err_box: atexit handlers run on the main thread, so a
     * lazy registration from a std.task worker would enter the handler once
     * per raising thread and each entry would act on the MAIN thread's TLS. */
    atexit(tk_err_release);
    /* 124.4a/g: parse capability grants from the RAW argv first (tk_cap_init
     * must see the --allow-* flags). */
    tk_cap_init(argc, argv);

    /* AMB-03: snapshot PATH now, before any toke code runs. */
    {
        const char *p = getenv("PATH");
        g_path_snapshot = p ? strdup(p) : NULL;
    }

    /* 124.4g: strip the consumed --allow-* tokens so the program never sees the
     * capability flags in its own argv (Deno-style). argv[0] is preserved; the
     * surviving entries still point into the OS argv strings (only the pointer
     * array is new). Both g_argv and args_init must get the filtered vector —
     * std.args keeps its own copy of the pointers. */
    if (argc > 0 && argv) {
        char **filtered = (char **)malloc(sizeof(char *) * (size_t)argc);
        if (filtered) {
            int fn = 0;
            for (int i = 0; i < argc; i++) {
                if (i > 0 && tk_cap_is_grant_flag(argv[i])) continue;
                filtered[fn++] = argv[i];
            }
            g_argc = fn;
            g_argv = filtered;
            args_init(fn, filtered);
            return;
        }
    }
    /* Fallback (no argv / malloc failed): unfiltered. */
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

/* 127.229: a JSON string, decoded.  *pp points at the opening quote; it is
 * left just past the closing one.  The escapes (\" \\ \n \uXXXX ...) are
 * decoded by strbuf_append_json_unescape; before this the bytes between the
 * quotes were copied verbatim, so \" ENDED the string and \\, \n, \u00e9
 * were kept as their escape text.  Returns a heap string ("" on OOM). */
static char *parse_json_string(const char **pp) {
    const char *p = *pp;
    if (*p == '"') p++;
    TkStrBuf b = TK_STRBUF_INIT;
    (void)strbuf_append_json_unescape(&b, &p);
    *pp = p;
    char *s = strbuf_take(&b);
    return s ? s : (char *)calloc(1, 1);
}

/* 127.229: skip a JSON string whose opening quote is at p; return the byte
 * after its closing quote (or the NUL).  An escaped quote does not end it. */
static const char *skip_json_string(const char *p) {
    if (*p == '"') p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p == '"' ? p + 1 : p;
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
            /* 127.229: a ',' '[' or ']' inside a string element is text,
             * not structure: ["a,b"] is one element. */
            if (*scan == '"') { scan = skip_json_string(scan); continue; }
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
            /* String element, escapes decoded (127.229) */
            data[i] = (int64_t)(intptr_t)parse_json_string(&p);
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

    /* String — the decoded content (127.229) */
    if (*p == '"')
        return (int64_t)(intptr_t)parse_json_string(&p);

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

/* 127.229: one JSON string literal to stdout, quoted and escaped by
 * strbuf_append_json (", \\ and every control byte).  The printers below
 * escaped '"' (and some '\\') by hand and wrote control characters raw, so
 * a parsed "x\ny" printed as a string literal with a real newline in it. */
static void put_json_string(const char *s) {
    TkStrBuf b = TK_STRBUF_INIT;
    strbuf_putc(&b, '"');
    strbuf_append_json(&b, s);
    strbuf_putc(&b, '"');
    if (!b.oom) fwrite(b.buf, 1, b.len, stdout);
    strbuf_free(&b);
}

void tk_json_print_str(const char *val) {
    if (!val) { printf("null\n"); return; }
    put_json_string(val);
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
        if (data[i]) put_json_string(data[i]);
        else fputs("null", stdout);
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

/* Concatenate two C strings.  Returns a new sealed string (127.170,
 * tk_strhdr.h): never freed, never written. */
char *tk_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = tk_strlen(a), lb = tk_strlen(b);
    char *r = tks_alloc(la + lb);         /* 127.170: sealed below */
    if (!r) abort();
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = '\0';
    return tks_seal(r, la + lb);
}

/* 127.26: one-allocation string interpolation. `n` NUL-terminated i8* parts
 * follow as varargs (NULL reads as ""); the result is a fresh NUL-terminated
 * heap string of their concatenation. Codegen lowers every `"…\(e)…"` literal
 * with >= 2 segments to a single call here instead of a pairwise
 * tk_str_concat chain (k-1 allocations) — docs/spec/semantics.md 2.4. */
char *tk_str_join_n(int64_t n, ...) {
    enum { STACK_PARTS = 32 };
    const char *stack_parts[STACK_PARTS];
    size_t stack_lens[STACK_PARTS];
    const char **parts = stack_parts;
    size_t *lens = stack_lens;
    if (n < 0) n = 0;
    if (n > STACK_PARTS) {
        parts = (const char **)malloc((size_t)n * sizeof *parts);
        lens = (size_t *)malloc((size_t)n * sizeof *lens);
        if (!parts || !lens) abort();
    }
    size_t total = 0;
    va_list ap;
    va_start(ap, n);
    for (int64_t i = 0; i < n; i++) {
        const char *p = va_arg(ap, const char *);
        if (!p) p = "";
        parts[i] = p;
        lens[i] = tk_strlen(p);      /* 127.170: O(1) for a sealed part */
        total += lens[i];
    }
    va_end(ap);
    char *r = tks_alloc(total);           /* 127.170: sealed below */
    if (!r) abort();
    size_t off = 0;
    for (int64_t i = 0; i < n; i++) {
        memcpy(r + off, parts[i], lens[i]);
        off += lens[i];
    }
    r[off] = '\0';
    if (parts != stack_parts) { free(parts); free(lens); }
    return tks_seal(r, off);
}

/* String length for C strings. */
int64_t tk_str_len(const char *s) {
    return s ? (int64_t)tk_strlen(s) : 0;
}

/*
 * NULL-safe three-way string comparison (story 127.83).  See tk_runtime.h for
 * why NULL is ordered before "" rather than being treated as equal to it.
 */
int64_t tk_str_cmp(const char *a, const char *b) {
    if (a == b) return 0;          /* covers NULL==NULL and pointer identity */
    if (!a) return -1;             /* missing sorts before every string      */
    if (!b) return 1;
    int r = strcmp(a, b);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

/* Character at index for C strings (returns the char code as i64). */
int64_t tk_str_char_at(const char *s, int64_t idx) {
    if (!s || idx < 0 || idx >= (int64_t)tk_strlen(s)) return 0;
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
        put_json_string(s);
        putchar('\n');
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
    /* 121.36: codes 2/3 are the other invalid division — the most negative
     * value over -1, whose quotient does not fit (ADR-0012 b). */
    if (op_code >= 2)
        fprintf(stderr, "RT004: invalid %s: overflow (most negative value by -1)\n",
                op_code == 3 ? "remainder" : "division");
    else
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

/* 124.2d: RT005 — nil dereference. Reading a field of a null struct pointer is
 * undefined; toke traps deterministically instead of reading wild memory. */
void tk_nil_trap(int32_t code) {
    (void)code;
    fprintf(stderr, "RT005: nil dereference\n");
    exit(1);
}

/* 127.225: RT010 -- `!` re-raised an error of one user error type into a
 * function declaring another, on a path the checker (E4076) could not judge.
 * The error cannot be carried across, and inventing a variant of the other
 * type would hand the caller a wrong answer; toke stops instead. */
void tk_reraise_trap(const char *from, const char *to) {
    fprintf(stderr, "RT010: error of type $%s re-raised with '!' into a "
            "function whose error type is $%s\n",
            from ? from : "?", to ? to : "?");
    exit(1);
}

/* ── 124.0c item 3: closure environments are reference counted ──────────────
 * memory-model §2.5: a closure's environment is malloc'd so it can outlive
 * the scope that made it, and "is freed when the last reference to the
 * closure is released".  The compiler lays each environment out as
 *
 *     base[0]  reference count
 *     base[1]  capture mask: bit i set = capture i is itself a closure value,
 *              owned by this environment and released with it
 *     base[2]  the lifted function's address        <- env (what it is called with)
 *     base[3+i] capture i
 *
 * and represents the closure VALUE as env + 1.  malloc'd storage is at least
 * 8-aligned, so the low bit tells a closure apart from a plain function
 * address (`&add`), which the same function-typed slot may hold and which is
 * never counted.  0 (an unset binding) is neither.  The count is atomic: a
 * closure value may reach another thread through std.task. */
static int64_t *tk_clos_base(int64_t v)
{
    return (int64_t *)(intptr_t)((v - 1) - 2 * (int64_t)sizeof(int64_t));
}

void tk_clos_retain(int64_t v)
{
    if (!(v & 1)) return;
    __atomic_fetch_add(&tk_clos_base(v)[0], 1, __ATOMIC_RELAXED);
}

void tk_clos_release(int64_t v)
{
    while (v & 1) {
        int64_t *b = tk_clos_base(v);
        if (__atomic_sub_fetch(&b[0], 1, __ATOMIC_ACQ_REL) != 0) return;
        uint64_t mask = (uint64_t)b[1];
        int64_t last = 0;
        /* release every owned capture; the last one iteratively, so a long
         * chain of closures capturing closures does not recurse per link */
        for (int i = 0; mask; i++, mask >>= 1)
            if (mask & 1u) {
                if (last & 1) tk_clos_release(last);
                last = b[3 + i];
            }
        free(b);
        v = last;
    }
}
