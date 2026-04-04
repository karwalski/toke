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

/* Store argc/argv for later access by tk_str_argv. */
void    tk_runtime_init(int argc, char **argv);

/* Return argv[index] as a C string (ptr). */
const char *tk_str_argv(int64_t index);

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
int64_t tk_str_len(const char *s);
int64_t tk_str_char_at(const char *s, int64_t idx);

/* Generic print: prints i64 by default (most benchmark tasks). */
void    tk_json_print(int64_t val);

/* Checked arithmetic overflow trap (D2=E).
 * op_code: 0=add, 1=sub, 2=mul. Prints RT002 and exits. */
void    tk_overflow_trap(int32_t op_code);

#endif /* TK_RUNTIME_H */
