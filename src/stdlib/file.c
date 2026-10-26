/* SPDX-License-Identifier: MIT */
/*
 * file.c — Implementation of the std.file standard library module.
 *
 * Uses POSIX APIs only. malloc is permitted at this stdlib boundary.
 * Callers own returned pointers.
 *
 * Story: 1.3.5  Branch: feature/stdlib-file
 */

#include "file.h"
#include <stdio.h>
#include <stdarg.h>   /* 135.10: formatted last-error messages */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <glob.h>
#include <ftw.h>
#include <fcntl.h>   /* AMB-07: O_NOFOLLOW */
#include <sys/select.h>  /* 121.73: FIFO reads wait in select(2) */
#include <sys/time.h>
#include <signal.h>      /* 121.84: SIGPIPE held off around stream writes */
#include <pthread.h>     /* 121.84: pthread_sigmask (per-thread mask) */

/* Map errno to a FileErrKind after a failed POSIX call.
 *
 * 127.246 (5) / 127.252 (4): EISDIR and ELOOP get the same kinds here as in
 * file_open_kind() below (the byte calls) -- $isdir and $symlink.  Until
 * 2026-09-30 this function put EISDIR under $invalid and ELOOP (an AMB-07
 * refusal to follow a final-component symlink) under $io, so file.write on a
 * directory and file.read on a symlink named a different variant from
 * file.readbytes / file.writebytes on the same path. */
static FileErr make_err(int err_no, const char *fallback)
{
    FileErr e;
    if (err_no == EISDIR) {
        e.kind = FILE_ERR_IS_DIR;
        e.msg  = "is a directory";
    } else if (err_no == ELOOP) {
        e.kind = FILE_ERR_SYMLINK;
        e.msg  = "final path component is a symlink and std.file does not follow it (AMB-07)";
    } else if (err_no == ENOENT) {
        e.kind = FILE_ERR_NOT_FOUND;
        e.msg  = "file not found";
    } else if (err_no == EACCES || err_no == EPERM) {
        e.kind = FILE_ERR_PERMISSION;
        e.msg  = "permission denied";
    } else if (err_no == EINVAL || err_no == EEXIST || err_no == ENOTEMPTY ||
               err_no == ENOTDIR) {
        e.kind = FILE_ERR_INVALID;
        e.msg  = fallback ? fallback : "invalid operation";
    } else {
        e.kind = FILE_ERR_IO;
        e.msg  = fallback ? fallback : "I/O error";
    }
    return e;
}

/*
 * 121.77 (3): the errno of a failed open(2) with O_NOFOLLOW, refined by
 * asking the path itself.  ELOOP does not only mean "the final component is
 * a symlink": a symlink loop (or too long a chain) in a PARENT component
 * gives it too, and every std.file call reported that as the AMB-07 refusal
 * of a final-component symlink -- a message about a file that is not a
 * symlink at all.  lstat(2) decides: a final symlink keeps ELOOP ($symlink),
 * a directory becomes EISDIR ($isdir), and anything else is the parent-loop
 * case, returned as TK_ELOOP_PARENT for the callers to report as $io.
 * Every other errno passes through unchanged.
 */
#define TK_ELOOP_PARENT (-1)
static int open_errno_refine(const char *path, int err_no)
{
    if (err_no != ELOOP || !path) return err_no;
    struct stat ls;
    if (lstat(path, &ls) == 0) {
        if (S_ISLNK(ls.st_mode)) return ELOOP;
        if (S_ISDIR(ls.st_mode)) return EISDIR;
    }
    return TK_ELOOP_PARENT;
}

/* make_err for the errno of a failed open(2) of `path` (see above), plus
 * ENXIO: a non-blocking open of a FIFO for writing with no reader (121.77). */
static FileErr make_open_err(const char *path, int err_no, const char *fallback)
{
    int e = open_errno_refine(path, err_no);
    if (e == TK_ELOOP_PARENT) {
        FileErr fe;
        fe.kind = FILE_ERR_IO;
        fe.msg  = "too many levels of symbolic links in the path (a symlink loop before the final component)";
        return fe;
    }
    if (e == ENXIO) {
        FileErr fe;
        fe.kind = FILE_ERR_NOT_REGULAR;
        fe.msg  = "a FIFO with no reader: no process has it open for reading";
        return fe;
    }
    return make_err(e, fallback);
}

/*
 * AMB-07: open with O_NOFOLLOW so a symlink on the final path component is NOT
 * followed — prevents write-through-symlink and TOCTOU redirection. Behavioural
 * change: opening a symlinked path fails with ELOOP. Covers the fopen modes
 * file.c uses ("rb"/"wb"/"ab"/"w"/"a"/"r").
 *
 * 121.77 (1): opened O_NONBLOCK, then switched back to blocking.  open(2) of
 * a FIFO blocks until the other end is opened: for writing, with no reader,
 * forever -- file.write / file.append / file.copy into such a path hung the
 * program.  Non-blocking, that open fails at once with ENXIO (reported by
 * make_open_err as $notregular).  A FIFO opened for reading with no writer
 * opens at once and reads as end-of-file.  O_NONBLOCK is cleared before the
 * stream is used so stdio's writes are ordinary blocking writes: a write to a
 * FIFO whose reader is slow waits for it instead of failing with EAGAIN after
 * a short write.  On a regular file the flag changes nothing.
 */
static FILE *fopen_nofollow(const char *path, const char *mode)
{
    int flags;
    if      (mode[0] == 'r') flags = O_RDONLY;
    else if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else return fopen(path, mode);   /* unknown mode: fall back */
    int fd = open(path, flags | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (fd < 0) return NULL;          /* errno set (ELOOP on symlink, ENXIO on a readerless FIFO) */
#ifdef F_SETNOSIGPIPE
    /* 127.273: on Darwin the SIGPIPE a failed write(2) raises is sent to the
     * PROCESS, not the writing thread, so the per-thread block in
     * sigpipe_hold does not stop it: any other thread that has SIGPIPE
     * unblocked takes it and the default action ends the program (exit 141;
     * C699 case 6, intermittent while its reader thread was still alive).
     * F_SETNOSIGPIPE turns the signal off for this descriptor only; the
     * write still fails with EPIPE.  Linux has no such flag and directs the
     * signal at the writing thread, where sigpipe_hold handles it. */
    if (mode[0] != 'r') (void)fcntl(fd, F_SETNOSIGPIPE, 1);
#endif
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
        int e = errno; close(fd); errno = e; return NULL;
    }
    FILE *f = fdopen(fd, mode);
    if (!f) { int e = errno; close(fd); errno = e; }
    return f;
}

