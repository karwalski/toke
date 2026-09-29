/*
 * os.c — std.os standard library module.
 *
 * Thin POSIX syscall bridge: each function is a minimal wrapper around
 * the corresponding POSIX call, converting between i64 and native types.
 *
 * Story: 74.2.1
 */

#include "os.h"
#include "capabilities.h"   /* 124.4c2: fs/env capability gates */
#include "tk_exitfail.h"    /* 127.231: os.exit(0) keeps a recorded failure */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* ── File operations ─────────────────────────────────────────────── */

int64_t tk_os_open(int64_t path, int64_t flags, int64_t mode) {
    /* 124.4c2: gate by access mode — reads need FS_READ, any write/create/
     * truncate/append needs FS_WRITE (O_RDWR needs both). */
    int af = (int)flags & O_ACCMODE;
    if (af == O_RDONLY || af == O_RDWR) TK_REQUIRE(TK_CAP_FS_READ);
    if (af == O_WRONLY || af == O_RDWR ||
        ((int)flags & (O_CREAT | O_TRUNC | O_APPEND))) TK_REQUIRE(TK_CAP_FS_WRITE);
    /* AMB-07: do not follow a symlink on the final component. A caller who
     * genuinely needs to open a symlink must clear this via a future opt-out. */
    return (int64_t)open((const char *)(intptr_t)path,
                         (int)flags | O_NOFOLLOW, (mode_t)mode);
}

int64_t tk_os_close(int64_t fd) {
    return (int64_t)close((int)fd);
}

/* AMB-08: os.read(fd, count) -> str. Allocates and returns the bytes read (as a
 * caller-owned string), instead of the old (fd, buf:i64, count) form that cast a
 * caller-supplied integer straight to a buffer pointer (arbitrary OOB read/write
 * primitive). Mirrors file.read. Returns "" on error. Note: NUL-terminated, so
 * embedded-NUL binary data is a known limitation (a [byte] variant is a follow-up). */
/* 135.23: os.read is "ownership": "caller" too; its failure paths returned
 * the string literal "", which a caller that frees its result cannot free. */
static int64_t os_empty_owned(void);
int64_t tk_os_read(int64_t fd, int64_t count) {
    if (count < 0) return os_empty_owned();
    char *buf = malloc((size_t)count + 1);
    if (!buf) return os_empty_owned();
    ssize_t n = read((int)fd, buf, (size_t)count);
    if (n < 0) { free(buf); return os_empty_owned(); }
    buf[n] = '\0';
    return (int64_t)(intptr_t)buf;
}

/* AMB-08: os.write(fd, data:str) -> bytes written. Writes the string's bytes; no
 * caller-supplied pointer. */
int64_t tk_os_write(int64_t fd, int64_t data) {
    const char *s = (const char *)(intptr_t)data;
    if (!s) return 0;
    ssize_t n = write((int)fd, s, strlen(s));
    return (n < 0) ? 0 : (int64_t)n;
}

int64_t tk_os_lseek(int64_t fd, int64_t offset, int64_t whence) {
    return (int64_t)lseek((int)fd, (off_t)offset, (int)whence);
}

int64_t tk_os_stat(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    struct stat st;
    int rc = stat((const char *)(intptr_t)path, &st);
    if (rc != 0) return (int64_t)-1;
    return (int64_t)st.st_size;
}

int64_t tk_os_unlink(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    return (int64_t)unlink((const char *)(intptr_t)path);
}

int64_t tk_os_rename(int64_t oldpath, int64_t newpath) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    return (int64_t)rename((const char *)(intptr_t)oldpath,
                           (const char *)(intptr_t)newpath);
}

int64_t tk_os_mkdir(int64_t path, int64_t mode) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    return (int64_t)mkdir((const char *)(intptr_t)path, (mode_t)mode);
}

int64_t tk_os_rmdir(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_WRITE);
    return (int64_t)rmdir((const char *)(intptr_t)path);
}

int64_t tk_os_access(int64_t path, int64_t amode) {
    TK_REQUIRE(TK_CAP_FS_READ);
    return (int64_t)access((const char *)(intptr_t)path, (int)amode);
}

/* An empty caller-owned string: a caller that frees the result of a
 * "ownership": "caller" function must be able to free every result, the
 * failure results included (135.23). */
static int64_t os_empty_owned(void) {
    char *e = (char *)calloc(1, 1);
    return (int64_t)(intptr_t)(e ? e : "");   /* "" only if calloc(1,1) fails */
}

/* 135.23: os.tki declares os.getcwd "ownership": "caller", and this returned
 * `static char buf[4096]` — a caller that freed it freed a static buffer, two
 * results aliased each other, and a working directory longer than 4095 bytes
 * came back as "".  Each call now returns its own heap string, grown until
 * the path fits; "" (also heap) when getcwd fails for any other reason. */
