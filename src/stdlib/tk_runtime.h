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

extern TK_TLS int64_t tk_current_error;

/* tk_err_box — the thread's error box, at least `nbytes` and zeroed.
 *
 * OWNERSHIP RULE (127.109).  Each thread owns exactly ONE error-box buffer,
 * grown on demand and REUSED by every raise on that thread.  A box is valid
 * until the next raise on the same thread — which is exactly the validity
 * window runtime-abi.md §7.5 restriction 1 already documents for the `$err`
 * arm's binding ("the slot is a single global and any call between the `let`
 * and the `mt` overwrites it").  So this adds no restriction: it makes the
 * allocation's lifetime equal to the reference's already-documented lifetime.
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

/* Release this thread's error-box buffer.  Registered via atexit() for the
 * main thread; a worker that raises leaves one buffer behind at thread exit,
 * which is constant, not workload-proportional. */
void    tk_err_release(void);

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

#endif /* TK_RUNTIME_H */
