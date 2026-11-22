/* SPDX-License-Identifier: MIT */
/*
 * tk_runtime.h — Lightweight runtime for toke benchmark programs.
 *
 * Provides JSON I/O and argv access so that compiled toke programs
 * can read JSON input from argv[1] and print JSON output to stdout.
 *
 * Array layout: pointer to contiguous i64 block where element at
 * index -1 stores the length.  i.e. ptr points to data[0], and
 * ptr[-1] == length.
 *
 * Story: 2.8.2
 */

#ifndef TK_RUNTIME_H
#define TK_RUNTIME_H

#include <stdint.h>
#include <string.h>   /* memset, tk_err_box_fast */

/* ── Story 127.101/127.109: the error channel ────────────────────────────────
 * runtime-abi.md §7.  `tk_current_error` is the tri-valued slot (0 = ok,
 * 1 = failed with no payload, anything else = a pointer to an error box).
 *
 * It is THREAD-LOCAL.  It was a plain global until 127.101, while both
 * llvm.c and the story text described it as thread-local, so two threads in
 * std.task raising errors clobbered each other's and the second caller read
 * the first's error (task.c's pool_worker calls a toke function pointer
 * directly, so this was reachable, not theoretical).
 *
 * TK_TLS, not C11 `_Thread_local`: tk_runtime.c is built with
 * `-std=c99 -Wpedantic -Werror`, under which `_Thread_local` is a hard
 * -Wc11-extensions error.  Anything declaring `tk_current_error` MUST spell
 * it with this macro — a plain-global declaration against a TLS definition
 * links without a diagnostic and then takes SIGBUS on first access. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define TK_TLS _Thread_local
#else
#  define TK_TLS __thread
#endif

/* 127.504: ONE thread-local object holds the slot AND the error box's buffer.
 * On Darwin every distinct thread-local variable a function touches costs a
 * tlv_get_addr call, and a std raise touched two: the box state inside
 * tk_err_box and the slot in the glue.  With both in one object and the box's
 * fast path inline below, a raise in a glue function makes one lookup.
 *
 * The SYMBOL is still `tk_current_error`, and `slot` is its first member, so
 * the address the emitted code loads and stores through
 * (`@tk_current_error = external thread_local global i64`, runtime-abi.md
 * §7.1) is the slot's, exactly as before: codegen is unchanged.  In C the
 * name is a macro for the slot, so `tk_current_error = x` keeps meaning what
 * it did.  The rules that follow:
 *   - never redeclare `tk_current_error` (or tk_err_box) in a file that
 *     includes this header -- it is declared here, as the object;
 *   - a test that stands in for tk_runtime.c defines the OBJECT, not an
 *     int64_t, because inlined glue reads `buf` past the slot:
 *         #undef tk_current_error
 *         TK_TLS TkErrTls tk_current_error;
 *         #define tk_current_error (tk_current_error.slot)
 *     and defines its tk_err_box stub as `void *(tk_err_box)(int64_t n)`.
 * A C file that does not include this header may still declare
 * `extern TK_TLS int64_t tk_current_error;` and reads the slot correctly. */
typedef struct {
    int64_t slot;   /* MUST stay first: the emitted code's @tk_current_error */
    void   *buf;    /* the error box (tk_err_box), NULL until the first raise */
    int64_t cap;    /* its capacity, never below TK_ERR_INLINE once buf is set */
} TkErrTls;
extern TK_TLS TkErrTls tk_current_error;

/* tk_err_box — the thread's error box, at least `nbytes` and zeroed.
 *
 * OWNERSHIP RULE (127.109).  Each thread owns exactly ONE error-box buffer,
 * grown on demand and REUSED by every raise on that thread.  A box is valid
 * until the next raise on the same thread, and so is the `$err` arm's binding
 * of it.  A `let`-bound fallible value, which must outlive later raises, keeps
 * its own copy (tk_err_keep / tk_err_rebox, 127.103).
 *
 * Before this, every error return and every `!` propagation called malloc()
 * and the emitted code contained no free at all: 16 bytes per error EVER
 * raised, unbounded (127.49 measured 9,142,858 boxes / 146 MB at N=64e6).
 * Now it is one buffer per thread, and there is nothing to free per error.
 *
 * NEVER returns NULL.  The common box — a 2-slot sum, or a record of up to 8
 * fields — comes from an inline thread-local array and cannot fail at all;
 * only a wider error type reaches the heap, and a failed growth there falls
 * back to that inline array.  A null return would be stored into the slot as
 * 0, and 0 is SUCCESS (§7.2), so an out-of-memory error return would have read
 * back as a successful one. */
