/* test_path.c — Unit tests for the std.path C library (Story 55.1).
 *
 * 127.295: every path_* result is a sealed string (runtime-abi §3.1) and is
 * never freed -- free() of one aborts -- so the tests do not free them. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/path.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* path_join */
    {
        const char *j1 = path_join("a", "b");
        ASSERT_STREQ(j1, "a/b", "path_join: 'a'+'b' == 'a/b'");

        const char *j2 = path_join("a/", "b");
        ASSERT_STREQ(j2, "a/b", "path_join: 'a/'+'b' trailing slash stripped");

        const char *j3 = path_join("a", "/b");
        ASSERT_STREQ(j3, "a/b", "path_join: 'a'+'/b' leading slash stripped from b");

        const char *j4 = path_join("a/", "/b");
        ASSERT_STREQ(j4, "a/b", "path_join: 'a/'+'/b' both slashes stripped");

        const char *j5 = path_join("/usr/local", "bin");
        ASSERT_STREQ(j5, "/usr/local/bin", "path_join: absolute prefix");

        const char *j6 = path_join("", "b");
        ASSERT(j6 != NULL, "path_join: empty first arg returns non-NULL");
    }

    /* path_ext */
    {
        const char *e1 = path_ext("foo.txt");
        ASSERT_STREQ(e1, ".txt", "path_ext: 'foo.txt' == '.txt'");

        const char *e2 = path_ext("foo.tar.gz");
        ASSERT_STREQ(e2, ".gz", "path_ext: 'foo.tar.gz' == '.gz' (last ext)");

        const char *e3 = path_ext("foo");
        ASSERT_STREQ(e3, "", "path_ext: 'foo' == '' (no extension)");

        const char *e4 = path_ext(".hidden");
        ASSERT_STREQ(e4, ".hidden", "path_ext: '.hidden' == '.hidden' (pure dotfile)");

        const char *e5 = path_ext("/path/to/file.c");
        ASSERT_STREQ(e5, ".c", "path_ext: path with directories");

        const char *e6 = path_ext("archive.");
        ASSERT_STREQ(e6, ".", "path_ext: 'archive.' == '.' (trailing dot)");
    }

    /* path_stem */
    {
        const char *s1 = path_stem("foo.txt");
        ASSERT_STREQ(s1, "foo", "path_stem: 'foo.txt' == 'foo'");

        const char *s2 = path_stem("foo.tar.gz");
        ASSERT_STREQ(s2, "foo.tar", "path_stem: 'foo.tar.gz' == 'foo.tar'");

        const char *s3 = path_stem("foo");
        ASSERT_STREQ(s3, "foo", "path_stem: 'foo' == 'foo' (no ext)");

        const char *s4 = path_stem(".hidden");
        ASSERT_STREQ(s4, "", "path_stem: '.hidden' == '' (dotfile → empty stem)");

        const char *s5 = path_stem("dir/file.tk");
        ASSERT_STREQ(s5, "file", "path_stem: 'dir/file.tk' == 'file'");
    }

    /* path_dir */
    {
        const char *d1 = path_dir("foo/bar.tk");
        ASSERT_STREQ(d1, "foo", "path_dir: 'foo/bar.tk' == 'foo'");

        const char *d2 = path_dir("bar.tk");
        ASSERT_STREQ(d2, ".", "path_dir: 'bar.tk' == '.'");

        const char *d3 = path_dir("/");
        ASSERT_STREQ(d3, "/", "path_dir: '/' == '/'");

        const char *d4 = path_dir("/foo");
        ASSERT_STREQ(d4, "/", "path_dir: '/foo' == '/'");

        const char *d5 = path_dir("a/b/c");
        ASSERT_STREQ(d5, "a/b", "path_dir: 'a/b/c' == 'a/b'");
    }

    /* path_base */
    {
        const char *b1 = path_base("/usr/local/bin");
        ASSERT_STREQ(b1, "bin", "path_base: '/usr/local/bin' == 'bin'");

        const char *b2 = path_base("foo.txt");
        ASSERT_STREQ(b2, "foo.txt", "path_base: 'foo.txt' == 'foo.txt'");

        const char *b3 = path_base("/");
        ASSERT_STREQ(b3, "/", "path_base: '/' == '/'");

        const char *b4 = path_base("");
        ASSERT_STREQ(b4, "", "path_base: '' == ''");
    }

    /* path_isabs */
    {
        ASSERT(path_isabs("/usr/local") == 1, "path_isabs: '/usr/local' == 1");
        ASSERT(path_isabs("relative/path") == 0, "path_isabs: 'relative/path' == 0");
        ASSERT(path_isabs("") == 0, "path_isabs: '' == 0");
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}
