/* SPDX-License-Identifier: MIT */
/*
 * process_glue.c — i64-ABI wrappers for std.process module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.process.
 */

#include "process.h"
#include "capabilities.h"   /* 124.4c: process.spawn capability gate */
#include "tk_runtime.h"     /* 127.241: tk_current_error + tk_err_box (127.109) */
#include "tk_strhdr.h"  /* 127.295: sealed strings, tk_strlen */
#include "tk_exitfail.h" /* 127.529: tk_exit_report before _exit */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* ── 138.36 (loke T-67): process handles are registry tokens ──────────────
 *
 * A toke program holds a process `handle` as an i64.  Until 138.36 that i64
 * was the ProcessHandle pointer itself and every wrapper below cast it back
 * and read through it, so any other integer -- process.wait(5), a helper
 * parameter declared `h:i64`, `x as i64`, arithmetic on a real handle, the
 * pid process.spawndetached returns -- was dereferenced and the program died
 * with SIGSEGV.  The checker refuses the routes it can see (138.35); this
 * covers every route, including the ones it cannot.
 *
 * What a toke program receives from process.spawn is a TOKEN from the
 * runtime's handle registry (tk_runtime.c, kind TK_HK_PROCESS; 138.45 lifted
 * 138.36's table there so every std module shares it):
 * token = (serial << 32) | (slot + 1).  proc_lookup() reads only the
 * registry, never the value as memory.  So a value the runtime did not hand
 * out finds no live slot and is answered exactly as NULL is --
 * wait/exitcode -1, stdout/stderr $err, kill false, isalive 0,
 * hasexited/poll 1.  Every integer below 2^32 is refused (its serial half is
 * 0), and so is a real token with either half moved (h+1, h + 2^32), and a
 * token of another kind.  0 is never a token, so `$err:e 0` and
 * process.badhandle() keep their meaning.
 *
 * Lifetime.  No toke call frees a handle: process.wait caches the exit code
 * and the output drained meanwhile, and a later wait, stdout, stderr or kill
 * answers from them (docs/stdlib/process.md), so a waited handle stays live
 * and is answered as before.  If a close is ever
 * added (it needs a spec story), it calls tk_handle_drop and the slot is
 * reused only under a fresh serial, so the old token is refused even when
 * the slot holds a newer child.  Thread-safe: task workers may call
 * std.process; the registry locks. */

/* proc_register -- the token for h, or 0 when the registry cannot grow (the
 * caller then disposes of h). */
static int64_t proc_register(ProcessHandle *h)
{
    return tk_handle_issue(TK_HK_PROCESS, h);
}

/* proc_lookup -- the ProcessHandle a token names, or NULL for anything the
 * runtime did not hand out (0, a forged integer, a pid, a moved token).
 * Never dereferences v. */
static ProcessHandle *proc_lookup(int64_t v)
{
    return (ProcessHandle *)tk_handle_token(TK_HK_PROCESS, v);
}

/* 138.19: the first token of a command string, as spawn_cmdstr splits it
 * (whitespace-separated, "..." kept whole) -- the executable a
 * process.exec / process.spawndetached would run, for the scoped gate. */
static const char *cmdstr_first(const char *cmdstr, char *out, size_t outsz) {
    if (!cmdstr || !outsz) return NULL;
    const char *p = cmdstr;
    size_t n = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return NULL;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < outsz) out[n++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n + 1 < outsz) out[n++] = *p++;
    }
    out[n] = '\0';
    return out;
}

/* 124.3 (ADR-0011): run a single command STRING as argv WITHOUT a shell.
 * Used by the convenience wrappers (exec/spawndetached) which pass one command
 * string, not a [str] argv array. The string is tokenized on whitespace (with
 * "..." kept intact) and execvp'd directly — shell metacharacters (; | & $() `
 * > <) are NOT interpreted, so a command built from untrusted data cannot inject
 * a second command. Callers needing pipelines/redirection must compose processes
 * explicitly (or build an argv array and use process.spawn). Replaces the prior
 * `sh -c cmdstr` path, which was a shell-injection sink. */
