#ifndef TK_STDLIB_OS_H
#define TK_STDLIB_OS_H

/*
 * os.h — C interface for the std.os standard library module.
 *
 * Thin POSIX syscall bridge: file ops, process, env, errno, constants.
 * All parameters and return values are i64 or const char * (str).
 *
 * Story: 74.2.1
 */

#include <stdint.h>

/* ── File operations ─────────────────────────────────────────────── */
int64_t tk_os_open(int64_t path, int64_t flags, int64_t mode);
int64_t tk_os_close(int64_t fd);
int64_t tk_os_read(int64_t fd, int64_t buf, int64_t count);
int64_t tk_os_write(int64_t fd, int64_t buf, int64_t count);
int64_t tk_os_lseek(int64_t fd, int64_t offset, int64_t whence);
int64_t tk_os_stat(int64_t path);
int64_t tk_os_unlink(int64_t path);
int64_t tk_os_rename(int64_t oldpath, int64_t newpath);
int64_t tk_os_mkdir(int64_t path, int64_t mode);
int64_t tk_os_rmdir(int64_t path);
int64_t tk_os_access(int64_t path, int64_t amode);
int64_t tk_os_getcwd(void);

/* ── Process ─────────────────────────────────────────────────────── */
int64_t tk_os_getpid(void);
int64_t tk_os_exit(int64_t status);

/* ── Environment ─────────────────────────────────────────────────── */
int64_t tk_os_getenv(int64_t name);
int64_t tk_os_setenv(int64_t name, int64_t value);

/* ── Errno ───────────────────────────────────────────────────────── */
int64_t tk_os_errno(void);
int64_t tk_os_strerror(int64_t errnum);

/* ── Constants (exposed as zero-arg functions) ───────────────────── */
int64_t tk_os_o_rdonly(void);
int64_t tk_os_o_wronly(void);
int64_t tk_os_o_rdwr(void);
int64_t tk_os_o_creat(void);
int64_t tk_os_o_trunc(void);
int64_t tk_os_o_append(void);
int64_t tk_os_stdin_fd(void);
int64_t tk_os_stdout_fd(void);
int64_t tk_os_stderr_fd(void);

#endif /* TK_STDLIB_OS_H */