/*
 * 121.73: read a FIFO to end-of-file with NON-BLOCKING read(2) calls, waiting
 * between them in select(2) with a 100 ms bound -- the 127.254 pattern of
 * src/readall.h's tk_read_fifo_fd (the stdlib does not include driver
 * headers, so this is a copy of that loop, not a second design).  On Darwin
 * under load the kernel can lose the wakeup for a writer's close, and a
 * blocking read then sleeps for good; a non-blocking read re-asks the kernel
 * each time.  poll(2) cannot stand in for select here: on Darwin it never
 * reports a FIFO's end-of-file.  A FIFO no process has open for writing reads
 * as end-of-file at once, so file.read of it returns "" instead of hanging.
 * fd must already be O_NONBLOCK.  Returns the NUL-terminated buffer, or NULL
 * with *err filled.
 */
static char *file_read_fifo(int fd, FileErr *err)
{
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { err->kind = FILE_ERR_IO; err->msg = "allocation failed"; return NULL; }
    for (;;) {
        if (len == cap) {
            if (cap >= (size_t)TK_FILE_MAX_BYTES) {
                free(buf);
                err->kind = FILE_ERR_TOO_LARGE;
                err->msg  = "fifo holds more than the file-size limit";
                return NULL;
            }
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) { free(buf); err->kind = FILE_ERR_IO; err->msg = "allocation failed"; return NULL; }
            buf = nb; cap *= 2;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n > 0) { len += (size_t)n; continue; }
        if (n == 0) break;                          /* end of file */
        if (errno == EINTR) continue;
        if ((errno != EAGAIN && errno != EWOULDBLOCK) || fd >= FD_SETSIZE) {
            int e = errno;
            free(buf);
            *err = make_err(e, "read failed");
            return NULL;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;                        /* bound, not a poll rate */
        (void)select(fd + 1, &rd, NULL, NULL, &tv); /* then read again */
    }
    if (len > (size_t)TK_FILE_MAX_BYTES) {
        free(buf);
        err->kind = FILE_ERR_TOO_LARGE;
        err->msg  = "fifo holds more than the file-size limit";
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

StrFileResult file_read(const char *path)
{
    StrFileResult r = {NULL, 0, {0, NULL}};
    /* 121.73: opened as the byte calls' file_open_for_read opens -- with
     * O_NONBLOCK, because open(2) on a FIFO with no writer BLOCKS FOREVER
     * without it, and file.read of such a path hung the program.  O_NOFOLLOW
     * is AMB-07, as before. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { r.is_err = 1; r.err = make_open_err(path, errno, NULL); return r; }

    /* 121.65: open(2) with O_RDONLY succeeds on a directory, and on Darwin
     * the fseek/ftell/fread that follow do not fail either -- fread returns
     * 0 -- so file.read(dir) came back `$ok ""` and a template renderer
     * handed a directory path rendered an empty page with no error.  Ask the
     * descriptor that was opened (no stat/open race) and refuse with the
     * same $isdir the byte calls (file_open_for_read) already give. */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        r.is_err = 1; r.err = make_err(e, "fstat failed"); return r;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        r.is_err = 1; r.err = make_err(EISDIR, NULL); return r;
    }
    /* 121.73: a FIFO cannot be sized with fseek/ftell (it used to fail with
     * "seek failed" when a writer was there, and hang in open when none was);
     * read it to end-of-file instead. */
    if (S_ISFIFO(st.st_mode)) {
        char *fb = file_read_fifo(fd, &r.err);
        close(fd);
        if (!fb) { r.is_err = 1; return r; }
        r.ok = fb;
        return r;
    }
    /* Anything else is read with stdio in blocking mode, as before. */
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        int e = errno;
        close(fd);
        r.is_err = 1; r.err = make_err(e, NULL); return r;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        r.is_err = 1; r.err = make_err(errno, "seek failed"); return r;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        r.is_err = 1; r.err = make_err(errno, "ftell failed"); return r;
    }
    /* 135.3c: fseek, not rewind() -- rewind reports failure only through errno, which nothing read (clang unix.Errno). */
    if (fseek(f, 0, SEEK_SET) != 0) {
        int e = errno;
        fclose(f);
        r.is_err = 1; r.err = make_err(e, "seek failed"); return r;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    r.ok = buf;
    return r;
}

/*
 * 121.84: a write that reaches a pipe or the disk can fail at three places,
 * and until this story file.write / file.append looked at only the first.
 * stdio BUFFERS what fputs is given, so for anything under the buffer size
 * fputs succeeds without a write(2) at all; the bytes reach the file in
 * fflush / fclose -- and their return values were ignored.  A full disk
 * (ENOSPC, EDQUOT) or a FIFO whose reader closed early (EPIPE) was reported
 * as $ok with the data lost.
 *
 * write_err names those errnos in the message (variant $io, as for every
 * other write failure); everything else goes through make_err.
 */
static FileErr write_err(int err_no, const char *fallback)
{
    FileErr e;
    if (err_no == EPIPE) {
        e.kind = FILE_ERR_IO;
        e.msg  = "the reader closed the pipe before all the data was written (EPIPE)";
        return e;
    }
    if (err_no == ENOSPC) {
        e.kind = FILE_ERR_IO;
        e.msg  = "no space left on device (ENOSPC); the file is incomplete";
        return e;
    }
#ifdef EDQUOT
    if (err_no == EDQUOT) {
        e.kind = FILE_ERR_IO;
        e.msg  = "disk quota exceeded (EDQUOT); the file is incomplete";
        return e;
    }
#endif
    return make_err(err_no, fallback);
}

/*
 * 121.84: SIGPIPE is held off for the duration of one write, then put back.
 *
 * write(2) to a FIFO with no reader left fails with EPIPE AND raises SIGPIPE,
 * whose default action ends the process (exit 141) -- so a program could not
 * even see the $err.  The process-wide disposition is NOT changed (a program
 * that relies on SIGPIPE, or installs its own handler, keeps it): SIGPIPE is
 * BLOCKED in this thread's mask only, the write runs, and if the write raised
 * a SIGPIPE that was not already pending it is taken off the pending set with
 * sigwait(2) -- it is thread-directed (POSIX: the signal for a failed write
 * goes to the writing thread), so the one consumed is the one this write
 * caused (on Linux; Darwin directs it at the process, so fopen_nofollow sets
 * F_SETNOSIGPIPE there instead, 127.273).  A SIGPIPE already pending before
 * the guard is left alone.  Then
 * the thread's original mask is restored.  sigtimedwait would be the usual
 * tool but Darwin lacks it; sigwait returns at once because the signal is
 * known to be pending.
 */
typedef struct { sigset_t old; int active; int was_pending; } SigpipeGuard;

static void sigpipe_hold(SigpipeGuard *g)
{
    sigset_t one, pend;
    sigemptyset(&one); sigaddset(&one, SIGPIPE);
    sigemptyset(&pend);
    g->was_pending = (sigpending(&pend) == 0 && sigismember(&pend, SIGPIPE) == 1);
    g->active = (pthread_sigmask(SIG_BLOCK, &one, &g->old) == 0);
}

static void sigpipe_release(SigpipeGuard *g)
{
    if (!g->active) return;
    int e = errno;   /* callers read errno after this */
    if (!g->was_pending) {
        sigset_t one, pend;
        sigemptyset(&one); sigaddset(&one, SIGPIPE);
        sigemptyset(&pend);
        if (sigpending(&pend) == 0 && sigismember(&pend, SIGPIPE) == 1) {
            int sig;
            (void)sigwait(&one, &sig);
        }
    }
    (void)pthread_sigmask(SIG_SETMASK, &g->old, NULL);
    errno = e;
}

/* file_write / file_append: open, write, flush, close -- every step checked.
 * fclose is checked even after a successful fflush: it can still report a
 * deferred error (NFS and some FUSE filesystems report write-back failures at
 * close). */
static BoolFileResult file_put(const char *path, const char *content,
                               const char *mode, const char *fallback)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    FILE *f = fopen_nofollow(path, mode);
    if (!f) { r.is_err = 1; r.err = make_open_err(path, errno, NULL); return r; }
    SigpipeGuard g;
    sigpipe_hold(&g);
    int saved = 0;
    if (fputs(content, f) == EOF) saved = errno ? errno : EIO;
    if (!saved && fflush(f) != 0) saved = errno ? errno : EIO;
    if (fclose(f) != 0 && !saved) saved = errno ? errno : EIO;
    sigpipe_release(&g);
    if (saved) { r.is_err = 1; r.err = write_err(saved, fallback); return r; }
    r.ok = 1;
    return r;
}

