/*
 * os.c — std.os standard library module.
 *
 * Thin POSIX syscall bridge: each function is a minimal wrapper around
 * the corresponding POSIX call, converting between i64 and native types.
 *
 * Story: 74.2.1
 */

#include "os.h"

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
    return (int64_t)open((const char *)(intptr_t)path, (int)flags, (mode_t)mode);
}

int64_t tk_os_close(int64_t fd) {
    return (int64_t)close((int)fd);
}

int64_t tk_os_read(int64_t fd, int64_t buf, int64_t count) {
    return (int64_t)read((int)fd, (void *)(intptr_t)buf, (size_t)count);
}

int64_t tk_os_write(int64_t fd, int64_t buf, int64_t count) {
    return (int64_t)write((int)fd, (const void *)(intptr_t)buf, (size_t)count);
}

int64_t tk_os_lseek(int64_t fd, int64_t offset, int64_t whence) {
    return (int64_t)lseek((int)fd, (off_t)offset, (int)whence);
}

int64_t tk_os_stat(int64_t path) {
    struct stat st;
    int rc = stat((const char *)(intptr_t)path, &st);
    if (rc != 0) return (int64_t)-1;
    return (int64_t)st.st_size;
}

int64_t tk_os_unlink(int64_t path) {
    return (int64_t)unlink((const char *)(intptr_t)path);
}

int64_t tk_os_rename(int64_t oldpath, int64_t newpath) {
    return (int64_t)rename((const char *)(intptr_t)oldpath,
                           (const char *)(intptr_t)newpath);
}

int64_t tk_os_mkdir(int64_t path, int64_t mode) {
    return (int64_t)mkdir((const char *)(intptr_t)path, (mode_t)mode);
}

int64_t tk_os_rmdir(int64_t path) {
    return (int64_t)rmdir((const char *)(intptr_t)path);
}

int64_t tk_os_access(int64_t path, int64_t amode) {
    return (int64_t)access((const char *)(intptr_t)path, (int)amode);
}

int64_t tk_os_getcwd(void) {
    static char buf[4096];
    char *result = getcwd(buf, sizeof buf);
    return (int64_t)(intptr_t)(result ? result : "");
}

/* ── Process ─────────────────────────────────────────────────────── */

int64_t tk_os_getpid(void) {
    return (int64_t)getpid();
}

int64_t tk_os_exit(int64_t status) {
    _exit((int)status);
    return 0; /* unreachable */
}

/* ── Environment ─────────────────────────────────────────────────── */

int64_t tk_os_getenv(int64_t name) {
    const char *val = getenv((const char *)(intptr_t)name);
    return (int64_t)(intptr_t)(val ? val : "");
}

int64_t tk_os_setenv(int64_t name, int64_t value) {
    return (int64_t)setenv((const char *)(intptr_t)name,
                           (const char *)(intptr_t)value, 1);
}

/* ── Errno ───────────────────────────────────────────────────────── */

int64_t tk_os_errno(void) {
    return (int64_t)errno;
}

int64_t tk_os_strerror(int64_t errnum) {
    return (int64_t)(intptr_t)strerror((int)errnum);
}

/* ── Constants ───────────────────────────────────────────────────── */

int64_t tk_os_o_rdonly(void)  { return (int64_t)O_RDONLY; }
int64_t tk_os_o_wronly(void)  { return (int64_t)O_WRONLY; }
int64_t tk_os_o_rdwr(void)    { return (int64_t)O_RDWR; }
int64_t tk_os_o_creat(void)   { return (int64_t)O_CREAT; }
int64_t tk_os_o_trunc(void)   { return (int64_t)O_TRUNC; }
int64_t tk_os_o_append(void)  { return (int64_t)O_APPEND; }
int64_t tk_os_stdin_fd(void)  { return (int64_t)STDIN_FILENO; }
int64_t tk_os_stdout_fd(void) { return (int64_t)STDOUT_FILENO; }
int64_t tk_os_stderr_fd(void) { return (int64_t)STDERR_FILENO; }
