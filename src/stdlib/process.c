/*
 * process.c — Implementation of the std.process standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned ProcessHandle pointers and must
 * free them via process_handle_free().
 *
 * POSIX APIs used: fork, execvp, pipe, fcntl, waitpid, read, kill, close.
 *
 * Exec-failure detection uses the "error pipe" idiom: a pipe with FD_CLOEXEC
 * on the write end is created before fork.  On successful exec the write end
 * is automatically closed (kernel closes all FD_CLOEXEC fds on exec), so the
 * parent reads 0 bytes and knows exec succeeded.  If exec fails the child
 * writes errno down the pipe before _exit(), and the parent reads it back.
 * This is race-free.
 *
 * Story: 2.7.1  Branch: feature/stdlib-2.7-process-env
 */

#include "process.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <poll.h>
#include "tk_runtime.h"   /* AMB-03: tk_path_snapshot() */

/* Initial buffer size for stdout/stderr capture */
#define STDOUT_BUF_INIT 4096

/*
 * AMB-03: exec without trusting the live, mutable PATH. If argv[0] contains a
 * '/', exec it directly (no search). Otherwise resolve it against the PATH
 * SNAPSHOT captured at startup (tk_path_snapshot), execv'ing the first
 * executable match. Replaces execvp, whose PATH lookup could be repointed by a
 * program's own env.set/dotenv between spawn calls. Returns only on failure
 * (errno set); the caller reports it down the error pipe.
 */
static void exec_no_ambient_path(const char *const *cmd)
{
    if (strchr(cmd[0], '/')) {
        execv(cmd[0], (char *const *)cmd);
        return;                                   /* failed; errno set */
    }
    const char *path = tk_path_snapshot();
    if (!path) path = "/usr/bin:/bin";            /* last-resort default */
    char *dup = strdup(path);
    if (!dup) { errno = ENOMEM; return; }
    char *save = NULL;
    for (char *dir = strtok_r(dup, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", dir[0] ? dir : ".", cmd[0])
                >= (int)sizeof full) continue;    /* too long; skip */
        if (access(full, X_OK) == 0)
            execv(full, (char *const *)cmd);      /* returns only on failure */
    }
    free(dup);
    errno = ENOENT;                               /* nothing runnable found */
}

/* Set FD_CLOEXEC on a file descriptor. Returns 0 on success, -1 on error. */
static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

SpawnResult process_spawn(const char **cmd)
{
    SpawnResult r = {NULL, 0, PROCESS_ERR_NONE, NULL};

    if (!cmd || !cmd[0]) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_NOT_FOUND;
        r.err_msg  = "empty command";
        return r;
    }

    /* Pipe for child stdout (read end kept by parent) */
    int stdout_pipe[2];
    if (pipe(stdout_pipe) != 0) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "pipe() failed for stdout";
        return r;
    }

    /* Pipe for child stderr (read end kept by parent) */
    int stderr_pipe[2];
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "pipe() failed for stderr";
        return r;
    }

    /* Pipe for child stdin (write end kept by parent) */
    int stdin_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "pipe() failed for stdin";
        return r;
    }

    /*
     * Error pipe: child writes errno here if exec fails.
     * FD_CLOEXEC on the write end means it auto-closes on successful exec.
     */
    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "pipe() failed for error channel";
        return r;
    }
    /* 121.29 (REL-04): the result was ignored.  Without FD_CLOEXEC the
     * exec'd program inherits the write end, the read below does not see
     * EOF until that program exits, and spawn blocks for its whole life. */
    if (set_cloexec(err_pipe[1]) != 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(err_pipe[0]);    close(err_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "fcntl(FD_CLOEXEC) failed for error channel";
        return r;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(err_pipe[0]);    close(err_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "fork() failed";
        return r;
    }

    if (pid == 0) {
        /* Child: set up I/O, then exec */
        close(err_pipe[0]);  /* child doesn't read from error pipe */

        /* Redirect stdin, stdout, stderr.  121.29 (REL-04): a failed dup2
         * left the child on the parent's stream while the parent read an
         * empty pipe; report it down the error pipe like a failed exec. */
        if (dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            int e = errno;
            ssize_t n;
            do { n = write(err_pipe[1], &e, sizeof e); } while (n == -1 && errno == EINTR);
            _exit(1);
        }
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        /* AMB-03: resolve against the PATH snapshot, not the ambient PATH. */
        exec_no_ambient_path((const char *const *)cmd);

        /* exec failed: send errno down the error pipe, then exit */
        int saved_errno = errno;
        {
            ssize_t n;
            do {
                n = write(err_pipe[1], &saved_errno, sizeof(saved_errno));
            } while (n == -1 && errno == EINTR);
        }
        close(err_pipe[1]);
        _exit(1);
    }

    /* Parent: close fds the child owns */
    close(stdout_pipe[1]); /* child writes stdout here */
    close(stderr_pipe[1]); /* child writes stderr here */
    close(stdin_pipe[0]);  /* child reads stdin here  */
    close(err_pipe[1]);    /* child writes errors here (also cloexec'd) */

    /*
     * Block-read from error pipe.
     * If exec succeeded the write end was closed by the kernel (FD_CLOEXEC),
     * so read() returns 0 immediately.
     * If exec failed the child writes sizeof(int) bytes of errno.
     */
    int child_errno = 0;
    ssize_t nr;
    do {
        nr = read(err_pipe[0], &child_errno, sizeof(child_errno));
    } while (nr == -1 && errno == EINTR);
    close(err_pipe[0]);

    if (nr > 0) {
        /* exec failed — reap the child and return an error */
        int status;
        waitpid(pid, &status, 0);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        close(stdin_pipe[1]);
        r.is_err = 1;
        if (child_errno == ENOENT) {
            r.err_kind = PROCESS_ERR_NOT_FOUND;
            r.err_msg  = "executable not found";
        } else if (child_errno == EACCES || child_errno == EPERM) {
            r.err_kind = PROCESS_ERR_PERMISSION;
            r.err_msg  = "permission denied";
        } else {
            r.err_kind = PROCESS_ERR_IO;
            r.err_msg  = "exec failed";
        }
        return r;
    }

    /* exec succeeded — build the handle */
    ProcessHandle *h = malloc(sizeof(ProcessHandle));
    if (!h) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        close(stdin_pipe[1]);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "allocation failed";
        return r;
    }

    memset(h, 0, sizeof *h);
    h->pid       = pid;
    h->stdout_fd = stdout_pipe[0];
    h->stderr_fd = stderr_pipe[0];
    h->stdin_fd  = stdin_pipe[1];
    h->exited    = 0;
    h->exit_code = 0;

    r.ok = h;
    return r;
}