BoolFileResult file_write(const char *path, const char *content)
{
    return file_put(path, content, "w", "write failed");
}

BoolFileResult file_append(const char *path, const char *content)
{
    return file_put(path, content, "a", "append failed");
}

int file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

BoolFileResult file_delete(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (unlink(path) != 0) {
        r.is_err = 1; r.err = make_err(errno, "unlink failed"); return r;
    }
    r.ok = 1;
    return r;
}

StrArrayFileResult file_list(const char *dir)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};
    DIR *d = opendir(dir);
    if (!d) { r.is_err = 1; r.err = make_err(errno, NULL); return r; }

    /* Two-pass: count then fill. */
    size_t cap = 16, count = 0;
    const char **entries = malloc(cap * sizeof(const char *));
    if (!entries) {
        closedir(d);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (count == cap) {
            cap *= 2;
            const char **tmp = realloc(entries, cap * sizeof(const char *));
            if (!tmp) { free(entries); closedir(d);
                r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r; }
            entries = tmp;
        }
        entries[count++] = strdup(ent->d_name);
    }
    closedir(d);
    r.ok.data = entries;
    r.ok.len  = (uint64_t)count;
    return r;
}

/* ── 28.2.1: directory operations ────────────────────────────────────────── */

int file_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int file_is_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

BoolFileResult file_mkdir(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (mkdir(path, 0755) != 0) {
        r.is_err = 1; r.err = make_err(errno, "mkdir failed"); return r;
    }
    r.ok = 1;
    return r;
}

/*
 * file_mkdir_p — create directory and all missing parent components.
 * Walks the path left-to-right, calling mkdir() on each prefix that does not
 * yet exist.  EEXIST on an existing component is not treated as an error.
 */
BoolFileResult file_mkdir_p(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    memcpy(tmp, path, len + 1);

    /* Walk each path separator, creating components one at a time. */
    for (size_t i = 1; i <= len; i++) {
        if (i == len || tmp[i] == '/') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                int saved_errno = errno;
                free(tmp);
                r.is_err = 1; r.err = make_err(saved_errno, "mkdir_p failed"); return r;
            }
            tmp[i] = saved;
        }
    }
    free(tmp);
    r.ok = 1;
    return r;
}

/*
 * file_rmdir_r — recursively remove a directory tree.
 * Walks entries: unlinks files, recurses into subdirectories, then rmdir().
 * Uses a path buffer built on the heap; no system("rm -rf").
 */
BoolFileResult file_rmdir_r(const char *path)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    DIR *d = opendir(path);
    if (!d) {
        r.is_err = 1; r.err = make_err(errno, "opendir failed"); return r;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Build child path: path + "/" + name */
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        char *child = malloc(plen + 1 + nlen + 1);
        if (!child) {
            closedir(d);
            r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
        }
        memcpy(child, path, plen);
        child[plen] = '/';
        memcpy(child + plen + 1, ent->d_name, nlen + 1);

        /* AMB-05: lstat (not stat) so a symlink to an external directory is
         * classified as a non-dir and removed as a link (unlink), never recursed
         * into — a recursive delete must not follow links outside the tree. */
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            BoolFileResult sub = file_rmdir_r(child);
            if (sub.is_err) { free(child); closedir(d); return sub; }
        } else {
            if (unlink(child) != 0) {
                int saved_errno = errno;
                free(child); closedir(d);
                r.is_err = 1; r.err = make_err(saved_errno, "unlink failed"); return r;
            }
        }
        free(child);
    }
    closedir(d);

    if (rmdir(path) != 0) {
        r.is_err = 1; r.err = make_err(errno, "rmdir failed"); return r;
    }
    r.ok = 1;
    return r;
}

