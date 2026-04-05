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

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
