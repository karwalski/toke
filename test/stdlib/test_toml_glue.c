/*
 * test_toml_glue.c — regression test for story 127.67.
 *
 * The std.toml wrappers return a bare i64 and the default codegen for
 * `mt toml.x(...) {$ok:…;$err:…}` takes the $ok arm when that i64 is
 * non-zero.  That sentinel is sound for a table handle or a string pointer
 * and WRONG for a value whose valid domain includes 0:
 *
 *   toml.bool(cfg; "minify")  with  minify = false   -> 0 -> read as error
 *   toml.i64 (cfg; "retries") with  retries = 0      -> 0 -> read as error
 *
 * Callers treat "error" as "key absent" and fall back to their default, so a
 * key that says false is obeyed as true.
 *
 * The fix follows str.toint / str.tofloat (114.53/114.54): the wrapper keeps
 * @tk_current_error truthful and always returns the real value.  This test
 * pins that protocol — it is the half of the fix that lives in the stdlib.
 * (The match site consumes @tk_current_error only for the wrappers listed in
 * llvm.c's is_num_parse_wrapper(); tk_toml_bool_w and tk_toml_i64_w still
 * need adding there before the toke-level `mt` arm follows suit.)
 *
 * Separately: tk_toml_section_w used to return the PARENT table when the key
 * was missing, so `toml.section(cfg; "typo")` silently handed back cfg and
 * every subsequent lookup read top-level keys through the $ok arm.  That is
 * the same "an error is indistinguishable from a value" shape, and it is
 * pinned here too.
 *
 * Build and run: make test-stdlib-toml-glue
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Defined in src/stdlib/tk_runtime.c in a real program; provided here so the
 * unit test links without dragging the runtime in.  The wrappers declare it
 * extern and are the things under test.
 *
 * 127.101: this stand-in MUST use the same thread-local spelling as the real
 * definition.  A plain global here against toml_glue.c's thread-local extern
 * links with no diagnostic and then takes SIGBUS on the first wrapper call —
 * which is exactly how this test failed when the slot became thread-local. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Thread_local int64_t tk_current_error = 0;
#else
__thread int64_t tk_current_error = 0;
#endif

int64_t tk_toml_load_w   (int64_t src);
int64_t tk_toml_section_w(int64_t tab, int64_t key);
int64_t tk_toml_str_w    (int64_t tab, int64_t key);
int64_t tk_toml_i64_w    (int64_t tab, int64_t key);
int64_t tk_toml_bool_w   (int64_t tab, int64_t key);

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define S(x) ((int64_t)(intptr_t)(x))

/* Poison the error global so "the wrapper never touched it" is a failure and
 * not an accidental pass. */
#define POISON() (tk_current_error = 0x5eed)

static const char *DOC =
    "minify = false\n"
    "inlinecss = false\n"
    "debug = true\n"
    "retries = 0\n"
    "port = 8080\n"
    "name = \"site\"\n"
    "[server]\n"
    "host = \"localhost\"\n";

int main(void)
{
    /* ── toml.load ─────────────────────────────────────────────────── */
    POISON();
    int64_t cfg = tk_toml_load_w(S(DOC));
    ASSERT(cfg != 0,               "toml.load of a valid document returns a table");
    ASSERT(tk_current_error == 0,  "toml.load success clears tk_current_error");

    POISON();
    int64_t bad = tk_toml_load_w(S("this is = = not toml\n["));
    ASSERT(bad == 0,               "toml.load of a malformed document returns 0");
    ASSERT(tk_current_error == 1,  "toml.load failure sets tk_current_error");

    /* ── toml.bool: the reported P0 ────────────────────────────────── */
    POISON();
    int64_t mn = tk_toml_bool_w(cfg, S("minify"));
    ASSERT(mn == 0,                "toml.bool of `minify = false` returns false");
    ASSERT(tk_current_error == 0,  "toml.bool of `false` is NOT an error");

    POISON();
    int64_t ic = tk_toml_bool_w(cfg, S("inlinecss"));
    ASSERT(ic == 0,                "toml.bool of `inlinecss = false` returns false");
    ASSERT(tk_current_error == 0,  "toml.bool of a second `false` key is NOT an error");

    POISON();
    int64_t db = tk_toml_bool_w(cfg, S("debug"));
    ASSERT(db == 1,                "toml.bool of `debug = true` returns true");
    ASSERT(tk_current_error == 0,  "toml.bool of `true` is not an error");

    POISON();
    (void)tk_toml_bool_w(cfg, S("absent"));
    ASSERT(tk_current_error == 1,  "toml.bool of a missing key IS an error");

    POISON();
    (void)tk_toml_bool_w(cfg, S("port"));
    ASSERT(tk_current_error == 1,  "toml.bool of a non-boolean value IS an error");

    /* ── toml.i64: the same defect on the integer accessor ─────────── */
    POISON();
    int64_t rt = tk_toml_i64_w(cfg, S("retries"));
    ASSERT(rt == 0,                "toml.i64 of `retries = 0` returns 0");
    ASSERT(tk_current_error == 0,  "toml.i64 of the value 0 is NOT an error");

    POISON();
    int64_t pt = tk_toml_i64_w(cfg, S("port"));
    ASSERT(pt == 8080,             "toml.i64 of `port = 8080` returns 8080");
    ASSERT(tk_current_error == 0,  "toml.i64 of a non-zero value is not an error");

    POISON();
    (void)tk_toml_i64_w(cfg, S("absent"));
    ASSERT(tk_current_error == 1,  "toml.i64 of a missing key IS an error");

    /* ── toml.str: sentinel already correct, protocol now explicit ─── */
    POISON();
    int64_t nm = tk_toml_str_w(cfg, S("name"));
    ASSERT(nm != 0 && strcmp((const char *)(intptr_t)nm, "site") == 0,
                                   "toml.str returns the string value");
    ASSERT(tk_current_error == 0,  "toml.str success clears tk_current_error");

    POISON();
    ASSERT(tk_toml_str_w(cfg, S("absent")) == 0,
                                   "toml.str of a missing key returns 0");
    ASSERT(tk_current_error == 1,  "toml.str of a missing key sets tk_current_error");

    /* ── toml.section: a missing section must not resolve to the parent ── */
    POISON();
    int64_t srv = tk_toml_section_w(cfg, S("server"));
    ASSERT(srv != 0 && srv != cfg, "toml.section returns the sub-table");
    ASSERT(tk_current_error == 0,  "toml.section success clears tk_current_error");
    ASSERT(tk_toml_str_w(srv, S("host")) != 0,
                                   "the sub-table resolves its own keys");

    POISON();
    int64_t missing = tk_toml_section_w(cfg, S("nosuch"));
    ASSERT(missing != cfg,         "toml.section of a missing key does NOT return the parent table");
    ASSERT(missing == 0,           "toml.section of a missing key returns the error sentinel");
    ASSERT(tk_current_error == 1,  "toml.section of a missing key sets tk_current_error");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
