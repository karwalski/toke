/*
 * test_file.c — Unit tests for the std.file C library (Stories 1.3.5, 28.2.1, 28.2.2).
 *
 * Build and run: make test-stdlib-file
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../src/stdlib/file.h"

/* Forward-declare mkdtemp in case the platform header does not expose it
 * under strict C99 mode.  The function is available in all supported libc
 * (macOS, Linux glibc/musl). */
#ifndef _POSIX_VERSION
char *mkdtemp(char *template);
#endif

#define TEST_PATH "/tmp/toke_test_file.txt"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* Ensure clean state */
    file_delete(TEST_PATH);

    /* file_write */
    BoolFileResult wr = file_write(TEST_PATH, "hello world");
    ASSERT(!wr.is_err && wr.ok, "write returns true");

    /* file_read */
    StrFileResult rd = file_read(TEST_PATH);
    ASSERT(!rd.is_err, "read ok after write");
    ASSERT_STREQ(rd.ok, "hello world", "read content == 'hello world'");

    /* file_exists — present */
    ASSERT(file_exists(TEST_PATH), "exists == true after write");

    /* file_append */
    BoolFileResult ap = file_append(TEST_PATH, " extra");
    ASSERT(!ap.is_err && ap.ok, "append returns true");

    /* file_read after append */
    StrFileResult rd2 = file_read(TEST_PATH);
    ASSERT(!rd2.is_err, "read ok after append");
    ASSERT_STREQ(rd2.ok, "hello world extra", "read content == 'hello world extra'");

    /* file_delete */
    BoolFileResult del = file_delete(TEST_PATH);
    ASSERT(!del.is_err && del.ok, "delete returns true");

    /* file_exists — absent */
    ASSERT(!file_exists(TEST_PATH), "exists == false after delete");

    /* file_read non-existent → FILE_ERR_NOT_FOUND */
    StrFileResult rd3 = file_read(TEST_PATH);
    ASSERT(rd3.is_err && rd3.err.kind == FILE_ERR_NOT_FOUND,
           "read missing file → FILE_ERR_NOT_FOUND");

    /* file_list /tmp → at least one entry */
    StrArrayFileResult ls = file_list("/tmp");
    ASSERT(!ls.is_err && ls.ok.len >= 1, "list /tmp returns >= 1 entry");

    /* ── 28.2.1: directory operations ──────────────────────────────────── */

    /* Use mkdtemp to create a unique temp sandbox */
    char tmpdir[] = "/tmp/toke_test_file_XXXXXX";
    char *base = mkdtemp(tmpdir);
    ASSERT(base != NULL, "mkdtemp created sandbox dir");

    /* file_is_dir / file_is_file on the sandbox itself */
    ASSERT(file_is_dir(base),  "sandbox is a directory");
    ASSERT(!file_is_file(base), "sandbox is not a regular file");

    /* file_mkdir — create a single subdirectory */
    char subdir[256];
    snprintf(subdir, sizeof(subdir), "%s/subdir", base);
    BoolFileResult md = file_mkdir(subdir);
    ASSERT(!md.is_err && md.ok, "file_mkdir creates subdir");
    ASSERT(file_is_dir(subdir), "subdir is a directory after file_mkdir");

    /* file_is_file on a directory → 0 */
    ASSERT(!file_is_file(subdir), "subdir is not a regular file");

    /* file_mkdir_p — create nested directories */
    char nested[256];
    snprintf(nested, sizeof(nested), "%s/a/b/c", base);
    BoolFileResult mdp = file_mkdir_p(nested);
    ASSERT(!mdp.is_err && mdp.ok, "file_mkdir_p creates nested dirs");
    ASSERT(file_is_dir(nested), "nested dir exists after file_mkdir_p");

    /* file_rmdir — remove empty dir */
    BoolFileResult rmd = file_rmdir(nested);
    ASSERT(!rmd.is_err && rmd.ok, "file_rmdir removes empty nested dir");
    ASSERT(!file_is_dir(nested), "nested dir gone after file_rmdir");

    /* file_rmdir_r — remove non-empty tree */
    /* Populate subdir with a file so it is non-empty */
    char subfile[256];
    snprintf(subfile, sizeof(subfile), "%s/hello.txt", subdir);
    BoolFileResult swf = file_write(subfile, "hi");
    ASSERT(!swf.is_err && swf.ok, "write file inside subdir for rmdir_r test");
    BoolFileResult rmr = file_rmdir_r(subdir);
    ASSERT(!rmr.is_err && rmr.ok, "file_rmdir_r removes non-empty tree");
    ASSERT(!file_is_dir(subdir), "subdir gone after file_rmdir_r");

    /* ── 28.2.2: copy, move, and metadata ──────────────────────────────── */

    /* file_write a source file for copy/move tests */
    char src_path[256];
    snprintf(src_path, sizeof(src_path), "%s/src.txt", base);
    BoolFileResult wsrc = file_write(src_path, "copy me");
    ASSERT(!wsrc.is_err && wsrc.ok, "wrote source file for copy test");

    /* file_size */
    U64FileResult fsz = file_size(src_path);
    ASSERT(!fsz.is_err && fsz.ok == 7, "file_size returns correct byte count");

    /* file_mtime — just verify it returns a plausible Unix timestamp (> 0) */
    U64FileResult fmt = file_mtime(src_path);
    ASSERT(!fmt.is_err && fmt.ok > 0, "file_mtime returns non-zero timestamp");

    /* file_copy */
    char dst_path[256];
    snprintf(dst_path, sizeof(dst_path), "%s/dst.txt", base);
    BoolFileResult cp = file_copy(src_path, dst_path);
    ASSERT(!cp.is_err && cp.ok, "file_copy succeeds");
    StrFileResult cprd = file_read(dst_path);
    ASSERT(!cprd.is_err, "read copied file ok");
    ASSERT_STREQ(cprd.ok, "copy me", "copied file has correct content");
    /* original still exists */
    ASSERT(file_exists(src_path), "source still exists after copy");

    /* file_move — rename within same fs */
    char mv_dst[256];
    snprintf(mv_dst, sizeof(mv_dst), "%s/moved.txt", base);
    BoolFileResult mv = file_move(src_path, mv_dst);
    ASSERT(!mv.is_err && mv.ok, "file_move succeeds");
    ASSERT(!file_exists(src_path), "source gone after move");
    ASSERT(file_exists(mv_dst),    "dest exists after move");
    StrFileResult mvrd = file_read(mv_dst);
    ASSERT(!mvrd.is_err, "read moved file ok");
    ASSERT_STREQ(mvrd.ok, "copy me", "moved file has correct content");

    /* file_size on missing file → error */
    U64FileResult fsz_miss = file_size(src_path);
    ASSERT(fsz_miss.is_err && fsz_miss.err.kind == FILE_ERR_NOT_FOUND,
           "file_size missing file → FILE_ERR_NOT_FOUND");

    /* Cleanup the whole sandbox */
    BoolFileResult cleanup = file_rmdir_r(base);
    ASSERT(!cleanup.is_err && cleanup.ok, "cleaned up sandbox with file_rmdir_r");

    /* ── 28.2.3: path utilities ─────────────────────────────────────────── */

    /* file_join */
    {
        const char *j1 = file_join("a", "b");
        ASSERT_STREQ(j1, "a/b", "file_join: 'a'+'b' == 'a/b'");
        free((void *)j1);

        const char *j2 = file_join("a/", "b");
        ASSERT_STREQ(j2, "a/b", "file_join: 'a/'+'b' == 'a/b'");
        free((void *)j2);

        const char *j3 = file_join("a", "/b");
        ASSERT_STREQ(j3, "a/b", "file_join: 'a'+'/b' == 'a/b'");
        free((void *)j3);

        const char *j4 = file_join("a/", "/b");
        ASSERT_STREQ(j4, "a/b", "file_join: 'a/'+'/b' == 'a/b'");
        free((void *)j4);

        const char *j5 = file_join("/usr/local", "bin");
        ASSERT_STREQ(j5, "/usr/local/bin", "file_join: absolute prefix");
        free((void *)j5);
    }

    /* file_basename */
    {
        const char *b1 = file_basename("/usr/local/bin");
        ASSERT_STREQ(b1, "bin", "file_basename: '/usr/local/bin' == 'bin'");
        free((void *)b1);

        const char *b2 = file_basename("foo.txt");
        ASSERT_STREQ(b2, "foo.txt", "file_basename: 'foo.txt' == 'foo.txt'");
        free((void *)b2);

        const char *b3 = file_basename("/");
        ASSERT_STREQ(b3, "/", "file_basename: '/' == '/'");
        free((void *)b3);

        const char *b4 = file_basename("");
        ASSERT_STREQ(b4, "", "file_basename: '' == ''");
        free((void *)b4);

        const char *b5 = file_basename("/foo/bar/");
        ASSERT_STREQ(b5, "bar", "file_basename: trailing slash stripped");
        free((void *)b5);
    }

    /* file_dirname */
    {
        const char *d1 = file_dirname("/usr/local/bin");
        ASSERT_STREQ(d1, "/usr/local", "file_dirname: '/usr/local/bin' == '/usr/local'");
        free((void *)d1);

        const char *d2 = file_dirname("foo");
        ASSERT_STREQ(d2, ".", "file_dirname: 'foo' == '.'");
        free((void *)d2);

        const char *d3 = file_dirname("/");
        ASSERT_STREQ(d3, "/", "file_dirname: '/' == '/'");
        free((void *)d3);

        const char *d4 = file_dirname("/foo");
        ASSERT_STREQ(d4, "/", "file_dirname: '/foo' == '/'");
        free((void *)d4);

        const char *d5 = file_dirname("a/b");
        ASSERT_STREQ(d5, "a", "file_dirname: 'a/b' == 'a'");
        free((void *)d5);
    }

    /* file_ext */
    {
        const char *e1 = file_ext("foo.txt");
        ASSERT_STREQ(e1, ".txt", "file_ext: 'foo.txt' == '.txt'");
        free((void *)e1);

        const char *e2 = file_ext("foo.tar.gz");
        ASSERT_STREQ(e2, ".gz", "file_ext: 'foo.tar.gz' == '.gz'");
        free((void *)e2);

        const char *e3 = file_ext("foo");
        ASSERT_STREQ(e3, "", "file_ext: 'foo' == ''");
        free((void *)e3);

        const char *e4 = file_ext(".hidden");
        ASSERT_STREQ(e4, ".hidden", "file_ext: '.hidden' == '.hidden'");
        free((void *)e4);

        const char *e5 = file_ext("/path/to/file.c");
        ASSERT_STREQ(e5, ".c", "file_ext: path with dirs");
        free((void *)e5);

        const char *e6 = file_ext("archive.");
        ASSERT_STREQ(e6, ".", "file_ext: trailing dot");
        free((void *)e6);
    }

    /* file_absolute */
    {
        /* /tmp always exists; realpath should resolve it. */
        const char *abs = file_absolute("/tmp");
        ASSERT(abs != NULL, "file_absolute: '/tmp' resolves");
        free((void *)abs);

        /* A non-existent path should return NULL. */
        const char *bad = file_absolute("/nonexistent_toke_path_xyz_99999");
        ASSERT(bad == NULL, "file_absolute: non-existent path returns NULL");
        /* No free — it is NULL. */
    }

    /* file_readlines — use a fresh isolated temp dir */
    {
        char rl_dir[] = "/tmp/toke_rl_XXXXXX";
        char *rl_base = mkdtemp(rl_dir);
        ASSERT(rl_base != NULL, "file_readlines: mkdtemp ok");

        if (rl_base) {
            char rl_path[256];
            snprintf(rl_path, sizeof(rl_path), "%s/lines.txt", rl_base);

            /* Normal multi-line file with trailing newline. */
            BoolFileResult wl = file_write(rl_path, "alpha\nbeta\ngamma\n");
            ASSERT(!wl.is_err && wl.ok, "file_readlines: write test file");

            StrArrayFileResult rl = file_readlines(rl_path);
            ASSERT(!rl.is_err && rl.ok.len == 3, "file_readlines: 3 lines from trailing-newline file");
            if (!rl.is_err && rl.ok.len == 3) {
                ASSERT_STREQ(rl.ok.data[0], "alpha", "file_readlines: line[0] == 'alpha'");
                ASSERT_STREQ(rl.ok.data[1], "beta",  "file_readlines: line[1] == 'beta'");
                ASSERT_STREQ(rl.ok.data[2], "gamma", "file_readlines: line[2] == 'gamma'");
            }
            for (uint64_t i = 0; i < rl.ok.len; i++) free((void *)rl.ok.data[i]);
            free((void *)rl.ok.data);

            /* File without trailing newline. */
            BoolFileResult wl2 = file_write(rl_path, "x\ny");
            ASSERT(!wl2.is_err && wl2.ok, "file_readlines: write no-trailing-newline file");

            StrArrayFileResult rl2 = file_readlines(rl_path);
            ASSERT(!rl2.is_err && rl2.ok.len == 2, "file_readlines: 2 lines without trailing newline");
            if (!rl2.is_err && rl2.ok.len == 2) {
                ASSERT_STREQ(rl2.ok.data[0], "x", "file_readlines: line[0] == 'x'");
                ASSERT_STREQ(rl2.ok.data[1], "y", "file_readlines: line[1] == 'y'");
            }
            for (uint64_t i = 0; i < rl2.ok.len; i++) free((void *)rl2.ok.data[i]);
            free((void *)rl2.ok.data);

            /* Empty file → len 0. */
            BoolFileResult wl3 = file_write(rl_path, "");
            ASSERT(!wl3.is_err && wl3.ok, "file_readlines: write empty file");

            StrArrayFileResult rl3 = file_readlines(rl_path);
            ASSERT(!rl3.is_err && rl3.ok.len == 0, "file_readlines: empty file → len 0");
            free((void *)rl3.ok.data);

            /* \r\n line endings. */
            BoolFileResult wl4 = file_write(rl_path, "one\r\ntwo\r\n");
            ASSERT(!wl4.is_err && wl4.ok, "file_readlines: write CRLF file");

            StrArrayFileResult rl4 = file_readlines(rl_path);
            ASSERT(!rl4.is_err && rl4.ok.len == 2, "file_readlines: CRLF → 2 lines");
            if (!rl4.is_err && rl4.ok.len == 2) {
                ASSERT_STREQ(rl4.ok.data[0], "one", "file_readlines: CRLF line[0] == 'one'");
                ASSERT_STREQ(rl4.ok.data[1], "two", "file_readlines: CRLF line[1] == 'two'");
            }
            for (uint64_t i = 0; i < rl4.ok.len; i++) free((void *)rl4.ok.data[i]);
            free((void *)rl4.ok.data);

            /* Error: missing file. */
            StrArrayFileResult rl5 = file_readlines("/tmp/toke_no_such_file_xyz");
            ASSERT(rl5.is_err && rl5.err.kind == FILE_ERR_NOT_FOUND,
                   "file_readlines: missing file → FILE_ERR_NOT_FOUND");

            file_rmdir_r(rl_base);
        }
    }

    /* file_glob */
    {
        char gl_dir[] = "/tmp/toke_gl_XXXXXX";
        char *gl_base = mkdtemp(gl_dir);
        ASSERT(gl_base != NULL, "file_glob: mkdtemp ok");

        if (gl_base) {
            char p1[256], p2[256], p3[256];
            snprintf(p1, sizeof(p1), "%s/a.txt", gl_base);
            snprintf(p2, sizeof(p2), "%s/b.txt", gl_base);
            snprintf(p3, sizeof(p3), "%s/c.dat", gl_base);
            file_write(p1, ""); file_write(p2, ""); file_write(p3, "");

            /* Pattern matching *.txt — should return exactly 2 entries. */
            char pat[256];
            snprintf(pat, sizeof(pat), "%s/*.txt", gl_base);
            StrArrayFileResult gr = file_glob(pat);
            ASSERT(!gr.is_err && gr.ok.len == 2, "file_glob: *.txt returns 2 matches");
            for (uint64_t i = 0; i < gr.ok.len; i++) free((void *)gr.ok.data[i]);
            free((void *)gr.ok.data);

            /* Pattern matching *.dat — should return exactly 1 entry. */
            snprintf(pat, sizeof(pat), "%s/*.dat", gl_base);
            StrArrayFileResult gr2 = file_glob(pat);
            ASSERT(!gr2.is_err && gr2.ok.len == 1, "file_glob: *.dat returns 1 match");
            if (!gr2.is_err && gr2.ok.len == 1) {
                ASSERT_STREQ(gr2.ok.data[0], p3, "file_glob: *.dat match is c.dat");
            }
            for (uint64_t i = 0; i < gr2.ok.len; i++) free((void *)gr2.ok.data[i]);
            free((void *)gr2.ok.data);

            /* Pattern with no matches → len 0, not an error. */
            snprintf(pat, sizeof(pat), "%s/*.xyz", gl_base);
            StrArrayFileResult gr3 = file_glob(pat);
            ASSERT(!gr3.is_err && gr3.ok.len == 0, "file_glob: no-match pattern → len 0");
            free((void *)gr3.ok.data);

            file_rmdir_r(gl_base);
        }
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
