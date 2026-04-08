/*
 * test_tk_runtime.c — Unit tests for tk_runtime.c (Story 2.8.2).
 *
 * Covers: tk_runtime_init/tk_str_argv, tk_json_parse (int/bool/str/array/
 * nested), tk_json_print_* (stdout capture), tk_array_concat,
 * tk_str_concat/len/char_at, tk_overflow_trap (subprocess).
 *
 * Build and run: make test-stdlib-runtime
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "../../src/stdlib/tk_runtime.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* ── stdout capture helpers ──────────────────────────────────────────────── */

/*
 * Redirect stdout to a pipe, call fn, restore stdout, return what was printed.
 * Caller must free() the returned string.
 */
typedef void (*print_fn)(void);

static char *capture_stdout(print_fn fn)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    int saved = dup(STDOUT_FILENO);
    fflush(stdout);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    fn();
    fflush(stdout);

    dup2(saved, STDOUT_FILENO);
    close(saved);

    /* Read all output */
    char buf[256];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return strdup(buf);
}

/* ── helpers for parameterised print tests ───────────────────────────────── */

static int64_t g_i64_val;
static void do_print_i64(void)   { tk_json_print_i64(g_i64_val); }
static void do_print_bool_1(void){ tk_json_print_bool(1); }
static void do_print_bool_0(void){ tk_json_print_bool(0); }
static void do_print_f64(void)   { tk_json_print_f64(3.14); }

static const char *g_str_val;
static void do_print_str(void)   { tk_json_print_str(g_str_val); }
static void do_print_str_null(void) { tk_json_print_str(NULL); }

static int64_t g_arr_data_backing[4]; /* backing store; arr ptr = +1 */
static void do_print_arr(void) {
    /* array: len=3, data={10,20,30} */
    g_arr_data_backing[0] = 3;
    g_arr_data_backing[1] = 10;
    g_arr_data_backing[2] = 20;
    g_arr_data_backing[3] = 30;
    tk_json_print_arr(g_arr_data_backing + 1);
}