int64_t tk_os_getcwd(void) {
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)malloc(cap);
        if (!buf) return os_empty_owned();
        if (getcwd(buf, cap)) return (int64_t)(intptr_t)buf;
        free(buf);
        if (errno != ERANGE || cap > ((size_t)1 << 20)) return os_empty_owned();
        cap *= 2;
    }
}

/* ── Process ─────────────────────────────────────────────────────── */

int64_t tk_os_getpid(void) {
    return (int64_t)getpid();
}

int64_t tk_os_exit(int64_t status) {
    /* 127.231: os.exit is _exit() and skips atexit handlers, so a recorded
     * failure (a failed test assertion, a server that did not start) would
     * be lost by os.exit(0).  tk_exitfail_atexit() returns when there is
     * none; otherwise it reports and exits 1. */
    if (status == 0) tk_exitfail_atexit();
    _exit((int)status);
    return 0; /* unreachable */
}

/* ── Environment ─────────────────────────────────────────────────── */

int64_t tk_os_getenv(int64_t name) {
    const char *val = getenv((const char *)(intptr_t)name);
    return (int64_t)(intptr_t)(val ? val : "");
}

int64_t tk_os_setenv(int64_t name, int64_t value) {
    TK_REQUIRE(TK_CAP_ENV_WRITE);
    return (int64_t)setenv((const char *)(intptr_t)name,
                           (const char *)(intptr_t)value, 1);
}

/* ── Errno ───────────────────────────────────────────────────────── */

int64_t tk_os_errno(void) {
    return (int64_t)errno;
}

/* 121.22 (AMB-11): each call returns its own heap copy.  This returned
 * strerror()'s pointer, and for an errno the C library has no text for,
 * strerror() formats "Unknown error: N" into ONE static buffer: measured on
 * main (macOS), a = os.strerror(9999); b = os.strerror(9998) left a reading
 * "Unknown error: 9998" -- the second call rewrote the first result, and two
 * threads calling it race on that buffer.  strerror_r writes into ours.
 * Its two signatures: glibc with _GNU_SOURCE returns the text (which may not
 * be the buffer); POSIX/XSI (macOS, musl, BSD) returns 0 or an error. */
int64_t tk_os_strerror(int64_t errnum) {
    char buf[256];
    const char *text = buf;
    buf[0] = '\0';
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    text = strerror_r((int)errnum, buf, sizeof buf);
    if (!text) text = "";
#else
    if (strerror_r((int)errnum, buf, sizeof buf) != 0 && buf[0] == '\0')
        snprintf(buf, sizeof buf, "Unknown error: %lld", (long long)errnum);
#endif
    char *out = strdup(text);
    return out ? (int64_t)(intptr_t)out : os_empty_owned();
}

/* ── Constants ───────────────────────────────────────────────────── */

int64_t tk_os_o_rdonly(void)  { return (int64_t)O_RDONLY; }
int64_t tk_os_o_wronly(void)  { return (int64_t)O_WRONLY; }
int64_t tk_os_o_rdwr(void)    { return (int64_t)O_RDWR; }
int64_t tk_os_o_creat(void)   { return (int64_t)O_CREAT; }
int64_t tk_os_o_trunc(void)   { return (int64_t)O_TRUNC; }
int64_t tk_os_o_append(void)  { return (int64_t)O_APPEND; }
int64_t tk_os_o_nofollow(void){ return (int64_t)O_NOFOLLOW; }
int64_t tk_os_stdin_fd(void)  { return (int64_t)STDIN_FILENO; }
int64_t tk_os_stdout_fd(void) { return (int64_t)STDOUT_FILENO; }
int64_t tk_os_stderr_fd(void) { return (int64_t)STDERR_FILENO; }

/* ── 131.38: no-underscore call-name forwarders ─────────────────────────────
 * os.tki (114.22) declares os.ordonly, os.stdinfd, ... ; llvm.c's os table
 * only maps the legacy o_rdonly/stdin_fd spellings, so the documented
 * no-underscore names fell through to the generic tk_os_<method>_w resolver
 * and were unresolved at link time.  Forwarders make both spellings link. */
int64_t tk_os_ordonly_w(void)  { return tk_os_o_rdonly(); }
int64_t tk_os_owronly_w(void)  { return tk_os_o_wronly(); }
int64_t tk_os_ordwr_w(void)    { return tk_os_o_rdwr(); }
int64_t tk_os_ocreat_w(void)   { return tk_os_o_creat(); }
int64_t tk_os_otrunc_w(void)   { return tk_os_o_trunc(); }
int64_t tk_os_oappend_w(void)  { return tk_os_o_append(); }
int64_t tk_os_stdinfd_w(void)  { return tk_os_stdin_fd(); }
int64_t tk_os_stdoutfd_w(void) { return tk_os_stdout_fd(); }
int64_t tk_os_stderrfd_w(void) { return tk_os_stderr_fd(); }
