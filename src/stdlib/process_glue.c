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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

/* ── 138.36 (loke T-67): the registry of live process handles ──────────────
 *
 * A toke program holds a process `handle` as an i64.  Until 138.36 that i64
 * was the ProcessHandle pointer itself and every wrapper below cast it back
 * and read through it, so any other integer -- process.wait(5), a helper
 * parameter declared `h:i64`, `x as i64`, arithmetic on a real handle, the
 * pid process.spawndetached returns -- was dereferenced and the program died
 * with SIGSEGV.  The checker refuses the routes it can see (138.35); this
 * covers every route, including the ones it cannot.
 *
 * What a toke program receives from process.spawn is now a TOKEN, never an
 * address:  token = (serial << 32) | (slot + 1).  `slot` indexes g_proc.slots;
 * `serial` is a counter, unique per registration and never 0, kept in the
 * slot.  proc_lookup() decodes the slot, bounds-checks it and compares the
 * stored serial: it reads only the registry, never the value as memory.  So
 * a value the runtime did not hand out finds no live slot and is answered
 * exactly as NULL is -- wait/exitcode -1, stdout/stderr $err, kill false,
 * isalive 0, hasexited/poll 1.  Every integer below 2^32 is refused (its
 * serial half is 0), and so is a real token with either half moved (h+1,
 * h + 2^32).  0 is never a token (slot + 1 >= 1), so `$err:e 0` and
 * process.badhandle() keep their meaning.
 *
 * Lifetime.  No toke call frees a handle: process.wait caches the exit code
 * and the output drained meanwhile, and a later wait, stdout, stderr or kill
 * answers from them (docs/stdlib/process.md), so a waited handle stays live
 * and is answered as before.  Slots are therefore never reused today.  If a
 * close is ever added (it needs a spec story), it must free the slot and give
 * it a fresh serial on reuse; the serial comparison then refuses the old
 * token even when the slot holds a newer child.
 *
 * Bounded: at most TK_PROC_MAX_LIVE slots of 16 bytes (16 MiB).  Each live
 * handle also holds its child's stdin pipe open, so the per-process fd limit
 * is reached long before this cap.  Thread-safe: task workers may call
 * std.process, so every access holds g_proc.mu.  A lookup returns the
 * ProcessHandle after unlocking, which is safe because nothing frees one. */

/* 2^20 live handles: a table of 16 MiB at most; see above for why the fd
 * limit binds first. */
#define TK_PROC_MAX_LIVE (1u << 20)

typedef struct { ProcessHandle *h; uint32_t serial; } ProcSlot;

static struct {
    pthread_mutex_t mu;
    ProcSlot *slots;
    uint32_t  n, cap;
    uint32_t  serial;   /* last serial handed out; 0 is never one */
} g_proc = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0 };

/* proc_register -- give h a slot and return its token, or 0 when the table
 * is full or cannot grow (the caller then disposes of h). */
static int64_t proc_register(ProcessHandle *h)
{
    int64_t tok = 0;
    pthread_mutex_lock(&g_proc.mu);
    if (g_proc.n == g_proc.cap && g_proc.cap < TK_PROC_MAX_LIVE) {
        uint32_t nc = g_proc.cap ? g_proc.cap * 2 : 16;
        if (nc > TK_PROC_MAX_LIVE) nc = TK_PROC_MAX_LIVE;
        ProcSlot *ns = (ProcSlot *)realloc(g_proc.slots, (size_t)nc * sizeof *ns);
        if (ns) { g_proc.slots = ns; g_proc.cap = nc; }
    }
    if (g_proc.n < g_proc.cap) {
        /* Serials stay below 2^31 so a token is a positive i64. */
        g_proc.serial = (g_proc.serial % 0x7fffffffu) + 1u;
        uint32_t i = g_proc.n++;
        g_proc.slots[i].h = h;
        g_proc.slots[i].serial = g_proc.serial;
        tok = (int64_t)(((uint64_t)g_proc.serial << 32) | (uint64_t)(i + 1u));
    }
    pthread_mutex_unlock(&g_proc.mu);
    return tok;
}

/* proc_lookup -- the ProcessHandle a token names, or NULL for anything the
 * runtime did not hand out (0, a forged integer, a pid, a moved token).
 * Never dereferences v. */
static ProcessHandle *proc_lookup(int64_t v)
{
    uint64_t u = (uint64_t)v;
    uint32_t serial = (uint32_t)(u >> 32);
    uint32_t lo = (uint32_t)(u & 0xffffffffu);
    ProcessHandle *h = NULL;
    if (serial == 0 || lo == 0) return NULL;
    pthread_mutex_lock(&g_proc.mu);
    if (lo - 1u < g_proc.n && g_proc.slots[lo - 1u].serial == serial)
        h = g_proc.slots[lo - 1u].h;
    pthread_mutex_unlock(&g_proc.mu);
    return h;
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
    TK_REQUIRE(TK_CAP_PROCESS_SPAWN);
    if (!cmd) return proc_raise(PROCESS_ERR_IO, "process.spawn: null command array");
    const int64_t *block = (const int64_t *)(intptr_t)cmd;
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
    TK_REQUIRE(TK_CAP_PROCESS_SPAWN);
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
    TK_REQUIRE(TK_CAP_PROCESS_SPAWN);
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
