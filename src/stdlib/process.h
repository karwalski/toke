#ifndef TK_STDLIB_PROCESS_H
#define TK_STDLIB_PROCESS_H

/*
 * process.h — C interface for the std.process standard library module.
 *
 * Type mappings:
 *   Str         = const char *    (null-terminated UTF-8)
 *   [Str]       = StrArray        (borrowed from str.h)
 *   Handle      = ProcessHandle * (opaque, heap-allocated, caller frees)
 *   i32         = int32_t
 *   bool        = int             (0 = false, 1 = true)
 *   Handle!ProcessErr = SpawnResult
 *   i32!ProcessErr    = WaitResult
 *   Str!ProcessErr    = StdoutResult
 *
 * ProcessHandle is arena-unmanaged (stdlib boundary, malloc permitted).
 * Caller must free via process_handle_free().
 *
 * Story: 2.7.1  Branch: feature/stdlib-2.7-process-env
 */

#include <stdint.h>
#include <sys/types.h>

/* ProcessErr variant enum */
typedef enum {
    PROCESS_ERR_NONE       = 0,
    PROCESS_ERR_NOT_FOUND  = 1,  /* executable not found */
    PROCESS_ERR_PERMISSION = 2,  /* permission denied */
    PROCESS_ERR_IO         = 3   /* pipe / read / wait I/O error */
} ProcessErrKind;

/* Opaque handle wrapping pid + stdout read-end fd */
typedef struct {
    pid_t   pid;
    int     stdout_fd;   /* read end of stdout pipe; -1 if already drained */
    int     stderr_fd;   /* read end of stderr pipe; -1 if already drained */
    int     stdin_fd;    /* write end of stdin pipe; -1 if closed */
    int     exited;      /* 1 once waitpid has been called */
    int32_t exit_code;
    char   *cwd;         /* working directory for spawn; NULL = inherit */
} ProcessHandle;

/* Result types */
typedef struct { ProcessHandle *ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } SpawnResult;
typedef struct { int32_t        ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } WaitResult;
typedef struct { const char    *ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } StdoutResult;

/* Additional result types for story 28.5.2 */
typedef struct { uint64_t ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } U64ProcessResult;
typedef struct { int32_t  ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } I32ProcessResult;
typedef struct { int      ok; int is_err; ProcessErrKind err_kind; const char *err_msg; } BoolProcessResult;

/* StdoutResult is also used for stderr reads */
typedef StdoutResult StrProcessResult;

/*
 * process_spawn — fork+exec cmd[0] with cmd[1..] as arguments.
 * cmd must be a NULL-terminated array of strings ([Str] in toke).
 * Returns SpawnResult.ok = heap-allocated ProcessHandle on success.
 */
SpawnResult   process_spawn(const char **cmd);

/*
 * process_wait — block until the process exits, return its exit code.
 */
WaitResult    process_wait(ProcessHandle *h);

/*
 * process_stdout — read all stdout output from the process.
 * Drains the pipe; returned string is heap-allocated.
 */
StdoutResult  process_stdout(ProcessHandle *h);

/*
 * process_kill — send SIGTERM to the process.
 * Returns 1 on success, 0 if the process could not be signalled.
 */
int           process_kill(ProcessHandle *h);

/*
 * process_handle_free — release all resources owned by the handle.
 */
void          process_handle_free(ProcessHandle *h);

/*
 * process_stdin_write — write len bytes of data to the child's stdin.
 * Returns number of bytes written, or error.
 * Story: 28.5.2
 */
U64ProcessResult   process_stdin_write(ProcessHandle *h, const char *data, uint64_t len);

/*
 * process_stderr — read all stderr output from the process.
 * Drains the pipe; returned string is heap-allocated.
 * Story: 28.5.2
 */
StrProcessResult   process_stderr(ProcessHandle *h);

/*
 * process_exit_code — wait for the process and return its exit code.
 * Caches the result on the handle.
 * Story: 28.5.2
 */
I32ProcessResult   process_exit_code(ProcessHandle *h);

/*
 * process_is_running — non-blocking check; returns 1 if still running.
 * Story: 28.5.2
 */
int                process_is_running(ProcessHandle *h);

/*
 * process_set_cwd — set working directory for spawn.
 * No-op if the process has already been spawned.
 * Story: 28.5.2
 */
void               process_set_cwd(ProcessHandle *h, const char *cwd);

/*
 * process_timeout — wait up to timeout_ms for the process to exit.
 * If the timeout expires, sends SIGKILL and waits for the process.
 * Returns the exit code (may be negative if killed by signal).
 * Story: 28.5.2
 */
I32ProcessResult   process_timeout(ProcessHandle *h, uint64_t timeout_ms);

#endif /* TK_STDLIB_PROCESS_H */