static void do_print_arr_bool(void) {
    g_arr_data_backing[0] = 3;
    g_arr_data_backing[1] = 1;
    g_arr_data_backing[2] = 0;
    g_arr_data_backing[3] = 1;
    tk_json_print_arr_bool(g_arr_data_backing + 1);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* 1. argv access */
static void test_argv(void)
{
    const char *fake_argv[] = { "prog", "hello", "world" };
    tk_runtime_init(3, (char **)fake_argv);
    ASSERT_STREQ(tk_str_argv(0), "prog",  "argv[0] == 'prog'");
    ASSERT_STREQ(tk_str_argv(1), "hello", "argv[1] == 'hello'");
    ASSERT_STREQ(tk_str_argv(2), "world", "argv[2] == 'world'");
    ASSERT_STREQ(tk_str_argv(3), "",      "argv out-of-bounds returns empty string");
    ASSERT_STREQ(tk_str_argv(-1), "",     "argv negative index returns empty string");
}

/* 2. json_parse: integers */
static void test_parse_int(void)
{
    ASSERT(tk_json_parse("42")  == 42,  "parse int 42");
    ASSERT(tk_json_parse("0")   == 0,   "parse int 0");
    ASSERT(tk_json_parse("-7")  == -7,  "parse int -7");
    ASSERT(tk_json_parse("999") == 999, "parse int 999");
}

/* 3. json_parse: booleans */
static void test_parse_bool(void)
{
    ASSERT(tk_json_parse("true")  == 1, "parse bool true == 1");
    ASSERT(tk_json_parse("false") == 0, "parse bool false == 0");
}

/* 4. json_parse: strings */
static void test_parse_string(void)
{
    const char *s = (const char *)(intptr_t)tk_json_parse("\"hello\"");
    ASSERT(s != NULL, "parse string: non-NULL");
    ASSERT_STREQ(s, "hello", "parse string: value == 'hello'");
    free((void *)s);

    const char *empty = (const char *)(intptr_t)tk_json_parse("\"\"");
    ASSERT(empty != NULL, "parse empty string: non-NULL");
    ASSERT(strlen(empty) == 0, "parse empty string: length == 0");
    free((void *)empty);
}

/* 5. json_parse: integer array */
static void test_parse_array(void)
{
    int64_t *arr = (int64_t *)(intptr_t)tk_json_parse("[1,2,3]");
    ASSERT(arr != NULL, "parse array: non-NULL");
    ASSERT(arr[-1] == 3, "parse array [1,2,3]: length == 3");
    ASSERT(arr[0] == 1,  "parse array [1,2,3]: arr[0] == 1");
    ASSERT(arr[1] == 2,  "parse array [1,2,3]: arr[1] == 2");
    ASSERT(arr[2] == 3,  "parse array [1,2,3]: arr[2] == 3");
    free(arr - 1);

    int64_t *empty = (int64_t *)(intptr_t)tk_json_parse("[]");
    ASSERT(empty != NULL, "parse empty array: non-NULL");
    ASSERT(empty[-1] == 0, "parse empty array: length == 0");
    free(empty - 1);
}

/* 6. json_parse: whitespace tolerance */
static void test_parse_whitespace(void)
{
    ASSERT(tk_json_parse("  42  ") == 42,  "parse int with leading/trailing ws");
    ASSERT(tk_json_parse("\t-5\n") == -5,  "parse int with tab/newline ws");
}

/* 7. json_print_i64 */
static void test_print_i64(void)
{
    g_i64_val = 42;
    char *out = capture_stdout(do_print_i64);
    ASSERT(out && strstr(out, "42") != NULL, "print_i64: output contains '42'");
    free(out);

    g_i64_val = -7;
    out = capture_stdout(do_print_i64);
    ASSERT(out && strstr(out, "-7") != NULL, "print_i64: output contains '-7'");
    free(out);
}

/* 8. json_print_bool */
static void test_print_bool(void)
{
    char *t = capture_stdout(do_print_bool_1);
    ASSERT(t && strstr(t, "true") != NULL, "print_bool: 1 prints 'true'");
    free(t);

    char *f = capture_stdout(do_print_bool_0);
    ASSERT(f && strstr(f, "false") != NULL, "print_bool: 0 prints 'false'");
    free(f);
}

/* 9. json_print_str */
static void test_print_str(void)
{
    g_str_val = "hi";
    char *out = capture_stdout(do_print_str);
    ASSERT(out && strstr(out, "\"hi\"") != NULL, "print_str: output is quoted");
    free(out);

    char *null_out = capture_stdout(do_print_str_null);
    ASSERT(null_out && strstr(null_out, "null") != NULL, "print_str NULL: prints 'null'");
    free(null_out);
}

/* 10. json_print_f64 */
static void test_print_f64(void)
{
    char *out = capture_stdout(do_print_f64);
    ASSERT(out && strstr(out, "3.14") != NULL, "print_f64: output contains '3.14'");
    free(out);
}

/* 11. json_print_arr */
static void test_print_arr(void)
{
    char *out = capture_stdout(do_print_arr);
    ASSERT(out && strstr(out, "[") != NULL, "print_arr: output has '['");
    ASSERT(out && strstr(out, "10") != NULL, "print_arr: output contains 10");
    ASSERT(out && strstr(out, "20") != NULL, "print_arr: output contains 20");
    ASSERT(out && strstr(out, "30") != NULL, "print_arr: output contains 30");
    free(out);
}

/* 12. json_print_arr_bool */
static void test_print_arr_bool(void)
{
    char *out = capture_stdout(do_print_arr_bool);
    ASSERT(out && strstr(out, "true")  != NULL, "print_arr_bool: contains 'true'");
    ASSERT(out && strstr(out, "false") != NULL, "print_arr_bool: contains 'false'");
    free(out);
}

/* 13. str_concat */
static void test_str_concat(void)
{
    char *s = tk_str_concat("foo", "bar");
    ASSERT_STREQ(s, "foobar", "str_concat: 'foo'+'bar' == 'foobar'");
    free(s);

    char *e = tk_str_concat("", "x");
    ASSERT_STREQ(e, "x", "str_concat: ''+'x' == 'x'");
    free(e);

    char *n = tk_str_concat(NULL, "hi");
    ASSERT_STREQ(n, "hi", "str_concat: NULL+'hi' == 'hi'");
    free(n);
}

/* 14. str_len */
static void test_str_len(void)
{
    ASSERT(tk_str_len("hello") == 5, "str_len: 'hello' == 5");
    ASSERT(tk_str_len("")      == 0, "str_len: '' == 0");
    ASSERT(tk_str_len(NULL)    == 0, "str_len: NULL == 0");
}

/* 15. str_char_at */
static void test_str_char_at(void)
{
    ASSERT(tk_str_char_at("abc", 0)  == 'a', "str_char_at: 'abc'[0] == 'a'");
    ASSERT(tk_str_char_at("abc", 2)  == 'c', "str_char_at: 'abc'[2] == 'c'");
    ASSERT(tk_str_char_at("abc", 3)  == 0,   "str_char_at: out-of-bounds == 0");
    ASSERT(tk_str_char_at("abc", -1) == 0,   "str_char_at: negative index == 0");
    ASSERT(tk_str_char_at(NULL,   0) == 0,   "str_char_at: NULL ptr == 0");
}

/* 16. array_concat */
static void test_array_concat(void)
{
    /* Build two arrays: [1,2] and [3,4,5] */
    int64_t back_a[3] = {2, 1, 2};
    int64_t back_b[4] = {3, 3, 4, 5};
    int64_t *a = back_a + 1;
    int64_t *b = back_b + 1;

    int64_t *c = tk_array_concat(a, b);
    ASSERT(c != NULL,   "array_concat: non-NULL");
    ASSERT(c[-1] == 5,  "array_concat: len == 5");
    ASSERT(c[0] == 1,   "array_concat: c[0] == 1");
    ASSERT(c[1] == 2,   "array_concat: c[1] == 2");
    ASSERT(c[2] == 3,   "array_concat: c[2] == 3");
    ASSERT(c[3] == 4,   "array_concat: c[3] == 4");
    ASSERT(c[4] == 5,   "array_concat: c[4] == 5");
    free(c - 1);

    /* Concat with NULL (treated as empty) */
    int64_t *d = tk_array_concat(NULL, b);
    ASSERT(d != NULL,   "array_concat NULL+b: non-NULL");
    ASSERT(d[-1] == 3,  "array_concat NULL+b: len == 3");
    ASSERT(d[0] == 3,   "array_concat NULL+b: d[0] == 3");
    free(d - 1);
}

/* 17. overflow_trap exits with code 1 */
static void test_overflow_trap(void)
{
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        /* child: redirect stderr so "RT002" doesn't pollute test output */
        int devnull = open("/dev/null", 1 /* O_WRONLY */);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        tk_overflow_trap(0);
        _exit(0); /* unreachable, but satisfy compiler */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int exited  = WIFEXITED(status);
    int exitcode = exited ? WEXITSTATUS(status) : -1;
    ASSERT(exited && exitcode == 1, "overflow_trap: exits with code 1");
}

/* 18. overflow_trap: op names don't crash for all valid codes */
static void test_overflow_trap_codes(void)
{
    /* We just verify it doesn't crash for valid op codes in a subprocess */
    for (int32_t op = 0; op <= 2; op++) {
        fflush(stdout); fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", 1);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
            tk_overflow_trap(op);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 1,
               "overflow_trap: all valid op codes exit with 1");
    }
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    alarm(30);

    test_argv();
    test_parse_int();
    test_parse_bool();
    test_parse_string();
    test_parse_array();
    test_parse_whitespace();
    test_print_i64();
    test_print_bool();
    test_print_str();
    test_print_f64();
    test_print_arr();
    test_print_arr_bool();
    test_str_concat();
    test_str_len();
    test_str_char_at();
    test_array_concat();
    test_overflow_trap();
    test_overflow_trap_codes();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    }
    fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
