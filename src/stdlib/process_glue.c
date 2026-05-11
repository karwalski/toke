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