/* ── 28.2.2: copy, move, and metadata ───────────────────────────────────── */

/*
 * file_copy — copy src to dst in 8 KB chunks.
 * dst is truncated/created.  Cross-device safe (no rename dependency).
 *
 * 121.84: file_copy_at also says WHICH path failed -- *dst_failed is set to 1
 * when the failure is on the destination (opening, writing or closing it) and
 * 0 when it is on the source -- so tk_file_copy_w names that path; it used to
 * name src in every error, including "no space left" on dst.  The writes run
 * under the SIGPIPE guard (a FIFO destination whose reader closes early is
 * an $err, not a killed process).
 */
BoolFileResult file_copy_at(const char *src, const char *dst, int *dst_failed)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    int side_dummy;
    if (!dst_failed) dst_failed = &side_dummy;
    *dst_failed = 0;
    FILE *in = fopen_nofollow(src, "rb");
    if (!in) {
        r.is_err = 1; r.err = make_open_err(src, errno, "open source failed"); return r;
    }
    FILE *out = fopen_nofollow(dst, "wb");
    if (!out) {
        int saved_errno = errno; fclose(in);
        *dst_failed = 1;
        r.is_err = 1; r.err = make_open_err(dst, saved_errno, "open dest failed");
        return r;
    }

    char buf[8192];
    size_t nread;
    int saved_errno = 0;
    SigpipeGuard g;
    sigpipe_hold(&g);
    /* 135.3c: stop at the first short read instead of calling fread() again
     * on a stream already at EOF or in error (clang unix.Stream), and check
     * fclose(out): buffered data is written THERE, so a full disk used to be
     * reported as a successful copy. */
    for (;;) {
        nread = fread(buf, 1, sizeof(buf), in);
        if (nread < sizeof(buf) && ferror(in)) {   /* errno is fread's here */
            saved_errno = errno ? errno : EIO; break;
        }
        if (nread > 0 && fwrite(buf, 1, nread, out) != nread) {
            saved_errno = errno ? errno : EIO; *dst_failed = 1; break;
        }
        if (nread < sizeof(buf)) break;
    }
    fclose(in);
    if (fclose(out) != 0 && !saved_errno) {
        saved_errno = errno ? errno : EIO; *dst_failed = 1;
    }
    sigpipe_release(&g);
    if (saved_errno) {
        r.is_err = 1;
        r.err = *dst_failed ? write_err(saved_errno, "write failed")
                            : make_err(saved_errno, "read failed");
        return r;
    }
    r.ok = 1;
    return r;
}

BoolFileResult file_copy(const char *src, const char *dst)
{
    return file_copy_at(src, dst, NULL);
}

/*
 * file_move — rename src to dst; fallback to copy+delete on EXDEV.
 */
BoolFileResult file_move(const char *src, const char *dst)
{
    BoolFileResult r = {0, 0, {0, NULL}};
    if (rename(src, dst) == 0) {
        r.ok = 1; return r;
    }
    if (errno != EXDEV) {
        r.is_err = 1; r.err = make_err(errno, "rename failed"); return r;
    }
    /* Cross-device: copy then delete source. */
    BoolFileResult cp = file_copy(src, dst);
    if (cp.is_err) return cp;
    if (unlink(src) != 0) {
        r.is_err = 1; r.err = make_err(errno, "unlink after copy failed"); return r;
    }
    r.ok = 1;
    return r;
}

/*
 * file_size lived here, beside file_mtime, as a bare stat(2) wrapper.  135.12
 * REPLACED it in place rather than adding a second one — see the 135.12
 * section at the foot of this file for the implementation and for why the
 * original could not answer the question a bounded reader asks.  There is
 * still exactly one file_size; only its body and its position moved.
 */

U64FileResult file_mtime(const char *path)
{
    U64FileResult r = {0, 0, {0, NULL}};
    struct stat st;
    if (stat(path, &st) != 0) {
        r.is_err = 1; r.err = make_err(errno, "stat failed"); return r;
    }
    r.ok = (uint64_t)st.st_mtime;
    return r;
}

/* ── 28.2.3: path utilities ─────────────────────────────────────────────── */

/*
 * file_absolute — resolve path to an absolute path via realpath(3).
 * Returns a heap-allocated string on success, NULL if realpath() fails.
 * Caller owns the returned string.
 */
const char *file_absolute(const char *path)
{
    /* realpath() with a NULL buffer allocates the result (POSIX.1-2008). */
    return realpath(path, NULL);
}

/*
 * file_realpath — 121.60: file.realpath(path):$str!$fileerr.
 *
 * file_absolute's realpath(3), with the reason kept: an absolute path with
 * every `.`, `..` and symlink resolved, or the FileErr of why not.  This is
 * the one std.file call that FOLLOWS symlinks, every component including the
 * last, because resolving them is what it is for: a caller that must keep a
 * read inside a directory resolves both and compares prefixes.  AMB-07 is
 * about opening a path, and this opens nothing.
 *
 * ELOOP here is a symlink cycle (or too deep a chain), not the O_NOFOLLOW
 * refusal make_err's message describes, so it carries its own message.
 */
StrFileResult file_realpath(const char *path)
{
    StrFileResult r = {NULL, 0, {0, NULL}};
    char *out = realpath(path, NULL);
    if (!out) {
        int e = errno;
        r.is_err = 1;
        if (e == ELOOP) {
            r.err.kind = FILE_ERR_SYMLINK;
            r.err.msg  = "too many levels of symbolic links";
        } else {
            r.err = make_err(e, strerror(e));
        }
        return r;
    }
    r.ok = out;
    return r;
}

/*
 * file_readlines — read the file at path and split into lines.
 * Lines are split on '\n'; '\r' is stripped.  A trailing newline does NOT
 * produce an extra empty entry.  Empty file → StrArray with len=0.
 * Returns StrArrayFileResult; caller owns all strings and the data array.
 */
