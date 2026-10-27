/* test_toml.c — Unit tests for the std.toml C library (Story 55.6.2). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../../src/stdlib/toml.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

static const char *sample_toml =
    "[site]\n"
    "name = \"my-site\"\n"
    "url = \"https://example.com\"\n"
    "\n"
    "[server]\n"
    "port = 3000\n"
    "workers = 4\n"
    "\n"
    "[build]\n"
    "minify = true\n";

int main(void)
{
    /* toml_load — valid input */
    TomlResult tab = toml_load(sample_toml);
    ASSERT(!tab.is_err, "toml_load(sample) not is_err");
    ASSERT(tab.ok != NULL, "toml_load(sample) ok != NULL");

    /* toml_load — invalid input */
    TomlResult bad = toml_load("invalid [[[");
    ASSERT(bad.is_err, "toml_load(invalid) is_err");

    /* toml_get_section — [site] */
    TomlResult site = toml_get_section(tab.ok, "site");
    ASSERT(!site.is_err, "toml_get_section('site') not is_err");
    ASSERT(site.ok != NULL, "toml_get_section('site') ok != NULL");

    /* toml_get_str from [site] */
    TomlStrResult name = toml_get_str(site.ok, "name");
    ASSERT(!name.is_err, "toml_get_str('name') not is_err");
    ASSERT_STREQ(name.ok, "my-site", "toml_get_str('name') == 'my-site'");

    TomlStrResult url = toml_get_str(site.ok, "url");
    ASSERT(!url.is_err, "toml_get_str('url') not is_err");
    ASSERT_STREQ(url.ok, "https://example.com", "toml_get_str('url') == 'https://example.com'");

    /* toml_get_str — nonexistent key */
    TomlStrResult missing_str = toml_get_str(site.ok, "nonexistent");
    ASSERT(missing_str.is_err, "toml_get_str('nonexistent') is_err");

    /* toml_get_section — [server] */
    TomlResult server = toml_get_section(tab.ok, "server");
    ASSERT(!server.is_err, "toml_get_section('server') not is_err");
    ASSERT(server.ok != NULL, "toml_get_section('server') ok != NULL");

    /* toml_get_i64 from [server] */
    TomlI64Result port = toml_get_i64(server.ok, "port");
    ASSERT(!port.is_err, "toml_get_i64('port') not is_err");
    ASSERT(port.ok == 3000, "toml_get_i64('port') == 3000");

    TomlI64Result workers = toml_get_i64(server.ok, "workers");
    ASSERT(!workers.is_err, "toml_get_i64('workers') not is_err");
    ASSERT(workers.ok == 4, "toml_get_i64('workers') == 4");

    /* toml_get_i64 — wrong type (string key, not integer) */
    TomlI64Result wrong_type = toml_get_i64(site.ok, "name");
    ASSERT(wrong_type.is_err, "toml_get_i64('name') is_err (wrong type)");

    /* toml_get_section — [build] */
    TomlResult build = toml_get_section(tab.ok, "build");
    ASSERT(!build.is_err, "toml_get_section('build') not is_err");
    ASSERT(build.ok != NULL, "toml_get_section('build') ok != NULL");

    /* toml_get_bool from [build] */
    TomlBoolResult minify = toml_get_bool(build.ok, "minify");
    ASSERT(!minify.is_err, "toml_get_bool('minify') not is_err");
    ASSERT(minify.ok == 1, "toml_get_bool('minify') == 1 (true)");

    /* toml_get_section — nonexistent section */
    TomlResult no_section = toml_get_section(tab.ok, "nonexistent");
    ASSERT(no_section.is_err, "toml_get_section('nonexistent') is_err");

    /* toml_load_file — nonexistent file */
    TomlResult file_miss = toml_load_file("/nonexistent/path.toml");
    ASSERT(file_miss.is_err, "toml_load_file('/nonexistent/path.toml') is_err");

    /* 113.B.4: the failure says which -- absent, or present with another type */
    ASSERT_STREQ(missing_str.err_msg, "key not found", "toml_get_str absent: 'key not found'");
    ASSERT_STREQ(wrong_type.err_msg, "key is not an integer", "toml_get_i64 wrong type: 'key is not an integer'");
    ASSERT_STREQ(toml_get_str(server.ok, "port").err_msg, "key is not a string", "toml_get_str wrong type");
    ASSERT_STREQ(toml_get_bool(site.ok, "name").err_msg, "key is not a boolean", "toml_get_bool wrong type");
    ASSERT_STREQ(no_section.err_msg, "key not found", "toml_get_section absent: 'key not found'");
    ASSERT_STREQ(toml_get_section(site.ok, "name").err_msg, "key is not a table", "toml_get_section on a value");

    /* 113.B.4: keys and count (values, then arrays, then sub-tables) */
    ASSERT(toml_count(tab.ok) == 3, "toml_count(root) == 3");
    ASSERT_STREQ(toml_key_at(tab.ok, 0), "site", "toml_key_at(root, 0) == site");
    ASSERT(toml_key_at(tab.ok, 3) == NULL, "toml_key_at past the end is NULL");
    ASSERT(toml_count(NULL) == 0, "toml_count(NULL) == 0");

    /* 113.B.4: arrays */
    {
        TomlResult a = toml_load("i = [1, -2]\ns = [\"x\"]\nd = [1.5, 2]\nb = [false]\n"
                                 "e = []\nm = [1, \"x\"]\nv = 1\n[[t]]\nk = 1\n[[t]]\nk = 2\n");
        ASSERT(!a.is_err, "toml_load(arrays) ok");
        TomlArrResult r = toml_get_arr(a.ok, "i", 'i');
        ASSERT(!r.is_err && r.len == 2 && r.data[1] == -2, "toml_get_arr i64");
        free(r.data);
        r = toml_get_arr(a.ok, "s", 's');
        ASSERT(!r.is_err && r.len == 1 && strcmp((const char *)(intptr_t)r.data[0], "x") == 0, "toml_get_arr str");
        if (!r.is_err) free((void *)(intptr_t)r.data[0]);
        free(r.data);
        r = toml_get_arr(a.ok, "d", 'd');
        { double d1 = 0; if (!r.is_err) memcpy(&d1, &r.data[1], sizeof d1);
          ASSERT(!r.is_err && r.len == 2 && d1 == 2.0, "toml_get_arr f64 reads an integer element"); }
        free(r.data);
        r = toml_get_arr(a.ok, "b", 'b');
        ASSERT(!r.is_err && r.len == 1 && r.data[0] == 0, "toml_get_arr bool");
        free(r.data);
        r = toml_get_arr(a.ok, "e", 's');
        ASSERT(!r.is_err && r.len == 0, "toml_get_arr empty array is ok, len 0");
        r = toml_get_arr(a.ok, "m", 'i');
        ASSERT(r.is_err && strcmp(r.err_msg, "array element 1 is not an integer") == 0, "toml_get_arr mixed: element 1");
        r = toml_get_arr(a.ok, "v", 'i');
        ASSERT(r.is_err && strcmp(r.err_msg, "key is not an array") == 0, "toml_get_arr on a value");
        r = toml_get_arr(a.ok, "zz", 'i');
        ASSERT(r.is_err && strcmp(r.err_msg, "key not found") == 0, "toml_get_arr absent");
        r = toml_get_arr(a.ok, "t", 't');
        ASSERT(!r.is_err && r.len == 2 && toml_get_i64((void *)(intptr_t)r.data[1], "k").ok == 2, "toml_get_arr tables");
        /* 113.B.4: a view is never freed; a document once */
        ASSERT(toml_free_root((void *)(intptr_t)r.data[0]) == 0, "toml_free_root(array-of-tables element) is a no-op");
        free(r.data);
        ASSERT(toml_free_root(a.ok) == 1, "toml_free_root(document) frees it");
        ASSERT(toml_free_root(a.ok) == 0, "toml_free_root(document) twice is a no-op");
    }
    ASSERT(toml_free_root(server.ok) == 0, "toml_free_root(section) is a no-op");
    ASSERT(toml_free_root(NULL) == 0, "toml_free_root(NULL) is a no-op");

    /* Cleanup */
    toml_free_table(tab.ok);

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