static int64_t spawn_cmdstr(const char *cmdstr) {
    if (!cmdstr) return 0;
    size_t len = tk_strlen(cmdstr);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return 0;
    const char **argv = (const char **)malloc((len / 2 + 2) * sizeof(char *));
    if (!argv) { free(buf); return 0; }
    int argc = 0;
    size_t bi = 0;
    const char *p = cmdstr;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;   /* skip whitespace */
        if (!*p) break;
        argv[argc++] = buf + bi;
        if (*p == '"') {                    /* quoted token: keep spaces intact */
            p++;
            while (*p && *p != '"') buf[bi++] = *p++;
            if (*p == '"') p++;
        } else {                            /* bare token */
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') buf[bi++] = *p++;
        }
        buf[bi++] = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) { free(buf); free(argv); return 0; }
    SpawnResult r = process_spawn(argv);
    free(buf);
    free(argv);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

/* 127.241: the error slot (runtime-abi.md §7.2).  process.spawn, stdout and
 * stderr are `T!ProcessErr` and returned a bare 0 on failure, so the $err arm
 * bound nil.  A failure now stores a {tag, msg} box of ProcessErr's sum
 * layout — tag = ProcessErrKind - 1 ($notfound 0, $permission 1, $io 2;
 * process.h) — and a success clears the slot.  process.c's messages are all
 * string literals, so the box never dangles. */
static int64_t proc_raise(ProcessErrKind kind, const char *msg) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    switch (kind) {
        case PROCESS_ERR_NOT_FOUND:  box[0] = 0; break;
        case PROCESS_ERR_PERMISSION: box[0] = 1; break;
        default:                     box[0] = 2; break;   /* $io */
    }
    box[1] = (int64_t)(intptr_t)(msg && *msg ? msg : "process: I/O error");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

static int64_t proc_ok(int64_t v) {
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return v;
}

/* process.spawn(cmd:[str]) — honours the .tki [str] argv contract: the toke
 * array pointer has block[-1] == element count and block[i] == char* (as i64),
 * matching the runtime array layout (cf. tk_str_split_w in str_glue.c).
 * We decode it into a real NULL-terminated argv[] and execvp directly. */
int64_t tk_process_spawn_w(int64_t cmd) {
    /* 138.19: the executable that would run, checked before PATH lookup by
     * the broker's own copy of it (process_spawn searches the same start-up
     * PATH).  Arguments are not scoped (design note, threat model). */
    const int64_t *block = (const int64_t *)(intptr_t)cmd;
    TK_REQUIRE_CMD((cmd && block[-1] > 0) ? (const char *)(intptr_t)block[0] : NULL);
    if (!cmd) return proc_raise(PROCESS_ERR_IO, "process.spawn: null command array");
    int64_t n = block[-1];
    if (n <= 0) return proc_raise(PROCESS_ERR_IO, "process.spawn: empty command");
    const char **argv = (const char **)malloc((size_t)(n + 1) * sizeof(char *));
    if (!argv) return proc_raise(PROCESS_ERR_IO, "process.spawn: allocation failed");
    for (int64_t i = 0; i < n; i++)
        argv[i] = (const char *)(intptr_t)block[i];
    argv[n] = NULL;
    SpawnResult r = process_spawn(argv);
    free(argv);
    if (r.is_err) return proc_raise(r.err_kind, r.err_msg);
    /* 138.36: hand out a registry token, not the pointer. */
    int64_t tok = proc_register(r.ok);
    if (!tok) {
        /* No slot: the program never sees this child, so reap it here. */
        kill(r.ok->pid, SIGKILL);
        waitpid(r.ok->pid, NULL, 0);
        process_handle_free(r.ok);
        return proc_raise(PROCESS_ERR_IO, "process.spawn: too many live process handles");
    }
    return proc_ok(tok);
}
int64_t tk_process_isalive_w(int64_t pid) {
    ProcessHandle *h = proc_lookup(pid);   /* 138.36 */
    if (!h) return 0;
    return (int64_t)process_is_running(h);
}
int64_t tk_process_exit_w(int64_t code) {
    if (tk_exit_report) tk_exit_report();   /* 127.529: TK_MEM_STATS */
    _exit((int)code);
    return 0;
}
int64_t tk_process_stdout_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return proc_raise(PROCESS_ERR_IO, "process.stdout: null handle");
    StdoutResult r = process_stdout(h);
    if (r.is_err) return proc_raise(r.err_kind, r.err_msg);
    return proc_ok((int64_t)(intptr_t)r.ok);
}
int64_t tk_process_stderr_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return proc_raise(PROCESS_ERR_IO, "process.stderr: null handle");
    StrProcessResult r = process_stderr(h);
    if (r.is_err) return proc_raise(r.err_kind, r.err_msg);
    return proc_ok((int64_t)(intptr_t)r.ok);
}
int64_t tk_process_kill_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return 0;
    return (int64_t)process_kill(h);
}
int64_t tk_process_badhandle_w(void) {
    return 0; /* NULL handle is the sentinel for "bad handle" */
}
int64_t tk_process_wait_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return -1;
    WaitResult r = process_wait(h);
    if (r.is_err) return -1;
    return (int64_t)r.ok;
}
int64_t tk_process_exitcode_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return -1;
    I32ProcessResult r = process_exit_code(h);
    if (r.is_err) return -1;
    return (int64_t)r.ok;
}
int64_t tk_process_hasexited_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return 1;
    /* If is_running returns 0, the process has exited */
    return process_is_running(h) ? 0 : 1;
}
int64_t tk_process_poll_w(int64_t p) {
    ProcessHandle *h = proc_lookup(p);   /* 138.36 */
    if (!h) return 1;
    /* Non-blocking check: returns 1 if exited, 0 if still running */
    return process_is_running(h) ? 0 : 1;
}
int64_t tk_process_print_w(int64_t v) {
    if (!v) {
        printf("(null)");
    } else {
        const char *s = (const char *)(intptr_t)v;
        printf("%s", s);
    }
    return 0;
}
int64_t tk_process_sleep_w(int64_t ms) {
    if (ms <= 0) return 0;
    usleep((useconds_t)(ms * 1000));
    return 0;
}

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* process.env(name) — get environment variable value */
int64_t tk_process_env_w(int64_t name) {
    if (!name) return 0;
    const char *val = getenv((const char *)(intptr_t)name);
    if (!val) return 0;
    return (int64_t)(intptr_t)tks_strdup(val);  /* 127.295: sealed */
}