StrArrayFileResult file_readlines(const char *path)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};

    StrFileResult fr = file_read(path);
    if (fr.is_err) {
        r.is_err = 1; r.err = fr.err; return r;
    }

    char *content = (char *)fr.ok; /* heap-allocated by file_read */
    size_t total  = strlen(content);

    /* Count lines (number of '\n' in content, adjusted for trailing newline). */
    size_t cap   = 16;
    size_t count = 0;
    const char **lines = malloc(cap * sizeof(const char *));
    if (!lines) {
        free(content);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }

    char *p   = content;
    char *end = content + total;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        char *seg_end = (nl != NULL) ? nl : end;

        /* Strip trailing '\r'. */
        size_t seg_len = (size_t)(seg_end - p);
        if (seg_len > 0 && p[seg_len - 1] == '\r') seg_len--;

        /* Skip empty trailing entry produced by a final '\n'. */
        if (nl == NULL || nl + 1 < end || seg_len > 0) {
            if (count == cap) {
                cap *= 2;
                const char **tmp = realloc(lines, cap * sizeof(const char *));
                if (!tmp) {
                    for (size_t i = 0; i < count; i++) free((void *)lines[i]);
                    free(lines); free(content);
                    r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
                }
                lines = tmp;
            }
            char *line = malloc(seg_len + 1);
            if (!line) {
                for (size_t i = 0; i < count; i++) free((void *)lines[i]);
                free(lines); free(content);
                r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
            }
            memcpy(line, p, seg_len);
            line[seg_len] = '\0';
            lines[count++] = line;
        }

        if (nl == NULL) break;
        p = nl + 1;
    }

    free(content);
    r.ok.data = lines;
    r.ok.len  = (uint64_t)count;
    return r;
}

/*
 * file_glob — expand a POSIX glob pattern and return matching paths.
 * Uses glob(3) from <glob.h>.  Returns StrArrayFileResult; caller owns all
 * strings and the data array.  No matches → ok.len == 0 (not an error).
 */
StrArrayFileResult file_glob(const char *pattern)
{
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};

    glob_t g;
    int rc = glob(pattern, 0, NULL, &g);

    if (rc == GLOB_NOMATCH) {
        /* No matches — return empty array. */
        r.ok.data = NULL;
        r.ok.len  = 0;
        return r;
    }
    if (rc != 0) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "glob failed"; return r;
    }

    size_t n = g.gl_pathc;
    const char **out = malloc(n * sizeof(const char *));
    if (!out) {
        globfree(&g);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = strdup(g.gl_pathv[i]);
        if (!out[i]) {
            for (size_t j = 0; j < i; j++) free((void *)out[j]);
            free(out); globfree(&g);
            r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
        }
    }
    globfree(&g);
    r.ok.data = out;
    r.ok.len  = (uint64_t)n;
    return r;
}

/* ── 55.3.4: recursive directory listing ─ */

/* accumulator for file_listall.  121.29 (REL-05): nftw passes no user
 * pointer, so the walk's state lives here -- and it said "thread-local" but
 * was a plain static: two threads listing at once shared data/len/cap and
 * realloc'd and freed each other's arrays.  It is thread-local now. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local
#else
static __thread
#endif
struct { const char **data; uint64_t len; uint64_t cap; const char *base_dir; size_t base_len; } _listall_acc;

static int _listall_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_F) {
        /* store path relative to base_dir, skip leading slash */
        const char *rel = fpath + _listall_acc.base_len;
        while (*rel == '/') rel++;
        if (_listall_acc.len == _listall_acc.cap) {
            _listall_acc.cap *= 2;
            const char **tmp = realloc(_listall_acc.data, _listall_acc.cap * sizeof(const char *));
            if (!tmp) return -1;
            _listall_acc.data = tmp;
        }
        char *copy = strdup(rel);
        if (!copy) return -1;      /* 121.29: a NULL entry was stored */
        _listall_acc.data[_listall_acc.len++] = copy;
    }
    return 0;
}

StrArrayFileResult file_listall(const char *dir) {
    StrArrayFileResult r = {{NULL, 0}, 0, {0, NULL}};
    _listall_acc.cap = 64;
    _listall_acc.len = 0;
    _listall_acc.base_dir = dir;
    _listall_acc.base_len = strlen(dir);
    _listall_acc.data = malloc(_listall_acc.cap * sizeof(const char *));
    if (!_listall_acc.data) {
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "allocation failed"; return r;
    }
    if (nftw(dir, _listall_cb, 64, FTW_PHYS) != 0) {
        for (uint64_t i = 0; i < _listall_acc.len; i++) free((void *)_listall_acc.data[i]);
        free(_listall_acc.data);
        r.is_err = 1; r.err.kind = FILE_ERR_IO; r.err.msg = "nftw failed"; return r;
    }
    r.ok.data = _listall_acc.data;
    r.ok.len  = _listall_acc.len;
    return r;
}

/* ══ 135.10 — binary file access ═════════════════════════════════════════
 *
 * file_read() above returns a NUL-terminated char*.  For text that is fine;
 * for a zip, a PDF, a PNG or an xlsx it silently returns the bytes before the
 * first zero, and a caller has no way to notice.  std.zip had to open its own
 * file descriptor to work around it (135.1); 135.3 and 135.4 would each have
 * invented the same workaround.
 *
 * THE RULE THESE TWO CALLS ARE BUILT AROUND: an empty result must never stand
 * for a failure.  A zero-length `@(byte)` means the file was read and is
 * empty; the error arm is a bare 0 (the compiled T!E ABI carries no payload —
 * 127.97), and file_lasterrkind() says which of the nine outcomes occurred.
 */

/* Last-error channel.  Messages are formatted (they name the path and the
 * numbers), so unlike zip's literal-only channel this needs storage; the
 * buffer is static and overwritten by the next failure, which is exactly the
 * lifetime the accessor documents. */
static char        g_file_err_msg[1024] = "";
static const char *g_file_err_kind      = "ok";

static const char *file_kind_token(FileErrKind k)
{
    switch (k) {
        case FILE_ERR_NOT_FOUND:   return "notfound";
        case FILE_ERR_PERMISSION:  return "permission";
        case FILE_ERR_IS_DIR:      return "isdir";
        case FILE_ERR_NOT_REGULAR: return "notregular";
        case FILE_ERR_SYMLINK:     return "symlink";
        case FILE_ERR_TOO_LARGE:   return "toolarge";
        case FILE_ERR_NO_MEM:      return "nomem";
        case FILE_ERR_BAD_ARG:     return "badarg";
        case FILE_ERR_INVALID:     return "invalid";
        case FILE_ERR_IO:          return "io";
    }
    return "io";
}