/* =========================================================================
 * 121.29 (REL-03): output pumping.
 *
 * The child's stdout and stderr are pipes of ~64 KiB.  Every call here that
 * waits -- for exit, for stdin to drain, for one stream's EOF -- used to
 * block on that one thing alone, so a child that filled the OTHER pipe
 * stopped, and so did the parent:
 *   - process.exec(cmd) did wait() then stdout(): any command printing more
 *     than a pipe's worth hung forever (measured: `head -c 200000
 *     /dev/zero`, both processes asleep);
 *   - stdin_write wrote everything before anything read stdout;
 *   - stdout() read to EOF while the child blocked on a full stderr.
 * Now every wait also drains whichever output pipes are open into the
 * handle, and stdout()/stderr() return what was drained plus the rest.
 * ========================================================================= */

static int buf_append(char **buf, size_t *len, size_t *cap, const char *src, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : STDOUT_BUF_INIT;
        while (*len + n + 1 > nc) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

/* Read what is available on one output fd into its buffer; close it at EOF.
 * Returns 0, or -1 on a read/alloc error. */
static int drain_fd(int *fd, char **buf, size_t *len, size_t *cap)
{
    char tmp[16384];
    ssize_t n;
    do { n = read(*fd, tmp, sizeof tmp); } while (n < 0 && errno == EINTR);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (n == 0) { close(*fd); *fd = -1; return 0; }
    return buf_append(buf, len, cap, tmp, (size_t)n);
}

/* pump -- one poll over the open output fds (and, if `in` is non-NULL, the
 * stdin fd for writability), draining output and writing as much of
 * in[*in_off..in_len) as the pipe takes.  timeout_ms as for poll().
 * Returns 0, or -1 on an I/O error (err_msg set). */
static int pump(ProcessHandle *h, int timeout_ms,
                const char *in, uint64_t in_len, uint64_t *in_off,
                const char **err_msg)
{
    struct pollfd pf[3];
    int n = 0, io = -1, ie = -1, ii = -1;
    if (h->stdout_fd >= 0 && !h->out_done) { pf[n].fd = h->stdout_fd; pf[n].events = POLLIN; io = n++; }
    if (h->stderr_fd >= 0 && !h->err_done) { pf[n].fd = h->stderr_fd; pf[n].events = POLLIN; ie = n++; }
    if (in && h->stdin_fd >= 0 && *in_off < in_len) { pf[n].fd = h->stdin_fd; pf[n].events = POLLOUT; ii = n++; }
    if (n == 0) return 0;

    int rc;
    do { rc = poll(pf, (nfds_t)n, timeout_ms); } while (rc < 0 && errno == EINTR);
    if (rc < 0) { *err_msg = "poll() failed"; return -1; }
    if (rc == 0) return 0;

    if (io >= 0 && (pf[io].revents & (POLLIN | POLLHUP | POLLERR)) &&
        drain_fd(&h->stdout_fd, &h->outbuf, &h->outlen, &h->outcap) != 0) {
        *err_msg = "read() failed"; return -1;
    }
    if (ie >= 0 && (pf[ie].revents & (POLLIN | POLLHUP | POLLERR)) &&
        drain_fd(&h->stderr_fd, &h->errbuf, &h->errlen, &h->errcap) != 0) {
        *err_msg = "read() failed"; return -1;
    }
    if (ii >= 0 && (pf[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
        /* Non-blocking for this write only: POLLOUT promises room for
         * PIPE_BUF bytes, not for the whole remainder. */
        int fl = fcntl(h->stdin_fd, F_GETFL);
        if (fl >= 0) fcntl(h->stdin_fd, F_SETFL, fl | O_NONBLOCK);
        ssize_t w;
        do {
            w = write(h->stdin_fd, in + *in_off, (size_t)(in_len - *in_off));
        } while (w < 0 && errno == EINTR);
        int e = errno;
        if (fl >= 0) fcntl(h->stdin_fd, F_SETFL, fl);
        if (w < 0 && e != EAGAIN && e != EWOULDBLOCK) { *err_msg = "write() failed"; return -1; }
        if (w > 0) *in_off += (uint64_t)w;
    }
    return 0;
}

static void record_status(ProcessHandle *h, int status)
{
    h->exited = 1;
    if (WIFEXITED(status)) {
        h->exit_code = (int32_t)WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        h->exit_code = (int32_t)(-WTERMSIG(status));
    }
}

/* wait_draining -- reap the child, draining its output meanwhile so a child
 * blocked on a full pipe can finish.  deadline_ms < 0: no deadline.
 * Returns 1 exited, 0 deadline passed, -1 error (err_msg set). */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_draining(ProcessHandle *h, int64_t timeout_ms, const char **err_msg)
{
    int64_t start = now_ms();
    for (;;) {
        int status = 0;
        pid_t w;
        int any_out = (h->stdout_fd >= 0 && !h->out_done) || (h->stderr_fd >= 0 && !h->err_done);
        if (!any_out && timeout_ms < 0) {
            do { w = waitpid(h->pid, &status, 0); } while (w == -1 && errno == EINTR);
        } else {
            do { w = waitpid(h->pid, &status, WNOHANG); } while (w == -1 && errno == EINTR);
        }
        if (w == -1) { *err_msg = "waitpid() failed"; return -1; }
        if (w == h->pid) { record_status(h, status); return 1; }

        int slice = 50;
        if (timeout_ms >= 0) {
            int64_t left = timeout_ms - (now_ms() - start);
            if (left <= 0) return 0;
            if (left < slice) slice = (int)left;
        }
        if (any_out) {
            if (pump(h, slice, NULL, 0, NULL, err_msg) != 0) return -1;
        } else {
            struct timespec ts = { 0, (long)slice * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
}

WaitResult process_wait(ProcessHandle *h)
{
    WaitResult r = {0, 0, PROCESS_ERR_NONE, NULL};
    if (!h) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "null handle";
        return r;
    }
    if (h->exited) {
        r.ok = h->exit_code;
        return r;
    }

    const char *em = NULL;
    if (wait_draining(h, -1, &em) < 0) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = em;
        return r;
    }
    r.ok = h->exit_code;
    return r;
}

/* take_stream -- pump until `fd` reaches EOF (draining the other stream
 * meanwhile), then hand over its buffer.  Shared by stdout and stderr. */
static StdoutResult take_stream(ProcessHandle *h, int is_err_stream)
{
    StdoutResult r = {NULL, 0, PROCESS_ERR_NONE, NULL};
    int   *fd   = is_err_stream ? &h->stderr_fd : &h->stdout_fd;
    int   *done = is_err_stream ? &h->err_done  : &h->out_done;
    char **buf  = is_err_stream ? &h->errbuf    : &h->outbuf;
    size_t *len = is_err_stream ? &h->errlen    : &h->outlen;
    size_t *cap = is_err_stream ? &h->errcap    : &h->outcap;

    if (*done) {
        /* Already returned -- return empty string */
        r.ok = "";
        return r;
    }
    while (*fd >= 0) {
        const char *em = NULL;
        if (pump(h, -1, NULL, 0, NULL, &em) != 0) {
            r.is_err   = 1;
            r.err_kind = PROCESS_ERR_IO;
            r.err_msg  = em;
            return r;
        }
    }
    if (!*buf && buf_append(buf, len, cap, "", 0) != 0) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "allocation failed";
        return r;
    }
    r.ok  = *buf;          /* ownership passes to the caller */
    *buf  = NULL; *len = 0; *cap = 0;
    *done = 1;
    return r;
}

StdoutResult process_stdout(ProcessHandle *h)
{
    if (!h) {
        StdoutResult r = {NULL, 1, PROCESS_ERR_IO, "null handle"};
        return r;
    }
    return take_stream(h, 0);
}

int process_kill(ProcessHandle *h)
{
    if (!h || h->exited) return 0;
    if (kill(h->pid, SIGTERM) == 0) return 1;
    return 0;
}

void process_handle_free(ProcessHandle *h)
{
    if (!h) return;
    if (h->stdout_fd >= 0) { close(h->stdout_fd); h->stdout_fd = -1; }
    if (h->stderr_fd >= 0) { close(h->stderr_fd); h->stderr_fd = -1; }
    if (h->stdin_fd  >= 0) { close(h->stdin_fd);  h->stdin_fd  = -1; }
    free(h->outbuf);
    free(h->errbuf);
    free(h);
}

/* -------------------------------------------------------------------------
 * Story 28.5.2 — new functions
 * --------------------------------------------------------------------- */

U64ProcessResult process_stdin_write(ProcessHandle *h, const char *data, uint64_t len)
{
    U64ProcessResult r = {0, 0, PROCESS_ERR_NONE, NULL};
    if (!h) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "null handle";
        return r;
    }
    if (h->stdin_fd < 0) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "stdin already closed";
        return r;
    }
    if (!data || len == 0) {
        r.ok = 0;
        return r;
    }

    /* 121.29 (REL-03): write while draining stdout/stderr, so a child that
     * answers before it has read all its input cannot fill its output pipe
     * and stop reading -- which left both processes blocked for good. */
    uint64_t written = 0;
    while (written < len) {
        const char *em = NULL;
        if (pump(h, -1, data, len, &written, &em) != 0) {
            r.is_err   = 1;
            r.err_kind = PROCESS_ERR_IO;
            r.err_msg  = em;
            return r;
        }
    }
    r.ok = written;
    return r;
}

StrProcessResult process_stderr(ProcessHandle *h)
{
    if (!h) {
        StrProcessResult r = {NULL, 1, PROCESS_ERR_IO, "null handle"};
        return r;
    }
    return take_stream(h, 1);
}

I32ProcessResult process_exit_code(ProcessHandle *h)
{
    I32ProcessResult r = {0, 0, PROCESS_ERR_NONE, NULL};
    WaitResult w = process_wait(h);
    r.ok = w.ok; r.is_err = w.is_err; r.err_kind = w.err_kind; r.err_msg = w.err_msg;
    return r;
}

int process_is_running(ProcessHandle *h)
{
    if (!h || h->exited) return 0;
    int status = 0;
    pid_t result = waitpid(h->pid, &status, WNOHANG);
    if (result == 0) {
        /* still running */
        return 1;
    }
    if (result == h->pid) {
        /* process exited */
        h->exited = 1;
        if (WIFEXITED(status)) {
            h->exit_code = (int32_t)WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            h->exit_code = (int32_t)(-WTERMSIG(status));
        }
        return 0;
    }
    /* waitpid error — treat as not running to avoid spinning */
    return 0;
}

/* process_set_cwd: removed (121.42 (7), AMB-09).  It could only store a
 * directory on a handle that had not been spawned, but process_spawn creates
 * the handle and sets its pid before returning, so it was always a no-op --
 * and spawn never read the stored value anyway.  No wrapper, no .tki entry
 * reached it.  A working-directory option belongs in spawn's own arguments,
 * gated like spawn; git history holds the code. */

I32ProcessResult process_timeout(ProcessHandle *h, uint64_t timeout_ms)
{
    I32ProcessResult r = {0, 0, PROCESS_ERR_NONE, NULL};
    if (!h) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "null handle";
        return r;
    }

    /* If already exited, return cached code immediately */
    if (h->exited) {
        r.ok = h->exit_code;
        return r;
    }

    /* 121.29 (REL-03): wait, draining output, until the deadline. */
    const char *em = NULL;
    int wr = wait_draining(h, (int64_t)(timeout_ms > INT64_MAX ? INT64_MAX : timeout_ms), &em);
    if (wr == 1) { r.ok = h->exit_code; return r; }
    if (wr < 0) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = em;
        return r;
    }

    /* Timeout expired — kill and wait */
    kill(h->pid, SIGKILL);
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(h->pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    h->exited = 1;
    if (waited == h->pid) {
        if (WIFEXITED(status)) {
            h->exit_code = (int32_t)WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            h->exit_code = (int32_t)(-WTERMSIG(status));
        }
    }
    r.ok = h->exit_code;
    return r;
}