/* process.exec(cmd) — run command and return stdout as string */
int64_t tk_process_exec_w(int64_t cmd) {
    char first[4096];
    TK_REQUIRE_CMD(cmdstr_first((const char *)(intptr_t)cmd, first, sizeof first));
    if (!cmd) return 0;
    int64_t handle = spawn_cmdstr((const char *)(intptr_t)cmd);
    if (!handle) return 0;
    ProcessHandle *h = (ProcessHandle *)(intptr_t)handle;
    /* 121.29 (REL-03): exec gives the command no input -- close its stdin so
     * a command that reads it (cat, sort, a filter) sees EOF instead of
     * waiting forever; process_wait now drains stdout/stderr while it waits,
     * so output larger than a pipe no longer deadlocks the pair. */
    if (h->stdin_fd >= 0) { close(h->stdin_fd); h->stdin_fd = -1; }
    process_wait(h);
    StdoutResult r = process_stdout(h);
    if (r.is_err) { process_handle_free(h); return 0; }
    int64_t result = (int64_t)(intptr_t)r.ok;
    process_handle_free(h);
    return result;
}

/* process.homedir() — return user's home directory */
int64_t tk_process_homedir_w(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    return (int64_t)(intptr_t)tks_strdup(home);  /* 127.295: sealed */
}

/* process.readlines(cmd) — run command and return stdout as array of lines */
int64_t tk_process_readlines_w(int64_t cmd) {
    int64_t output = tk_process_exec_w(cmd);
    if (!output) return 0;
    /* Split on newlines */
    extern int64_t tk_str_split_w(int64_t, int64_t);
    return tk_str_split_w(output, (int64_t)(intptr_t)"\n");
}

/* process.spawndetached(cmd) — spawn a background process, don't track handle */
int64_t tk_process_spawndetached_w(int64_t cmd) {
    char first[4096];
    TK_REQUIRE_CMD(cmdstr_first((const char *)(intptr_t)cmd, first, sizeof first));
    if (!cmd) return 0;
    int64_t handle = spawn_cmdstr((const char *)(intptr_t)cmd);
    /* Return pid as integer, don't wait */
    if (!handle) return 0;
    ProcessHandle *h = (ProcessHandle *)(intptr_t)handle;
    int64_t pid = (int64_t)h->pid;
    /* Intentionally don't free — detached process lives independently */
    return pid;
}

/* std.homedir() — alias exposed at top level */
int64_t tk_std_homedir_w(void) {
    return tk_process_homedir_w();
}