void file_setlasterr(FileErrKind kind, const char *msg)
{
    g_file_err_kind = file_kind_token(kind);
    snprintf(g_file_err_msg, sizeof g_file_err_msg, "%s", msg ? msg : "");
}

const char *file_lasterr(void)     { return g_file_err_msg; }
const char *file_lasterrkind(void) { return g_file_err_kind; }

static void file_clear_lasterr(void)
{
    g_file_err_kind  = "ok";
    g_file_err_msg[0] = '\0';
}

/* Record a failure and build the FileErr the result carries. */
static FileErr file_fail(FileErrKind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_file_err_msg, sizeof g_file_err_msg, fmt, ap);
    va_end(ap);
    g_file_err_kind = file_kind_token(kind);

    FileErr e;
    e.kind = kind;
    e.msg  = g_file_err_msg;
    return e;
}

/*
 * Classify an errno from open(2) on a path that is being read or written.
 * EISDIR and ELOOP are pulled out of the generic I/O bucket deliberately:
 * "it is a directory" and "it is a symlink and std.file does not follow the
 * final component (AMB-07)" are both things a caller can act on, and both
 * would otherwise arrive as an undifferentiated I/O error.
 */
static FileErrKind file_open_kind(int err_no)
{
    switch (err_no) {
        case ENOENT:  return FILE_ERR_NOT_FOUND;
        case ENOTDIR: return FILE_ERR_NOT_FOUND;
        case EACCES:  return FILE_ERR_PERMISSION;
        case EPERM:   return FILE_ERR_PERMISSION;
        case EISDIR:  return FILE_ERR_IS_DIR;
        case ELOOP:   return FILE_ERR_SYMLINK;
        default:      return FILE_ERR_IO;
    }
}

/* 121.77 (3): file_open_kind for the errno of a failed open(2) of `path`,
 * refined by open_errno_refine: ELOOP is $symlink only when the final
 * component really is a symlink.  *msg_errno receives the errno whose
 * strerror() describes the refined outcome. */
static FileErrKind file_open_kind_at(const char *path, int err_no, int *msg_errno)
{
    int e = open_errno_refine(path, err_no);
    if (e == TK_ELOOP_PARENT) { *msg_errno = ELOOP; return FILE_ERR_IO; }
    *msg_errno = e;
    if (e == ENXIO) return FILE_ERR_NOT_REGULAR;
    return file_open_kind(e);
}

/*
 * file_readbytes — read a whole file as bytes, exactly, or say why not.
 *
 * Nine distinct outcomes, every one of them reportable:
 *
 *   ok          a byte-exact copy; length 0 for an empty file (NOT an error)
 *   notfound    no such path, or a component of it is not a directory
 *   permission  EACCES/EPERM on open
 *   isdir       the path is a directory
 *   notregular  fifo, socket, device — st_size is meaningless, so reading it
 *               as "the whole file" would return a plausible wrong answer
 *   symlink     final component is a symlink; AMB-07 forbids following it
 *   toolarge    over TK_FILE_MAX_BYTES (see the note on the 8x array cost)
 *   nomem       the file fits the cap but the allocation failed
 *   io          read(2) failed, or the file changed size mid-read
 *
 * The size is taken with fstat on the SAME descriptor that is read, so there
 * is no stat/open race, and a short read is an error rather than a quietly
 * truncated buffer — which is the defect this whole story exists to remove.
 */
static int file_open_for_read(const char *path, const char *who,
                              struct stat *st, FileErr *err);

BytesFileResult file_readbytes(const char *path)
{
    BytesFileResult r = {{NULL, 0}, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.readbytes: null path");
        return r;
    }

    /* 135.12: the open-and-classify prologue is now shared with
     * file_readrange and file_size (file_open_for_read, below).  It was
     * duplicated first and shared second, which is the wrong order: the
     * O_NONBLOCK that stops a FIFO open blocking forever was found by running
     * 135.10, and a second hand-written copy would not have had it. */
    struct stat st;
    FileErr e;
    int fd = file_open_for_read(path, "file.readbytes", &st, &e);
    if (fd < 0) { r.is_err = 1; r.err = e; return r; }

    uint64_t want = (uint64_t)st.st_size;
    if (want > TK_FILE_MAX_BYTES) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_TOO_LARGE,
                          "file.readbytes: %llu bytes exceeds the %llu-byte limit: %s",
                          (unsigned long long)want,
                          (unsigned long long)TK_FILE_MAX_BYTES, path);
        return r;
    }

    /* +1 so an empty file still gets a non-NULL buffer; the length, not the
     * pointer and never a NUL, is what says how much there is. */
    uint8_t *buf = (uint8_t *)malloc((size_t)want + 1);
    if (!buf) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_NO_MEM,
                          "file.readbytes: allocation of %llu bytes failed: %s",
                          (unsigned long long)want, path);
        return r;
    }

    uint64_t got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, (size_t)(want - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            close(fd); free(buf);
            r.is_err = 1;
            r.err = file_fail(file_open_kind(saved) == FILE_ERR_PERMISSION
                                  ? FILE_ERR_PERMISSION : FILE_ERR_IO,
                              "file.readbytes: read failed after %llu of %llu bytes: %s: %s",
                              (unsigned long long)got, (unsigned long long)want,
                              strerror(saved), path);
            return r;
        }
        if (n == 0) break;           /* file shrank under us */
        got += (uint64_t)n;
    }
    close(fd);

    if (got != want) {
        free(buf);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO,
                          "file.readbytes: short read, %llu of %llu bytes "
                          "(the file changed size during the read): %s",
                          (unsigned long long)got, (unsigned long long)want, path);
        return r;
    }

    buf[want] = 0;   /* convenience for C callers only; never a terminator */
    file_clear_lasterr();
    r.ok.data = buf;
    r.ok.len  = want;
    return r;
}

