/*
 * process_glue.c — i64-ABI wrappers for std.process module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.process.
 */

#include "process.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int64_t tk_process_spawn_w(int64_t cmd) {
    if (!cmd) return 0;
    const char *cmdstr = (const char *)(intptr_t)cmd;
    /* Build a NULL-terminated argv: {"sh", "-c", cmdstr, NULL} */
    const char *argv[] = { "sh", "-c", cmdstr, NULL };
    SpawnResult r = process_spawn(argv);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}
int64_t tk_process_isalive_w(int64_t pid) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)pid;
    if (!h) return 0;
    return (int64_t)process_is_running(h);
}
int64_t tk_process_exit_w(int64_t code) {
    _exit((int)code);
    return 0;
}
int64_t tk_process_stdout_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return 0;
    StdoutResult r = process_stdout(h);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}
int64_t tk_process_stderr_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return 0;
    StrProcessResult r = process_stderr(h);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}
int64_t tk_process_kill_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return 0;
    return (int64_t)process_kill(h);
}
int64_t tk_process_badhandle_w(void) {
    return 0; /* NULL handle is the sentinel for "bad handle" */
}
int64_t tk_process_wait_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return -1;
    WaitResult r = process_wait(h);
    if (r.is_err) return -1;
    return (int64_t)r.ok;
}
int64_t tk_process_exitcode_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return -1;
    I32ProcessResult r = process_exit_code(h);
    if (r.is_err) return -1;
    return (int64_t)r.ok;
}
int64_t tk_process_hasexited_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
    if (!h) return 1;
    /* If is_running returns 0, the process has exited */
    return process_is_running(h) ? 0 : 1;
}
int64_t tk_process_poll_w(int64_t p) {
    ProcessHandle *h = (ProcessHandle *)(intptr_t)p;
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
    return (int64_t)(intptr_t)strdup(val);
}

/* process.exec(cmd) — run command and return stdout as string */
int64_t tk_process_exec_w(int64_t cmd) {
    if (!cmd) return 0;
    int64_t handle = tk_process_spawn_w(cmd);
    if (!handle) return 0;
    ProcessHandle *h = (ProcessHandle *)(intptr_t)handle;
    process_wait(h);
    StdoutResult r = process_stdout(h);
    if (r.is_err) { process_handle_free(h); return 0; }
    int64_t result = (int64_t)(intptr_t)r.ok;
    process_handle_free(h);
    return result;
}

/* process.homedir() — return user's home directory */
int64_t tk_process_homedir_w(int64_t dummy) {
    (void)dummy;
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    return (int64_t)(intptr_t)strdup(home);
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
    if (!cmd) return 0;
    int64_t handle = tk_process_spawn_w(cmd);
    /* Return pid as integer, don't wait */
    if (!handle) return 0;
    ProcessHandle *h = (ProcessHandle *)(intptr_t)handle;
    int64_t pid = (int64_t)h->pid;
    /* Intentionally don't free — detached process lives independently */
    return pid;
}

/* std.homedir() — alias exposed at top level */
int64_t tk_std_homedir_w(int64_t dummy) {
    return tk_process_homedir_w(dummy);
}