void   *tk_err_box(int64_t nbytes);

/* The common raise, inline (127.504): a box that fits the inline size, once
 * the thread's buffer exists, is zeroed with a constant-size store (127.492)
 * and returned with no call.  Its capacity is never below TK_ERR_INLINE (the
 * buffer starts there and only grows; the OOM fallback returns to it), so
 * zeroing past nbytes stays inside it.  Everything else is the out-of-line
 * tk_err_box, which the emitted code also calls. */
#define TK_ERR_INLINE 64
static inline void *tk_err_box_fast(int64_t nbytes)
{
    TkErrTls *t = &tk_current_error;
    if (nbytes > 0 && nbytes <= TK_ERR_INLINE && t->buf) {
        memset(t->buf, 0, TK_ERR_INLINE);
        return t->buf;
    }
    return (tk_err_box)(nbytes);
}

/* Declared and used above as the object; from here on, the slot. */
#define tk_current_error (tk_current_error.slot)
#define tk_err_box(n) tk_err_box_fast(n)

/* Release this thread's error-box buffer.  Registered via atexit() for the
 * main thread; a worker that raises leaves one buffer behind at thread exit,
 * which is constant, not workload-proportional. */
void    tk_err_release(void);

/* 127.103: copy a let-bound error's box into the binding's own buffer
 * (returns a pointer to the copy, or v itself for 0 / 1), and copy it back
 * into the thread box for the $err arm.  See tk_runtime.c. */
int64_t tk_err_keep(int64_t v, void *keep, int64_t nbytes);
int64_t tk_err_rebox(int64_t v, int64_t nbytes);

/* 127.102/127.241: format a raised error's message into a per-thread ring
 * slot that stays valid across the next 7 raises on the thread (tk_runtime.c).
 * For a message from a C core's static last-error buffer, which the next
 * call overwrites; a string literal needs no copy. */
const char *tk_err_msg(const char *fmt, ...);

/* Store argc/argv for later access by tk_str_argv. */
void    tk_runtime_init(int argc, char **argv);

/* Return argv[index] as a C string (ptr). */
const char *tk_str_argv(int64_t index);

/* AMB-03: PATH snapshot captured at tk_runtime_init (or NULL). Used by process
 * exec so command resolution never trusts the live, mutable PATH. */
const char *tk_path_snapshot(void);

/* Parse a JSON string into a toke runtime value.
 * Returns an i64:
 *   - for integers: the value directly
 *   - for arrays of integers: pointer to array data (len at ptr[-1])
 *   - for booleans: 0 or 1
 *   - for strings: pointer to C string
 */
int64_t tk_json_parse(const char *json);

/* Print a toke runtime value as JSON to stdout.
 * val_type hint: 0=i64, 1=bool, 2=string, 3=array of i64,
 *                4=f64, 5=array of bool
 * If val_type is unknown, tries to print as i64. */
void    tk_json_print_i64(int64_t val);
void    tk_json_print_bool(int64_t val);
void    tk_json_print_str(const char *val);
void    tk_json_print_arr(int64_t *data);
void    tk_json_print_f64(double val);
void    tk_json_print_arr_str(const char **data, int64_t len);
void    tk_json_print_arr_bool(int64_t *data);

/* Array / string operations */
int64_t *tk_array_concat(int64_t *a, int64_t *b);
char   *tk_str_concat(const char *a, const char *b);
char   *tk_str_join_n(int64_t n, ...);  /* 127.26: one-alloc interpolation */
int64_t tk_str_len(const char *s);
int64_t tk_str_char_at(const char *s, int64_t idx);