/*
 * file_writebytes — write bytes exactly, or say why not.
 *
 * The write side has the same defect as the read side: file_write() takes a
 * char* and calls fputs, so it stops at the first zero.  Without this call
 * there is no way to put a decompressed zip entry (135.1 hands back an exact
 * `@(byte)`) back on disk, and a caller who tried would get a file truncated
 * at the first zero with a successful return value.
 *
 * Outcomes: ok, notfound (a parent directory is missing), permission, isdir,
 * symlink, io (including a short write — ENOSPC), badarg (null path).
 *
 * Semantics match file.write deliberately: create-or-truncate, mode 0644,
 * O_NOFOLLOW, no atomic temp-file-and-rename.  A failure partway through
 * therefore leaves a partially written file, and the message says how many
 * bytes reached the disk; the caller decides whether to delete it.
 */
BoolFileResult file_writebytes(const char *path, const uint8_t *data, uint64_t len)
{
    BoolFileResult r = {0, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.writebytes: null path");
        return r;
    }
    if (!data && len > 0) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG,
                          "file.writebytes: null buffer with length %llu",
                          (unsigned long long)len);
        return r;
    }

    /* O_NONBLOCK for the same reason as the read side: opening a FIFO for
     * writing with no reader blocks, and ENXIO is the non-blocking form of
     * that — reported as "not a regular file", which is what it means. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (fd < 0) {
        int saved = errno;
        FileErrKind k = file_open_kind_at(path, errno, &saved);
        r.is_err = 1;
        r.err = file_fail(k, "file.writebytes: %s: %s",
                          k == FILE_ERR_SYMLINK
                              ? "final path component is a symlink and std.file does not follow it (AMB-07)"
                              : (k == FILE_ERR_NOT_REGULAR
                                     ? "not a regular file (fifo, socket or device)"
                                     : strerror(saved)),
                          path);
        return r;
    }

    /* The destination must be a regular file.  Writing bytes into a device or
     * a fifo through a call named "write this file" would succeed and mean
     * something entirely different. */
    struct stat wst;
    if (fstat(fd, &wst) == 0 && !S_ISREG(wst.st_mode)) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(S_ISDIR(wst.st_mode) ? FILE_ERR_IS_DIR : FILE_ERR_NOT_REGULAR,
                          "file.writebytes: %s: %s",
                          S_ISDIR(wst.st_mode) ? "is a directory"
                                               : "not a regular file (fifo, socket or device)",
                          path);
        return r;
    }

    uint64_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, (size_t)(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            close(fd);
            r.is_err = 1;
            r.err = file_fail(FILE_ERR_IO,
                              "file.writebytes: write failed after %llu of %llu bytes "
                              "(file left partially written): %s: %s",
                              (unsigned long long)done, (unsigned long long)len,
                              strerror(saved), path);
            return r;
        }
        done += (uint64_t)n;
    }

    if (close(fd) != 0) {
        int saved = errno;
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO, "file.writebytes: close failed: %s: %s",
                          strerror(saved), path);
        return r;
    }

    file_clear_lasterr();
    r.ok = 1;
    return r;
}

/*
 * ══ 135.12 — reading a file that does not fit ═══════════════════════════
 *
 * file_readbytes() refuses anything over 64 MiB, and the refusal is correct:
 * a toke @(byte) costs 8 bytes per byte, so the cap already stands for
 * ~576 MiB resident.  The refusal is also a dead end for this epic, because
 * the inputs it exists to read — PDF statements, XLSX exports, bulk
 * government downloads — go past it.
 *
 * WHY A BOUNDED READ AND NOT A STREAM.  The two stories this unblocks parse
 * formats whose INDEX IS AT THE END and whose contents are reached by byte
 * offset: a PDF is found through the startxref pointer in its trailer, and a
 * zip (so every XLSX) through the end-of-central-directory record, which then
 * gives a local-header offset per entry.  Neither can be parsed by reading
 * forward from the start, so a sequential stream would be the wrong primitive
 * for both of the two consumers named in the story.  A bounded read is also
 * the more primitive of the two: a caller can build a sequential scan out of
 * `offset += n` in four lines of toke, and cannot build random access out of
 * a stream at all.  Shipping this does not foreclose a stream later; shipping
 * a stream would have foreclosed the parsers.
 *
 * It is also stateless — no handle, no close, no lifetime to get wrong, and
 * nothing to leak if a caller forgets.  135.1 needed a handle and so needed a
 * close and a last-error accessor with it; this needs neither.
 *
 * The price is one open()+fstat() per call, which for a parser making many
 * small reads is real.  That is a fair trade for a call with no lifetime, and
 * if it ever measures as the bottleneck the answer is a cached descriptor
 * behind the SAME signature, not a different interface.
 */

/*
 * Open a path for reading and fstat it, applying every refusal the byte calls
 * share: symlink (AMB-07), directory, non-regular, permission, missing.  On
 * success returns the fd (>= 0) and fills *st.  On failure returns -1 and
 * fills *err, tagged with `who` so the message still names the caller.
 *
 * Factored out of file_readbytes' body rather than reimplemented: the fifo
 * hang that 135.10 found by running it (open(2) on a FIFO with no writer
 * blocks forever without O_NONBLOCK) would otherwise have to be remembered
 * separately here, and it would not have been.
 */
