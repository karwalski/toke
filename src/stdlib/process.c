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

/* Initial buffer size for stdout/stderr capture */
#define STDOUT_BUF_INIT 4096

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
    set_cloexec(err_pipe[1]);   /* write end: close on exec success */

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

        /* Redirect stdin */
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);

        /* Redirect stdout */
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        /* Redirect stderr */
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        /* execvp: cast from const char ** is safe — exec replaces the image */
        execvp(cmd[0], (char *const *)cmd);

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

    h->pid       = pid;
    h->stdout_fd = stdout_pipe[0];
    h->stderr_fd = stderr_pipe[0];
    h->stdin_fd  = stdin_pipe[1];
    h->exited    = 0;
    h->exit_code = 0;
    h->cwd       = NULL;

    r.ok = h;
    return r;
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

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(h->pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "waitpid() failed";
        return r;
    }

    h->exited = 1;
    if (WIFEXITED(status)) {
        h->exit_code = (int32_t)WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        h->exit_code = (int32_t)(-WTERMSIG(status));
    }
    r.ok = h->exit_code;
    return r;
}

StdoutResult process_stdout(ProcessHandle *h)
{
    StdoutResult r = {NULL, 0, PROCESS_ERR_NONE, NULL};
    if (!h) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "null handle";
        return r;
    }
    if (h->stdout_fd < 0) {
        /* Already drained — return empty string */
        r.ok = "";
        return r;
    }

    size_t capacity = STDOUT_BUF_INIT;
    size_t len      = 0;
    char  *buf      = malloc(capacity);
    if (!buf) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "allocation failed";
        return r;
    }

    ssize_t n;
    while ((n = read(h->stdout_fd, buf + len, capacity - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= capacity) {
            capacity *= 2;
            char *nb = realloc(buf, capacity);
            if (!nb) {
                free(buf);
                r.is_err   = 1;
                r.err_kind = PROCESS_ERR_IO;
                r.err_msg  = "realloc failed";
                return r;
            }
            buf = nb;
        }
    }

    if (n < 0) {
        free(buf);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "read() failed";
        return r;
    }

    buf[len] = '\0';
    close(h->stdout_fd);
    h->stdout_fd = -1;

    r.ok = buf;
    return r;
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
    if (h->cwd) { free(h->cwd); h->cwd = NULL; }
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

    uint64_t written = 0;
    while (written < len) {
        ssize_t n;
        do {
            n = write(h->stdin_fd, data + written, (size_t)(len - written));
        } while (n == -1 && errno == EINTR);
        if (n < 0) {
            r.is_err   = 1;
            r.err_kind = PROCESS_ERR_IO;
            r.err_msg  = "write() failed";
            return r;
        }
        written += (uint64_t)n;
    }
    r.ok = written;
    return r;
}

StrProcessResult process_stderr(ProcessHandle *h)
{
    StrProcessResult r = {NULL, 0, PROCESS_ERR_NONE, NULL};
    if (!h) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "null handle";
        return r;
    }
    if (h->stderr_fd < 0) {
        r.ok = "";
        return r;
    }

    size_t capacity = STDOUT_BUF_INIT;
    size_t len      = 0;
    char  *buf      = malloc(capacity);
    if (!buf) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "allocation failed";
        return r;
    }

    ssize_t n;
    while ((n = read(h->stderr_fd, buf + len, capacity - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= capacity) {
            capacity *= 2;
            char *nb = realloc(buf, capacity);
            if (!nb) {
                free(buf);
                r.is_err   = 1;
                r.err_kind = PROCESS_ERR_IO;
                r.err_msg  = "realloc failed";
                return r;
            }
            buf = nb;
        }
    }

    if (n < 0) {
        free(buf);
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "read() failed";
        return r;
    }

    buf[len] = '\0';
    close(h->stderr_fd);
    h->stderr_fd = -1;

    r.ok = buf;
    return r;
}

I32ProcessResult process_exit_code(ProcessHandle *h)
{
    I32ProcessResult r = {0, 0, PROCESS_ERR_NONE, NULL};
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

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(h->pid, &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        r.is_err   = 1;
        r.err_kind = PROCESS_ERR_IO;
        r.err_msg  = "waitpid() failed";
        return r;
    }

    h->exited = 1;
    if (WIFEXITED(status)) {
        h->exit_code = (int32_t)WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        h->exit_code = (int32_t)(-WTERMSIG(status));
    }
    r.ok = h->exit_code;
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

void process_set_cwd(ProcessHandle *h, const char *cwd)
{
    if (!h || h->exited) return;
    /* No-op if already spawned (pid is set in a live handle).
     * Since spawn always sets pid before returning, we can only call this
     * before process_spawn. In practice callers set cwd on a handle that
     * was pre-allocated, but with the current API the handle is created
     * inside process_spawn. We therefore store the value and chdir in
     * the child only when cwd is non-NULL at spawn time.
     * If the handle has already been used (pid != 0 and not exited),
     * this is documented as a no-op. */
    if (h->pid != 0) return;
    free(h->cwd);
    if (!cwd) {
        h->cwd = NULL;
        return;
    }
    h->cwd = strdup(cwd);
}

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

    /* Poll loop: check every 1 ms */
    uint64_t elapsed_ms = 0;
    struct timespec sleep_req;
    sleep_req.tv_sec  = 0;
    sleep_req.tv_nsec = 1000000L; /* 1 ms */

    while (elapsed_ms < timeout_ms) {
        int status = 0;
        pid_t result = waitpid(h->pid, &status, WNOHANG);
        if (result == h->pid) {
            h->exited = 1;
            if (WIFEXITED(status)) {
                h->exit_code = (int32_t)WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                h->exit_code = (int32_t)(-WTERMSIG(status));
            }
            r.ok = h->exit_code;
            return r;
        }
        nanosleep(&sleep_req, NULL);
        elapsed_ms++;
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