/*
 * tk_str_cmp — NULL-safe three-way string comparison (story 127.83).
 *
 * `==`, `!=` and the ordering operators on $str lowered straight to strcmp(),
 * which dereferences its arguments.  An optional `?(T)` yields the NULL
 * sentinel on a miss, so the documented way to test for a missing value
 * (`keychain.get(...) == ""`) segfaulted the caller.
 *
 * NULL is the "missing" sentinel and is NOT the empty string: a wiped secret
 * must stay distinguishable from an empty one (see docs/stdlib/securemem.md), and
 * TOML/JSON "absent" must stay distinguishable from "present but empty"
 * (test/conform/C005).  NULL therefore sorts before every string, "" included.
 *
 * Returns <0, 0 or >0 like strcmp().
 */
int64_t tk_str_cmp(const char *a, const char *b);

/* Generic print: prints i64 by default (most benchmark tasks). */
void    tk_json_print(int64_t val);

/* Checked arithmetic overflow trap (D2=E).
 * op_code: 0=add, 1=sub, 2=mul. Prints RT002 and exits. */
void    tk_overflow_trap(int32_t op_code);

/* 124.0c item 3: closure environment reference counting (tk_runtime.c). */
void tk_clos_retain(int64_t v);
void tk_clos_release(int64_t v);

/* ── 138.45: the registry of live std handles (tk_runtime.c) ─────────────────
 * One table for every std module that hands a C object to a toke program as
 * an i64 (lifted from 138.36's process registry).  A slot holds the object's
 * address, its KIND and a serial; lookups read only the table, never the
 * value as memory, so a value the runtime did not hand out -- a forged
 * integer, a record literal, a handle of another kind, a released one -- is
 * answered NULL and the caller answers it as it answers a null handle.
 *
 * Two ways to name an entry:
 *   - a TOKEN, (serial << 32) | (slot + 1) (tk_handle_issue / tk_handle_token).
 *     Every kind with a release call (process, strbuf, pdfdoc, xlsxbook,
 *     ziparchive, tomlval) hands these out instead of the address: a released
 *     object's slot is reused only under a fresh serial, so a stale token is
 *     refused even after malloc hands its address to a newer object;
 *   - the ADDRESS itself (tk_handle_adopt / tk_handle_addr), for kinds whose
 *     value must stay a real pointer and that toke code never releases: a
 *     record toke code reads fields of, a handle compiled code passes to the
 *     glue directly (std.vec), or an object C callbacks hand to toke.  An
 *     address-keyed kind must not gain a release call: a dropped address is
 *     accepted again once malloc reuses it (138.45 review).
 * Registering an address again is idempotent (the same token, for a token
 * kind).  tk_handle_drop forgets one. */
enum {
    TK_HK_PROCESS = 1, TK_HK_VEC = 2, TK_HK_STRBUF = 3, TK_HK_SECUREBUF = 4,
    TK_HK_PDFDOC = 5, TK_HK_XLSXBOOK = 6, TK_HK_ZIPARCHIVE = 7,
    TK_HK_TOMLVAL = 8, TK_HK_SVGELEM = 9, TK_HK_SVGDOC = 10,
    TK_HK_TASKSCOPE = 11, TK_HK_LLMCLIENT = 12
};
/* A token for p (0 when the table is full; the caller disposes of p). */
int64_t tk_handle_issue(int kind, void *p);
/* As tk_handle_issue, for a VIEW into another registered object `owner` (a
 * toml section of a document): dropping the owner drops its views too. */
int64_t tk_handle_issue_view(int kind, void *p, const void *owner);
/* The object a token names, or NULL. */
void   *tk_handle_token(int kind, int64_t tok);
/* Register p as a live object of `kind`; returns p, or NULL when the table
 * is full (or p is NULL). */
void   *tk_handle_adopt(int kind, void *p);
/* The owner p was adopted under, or NULL (not a view, or not registered). */
const void *tk_handle_owner(const void *p);
/* v as a pointer when it is a live object of `kind`, else NULL. */
void   *tk_handle_addr(int kind, int64_t v);
/* Forget p (any kind) and every view adopted with p as its owner.  1 when p
 * was registered. */
int     tk_handle_drop(const void *p);

#endif /* TK_RUNTIME_H */
