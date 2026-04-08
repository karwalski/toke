/* test_toml.c — Unit tests for the std.toml C library (Story 55.6.2). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

    /* Cleanup */
    toml_free_table(tab.ok);

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