static int file_open_for_read(const char *path, const char *who,
                              struct stat *st, FileErr *err)
{
    /* O_NONBLOCK is load-bearing, not defensive: open(2) on a FIFO with no
     * writer BLOCKS FOREVER without it, so a std.file call would hang on a
     * path this function is going to refuse anyway.  On a regular file it
     * changes nothing — reads never return EAGAIN. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        int saved = errno;
        FileErrKind k = file_open_kind_at(path, errno, &saved);
        *err = file_fail(k, "%s: %s: %s", who,
                         k == FILE_ERR_SYMLINK
                             ? "final path component is a symlink and std.file does not follow it (AMB-07)"
                             : strerror(saved),
                         path);
        return -1;
    }
    if (fstat(fd, st) != 0) {
        int saved = errno;
        close(fd);
        *err = file_fail(FILE_ERR_IO, "%s: fstat failed: %s: %s",
                         who, strerror(saved), path);
        return -1;
    }
    if (S_ISDIR(st->st_mode)) {
        close(fd);
        *err = file_fail(FILE_ERR_IS_DIR, "%s: is a directory: %s", who, path);
        return -1;
    }
    if (!S_ISREG(st->st_mode)) {
        close(fd);
        *err = file_fail(FILE_ERR_NOT_REGULAR,
                         "%s: not a regular file (fifo, socket or device): %s",
                         who, path);
        return -1;
    }
    return fd;
}

/*
 * file_size — how big is it, without reading any of it.
 *
 * The first call a bounded reader makes, because the formats this epic reads
 * are indexed from their own end.  Its outcomes are file_readbytes' outcomes
 * minus the ones only a read can hit: nomem, io-short and toolarge cannot
 * occur, because nothing is allocated and nothing is read.
 *
 * THIS REPLACES THE 28.2-ERA file_size, IT IS NOT A SECOND ONE.  A parallel
 * implementation of a file operation beside the real one is exactly what
 * 135.11 is removing and what Epic 136's wrapper-versus-core drift cost 31
 * defects, so the old body is gone rather than left to disagree with this.
 * Four things the original could not do, each of which a bounded reader needs:
 *
 *   1. It used stat(2), which FOLLOWS SYMLINKS, while every read path in this
 *      module refuses to follow the final component (AMB-07, O_NOFOLLOW).  So
 *      file.size(link) reported the target's size while file.readrange(link)
 *      refused the link — two calls disagreeing about one path, which is the
 *      drift itself and not a hypothetical.
 *   2. It collapsed every failure into one generic error and never touched
 *      the last-error slot, so file.lasterrkind() after it returned whatever
 *      some EARLIER call had left there.
 *   3. It reported st_size for a DIRECTORY and for a FIFO.  A fifo's st_size
 *      is not its content — 135.10 made that its own refusal (notregular)
 *      precisely because returning it is a plausible wrong answer, and a
 *      reader that trusted it would then read a range that does not exist.
 *   4. It stat'd a PATH rather than the descriptor that gets read, so the
 *      size a caller plans with could race the file it then opens.
 *
 * A size of ZERO is a legitimate answer.  See the note in file.h and the
 * wrapper in file_glue.c: this is the one call in the module whose ok value
 * collides with the failure sentinel, and so the one that forces the error to
 * be carried in the slot rather than in the value.
 */
U64FileResult file_size(const char *path)
{
    U64FileResult r = {0, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.size: null path");
        return r;
    }

    struct stat st;
    FileErr e;
    int fd = file_open_for_read(path, "file.size", &st, &e);
    if (fd < 0) { r.is_err = 1; r.err = e; return r; }
    close(fd);

    file_clear_lasterr();
    r.ok = (uint64_t)st.st_size;
    return r;
}

/*
 * file_readrange — the bounded read.
 *
 * Outcomes, and which of them differ from file_readbytes:
 *
 *   ok          `len` bytes from `offset`, or FEWER if the range runs past
 *               the end — a short result is an end, not a failure, and an
 *               offset at or beyond the end is a real zero-length ok result
 *   badarg      null path, or a negative offset or length
 *   toolarge    LEN over TK_FILE_MAX_WINDOW.  Note what is NOT checked: the
 *               file's own size.  That is the whole story — the window is
 *               what costs memory, and the file is not.
 *   notfound / permission / isdir / notregular / symlink / nomem
 *               exactly as file_readbytes, through the shared open above
 *   io          pread(2) failed, or it stopped short INSIDE the file
 *
 * The size is taken by fstat on the SAME descriptor that is read, so the
 * clamp is not racing an independent stat; and pread is used rather than
 * lseek+read so the offset is part of the call and there is no seek position
 * to be wrong about.
 */
BytesFileResult file_readrange(const char *path, int64_t offset, int64_t len)
{
    BytesFileResult r = {{NULL, 0}, 0, {0, NULL}};

    if (!path) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG, "file.readrange: null path");
        return r;
    }
    if (offset < 0) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG,
                          "file.readrange: negative offset %lld: %s",
                          (long long)offset, path);
        return r;
    }
    if (len < 0) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_BAD_ARG,
                          "file.readrange: negative length %lld: %s",
                          (long long)len, path);
        return r;
    }
    if ((uint64_t)len > TK_FILE_MAX_WINDOW) {
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_TOO_LARGE,
                          "file.readrange: a %llu-byte window exceeds the "
                          "%llu-byte per-call limit (the FILE may be any size; "
                          "read it in windows): %s",
                          (unsigned long long)len,
                          (unsigned long long)TK_FILE_MAX_WINDOW, path);
        return r;
    }

    struct stat st;
    FileErr e;
    int fd = file_open_for_read(path, "file.readrange", &st, &e);
    if (fd < 0) { r.is_err = 1; r.err = e; return r; }

    uint64_t size = (uint64_t)st.st_size;
    uint64_t want = 0;
    if ((uint64_t)offset < size) {
        uint64_t avail = size - (uint64_t)offset;
        want = ((uint64_t)len < avail) ? (uint64_t)len : avail;
    }
    /* offset >= size leaves want == 0: a real empty result, marked ok. */

    /* +1 so an empty window still gets a non-NULL buffer; the length, not the
     * pointer and never a NUL, is what says how much there is. */
    uint8_t *buf = (uint8_t *)malloc((size_t)want + 1);
    if (!buf) {
        close(fd);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_NO_MEM,
                          "file.readrange: allocation of %llu bytes failed: %s",
                          (unsigned long long)want, path);
        return r;
    }

    uint64_t got = 0;
    while (got < want) {
        ssize_t n = pread(fd, buf + got, (size_t)(want - got),
                          (off_t)((uint64_t)offset + got));
        if (n < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            close(fd); free(buf);
            r.is_err = 1;
            r.err = file_fail(FILE_ERR_IO,
                              "file.readrange: read failed after %llu of %llu bytes "
                              "at offset %lld: %s: %s",
                              (unsigned long long)got, (unsigned long long)want,
                              (long long)offset, strerror(saved), path);
            return r;
        }
        if (n == 0) break;   /* the file shrank under us */
        got += (uint64_t)n;
    }
    close(fd);

    if (got != want) {
        free(buf);
        r.is_err = 1;
        r.err = file_fail(FILE_ERR_IO,
                          "file.readrange: short read, %llu of %llu bytes at "
                          "offset %lld (the file changed size during the read): %s",
                          (unsigned long long)got, (unsigned long long)want,
                          (long long)offset, path);
        return r;
    }

    buf[want] = 0;   /* convenience for C callers only; never a terminator */
    file_clear_lasterr();
    r.ok.data = buf;
    r.ok.len  = want;
    return r;
}
