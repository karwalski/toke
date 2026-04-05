/*
 * test_process.c — Unit tests for the std.process C library (Story 2.7.1).
 *
 * Build and run: make test-stdlib-process
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include "../../src/stdlib/process.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* --- Test 1: spawn a known command (true) ----------------------------- */
    {
        const char *cmd[] = {"true", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(true) succeeds");
        if (!sr.is_err) {
            WaitResult wr = process_wait(sr.ok);
            ASSERT(!wr.is_err, "wait(true) no error");
            ASSERT(wr.ok == 0, "wait(true) exit code == 0");
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 2: wait returns non-zero for false -------------------------- */
    {
        const char *cmd[] = {"false", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(false) succeeds");
        if (!sr.is_err) {
            WaitResult wr = process_wait(sr.ok);
            ASSERT(!wr.is_err, "wait(false) no error");
            ASSERT(wr.ok != 0, "wait(false) exit code != 0");
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 3: stdout captures output ----------------------------------- */
    {
        const char *cmd[] = {"echo", "hello toke", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(echo) succeeds");
        if (!sr.is_err) {
            StdoutResult or_ = process_stdout(sr.ok);
            ASSERT(!or_.is_err, "stdout(echo) no error");
            ASSERT(or_.ok && strstr(or_.ok, "hello toke") != NULL,
                   "stdout(echo) contains 'hello toke'");
            process_wait(sr.ok);
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 4: stdout captures multi-line output ------------------------ */
    {
        const char *cmd[] = {"printf", "line1\nline2\n", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(printf multi-line) succeeds");
        if (!sr.is_err) {
            StdoutResult or_ = process_stdout(sr.ok);
            ASSERT(!or_.is_err, "stdout(printf) no error");
            ASSERT(or_.ok && strstr(or_.ok, "line1") != NULL,
                   "stdout(printf) contains 'line1'");
            ASSERT(or_.ok && strstr(or_.ok, "line2") != NULL,
                   "stdout(printf) contains 'line2'");
            process_wait(sr.ok);
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 5: kill sends SIGTERM --------------------------------------- */
    {
        const char *cmd[] = {"sleep", "60", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(sleep 60) succeeds");
        if (!sr.is_err) {
            int killed = process_kill(sr.ok);
            ASSERT(killed == 1, "kill(sleep) returns 1");
            /* wait so we don't leave a zombie */
            process_wait(sr.ok);
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 6: ProcessErr NotFound for missing command ------------------ */
    {
        const char *cmd[] = {"/nonexistent_toke_bin_xyz", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(sr.is_err, "spawn(missing) is error");
        ASSERT(sr.err_kind == PROCESS_ERR_NOT_FOUND ||
               sr.err_kind == PROCESS_ERR_PERMISSION,
               "spawn(missing) err_kind is NotFound or Permission");
    }

    /* --- Test 7: null handle is safe -------------------------------------- */
    {
        WaitResult wr = process_wait(NULL);
        ASSERT(wr.is_err, "wait(NULL) is error");
        StdoutResult or_ = process_stdout(NULL);
        ASSERT(or_.is_err, "stdout(NULL) is error");
        int k = process_kill(NULL);
        ASSERT(k == 0, "kill(NULL) returns 0");
    }

    /* --- Test 8: spawn with arguments passes them correctly --------------- */
    {
        const char *cmd[] = {"sh", "-c", "exit 42", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(sh -c 'exit 42') succeeds");
        if (!sr.is_err) {
            WaitResult wr = process_wait(sr.ok);
            ASSERT(!wr.is_err, "wait(sh exit 42) no error");
            ASSERT(wr.ok == 42, "wait(sh exit 42) exit code == 42");
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 9: double wait is safe (returns cached exit code) ----------- */
    {
        const char *cmd[] = {"true", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(true) for double-wait");
        if (!sr.is_err) {
            process_wait(sr.ok);
            WaitResult wr2 = process_wait(sr.ok);
            ASSERT(!wr2.is_err, "double wait no error");
            ASSERT(wr2.ok == 0, "double wait exit code == 0");
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 10: stdout on already-drained handle returns empty string --- */
    {
        const char *cmd[] = {"echo", "drain me", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "spawn(echo drain me) succeeds");
        if (!sr.is_err) {
            process_stdout(sr.ok);          /* first drain */
            StdoutResult or2 = process_stdout(sr.ok);  /* second call */
            ASSERT(!or2.is_err, "second stdout() call not error");
            ASSERT(or2.ok != NULL, "second stdout() call returns non-NULL");
            process_wait(sr.ok);
            process_handle_free(sr.ok);
        }
    }

    /* === Story 28.5.2 tests ============================================== */

    /* --- Test 11: process_stdin_write + read stdout (cat) ---------------- */
    {
        const char *cmd[] = {"cat", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "28.5.2 spawn(cat) succeeds");
        if (!sr.is_err) {
            ProcessHandle *h = sr.ok;
            U64ProcessResult wr = process_stdin_write(h, "hello", 5);
            ASSERT(!wr.is_err, "28.5.2 stdin_write no error");
            ASSERT(wr.ok == 5, "28.5.2 stdin_write returns 5");
            /* Close stdin so cat knows to stop */
            close(h->stdin_fd);
            h->stdin_fd = -1;
            StdoutResult or_ = process_stdout(h);
            ASSERT(!or_.is_err, "28.5.2 cat stdout no error");
            ASSERT(or_.ok && strstr(or_.ok, "hello") != NULL,
                   "28.5.2 cat stdout contains 'hello'");
            process_wait(h);
            process_handle_free(h);
        }
    }

    /* --- Test 12: process_stderr reads stderr output --------------------- */
    {
        const char *cmd[] = {"sh", "-c", "echo err >&2", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "28.5.2 spawn(sh echo err >&2) succeeds");
        if (!sr.is_err) {
            StrProcessResult er = process_stderr(sr.ok);
            ASSERT(!er.is_err, "28.5.2 stderr no error");
            ASSERT(er.ok && strstr(er.ok, "err") != NULL,
                   "28.5.2 stderr contains 'err'");
            process_wait(sr.ok);
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 13: process_exit_code returns the correct code ------------- */
    {
        const char *cmd[] = {"sh", "-c", "exit 42", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "28.5.2 spawn(sh exit 42) succeeds");
        if (!sr.is_err) {
            I32ProcessResult er = process_exit_code(sr.ok);
            ASSERT(!er.is_err, "28.5.2 exit_code no error");
            ASSERT(er.ok == 42, "28.5.2 exit_code == 42");
            process_handle_free(sr.ok);
        }
    }

    /* --- Test 14: process_is_running ------------------------------------ */
    {
        const char *cmd[] = {"sleep", "10", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "28.5.2 spawn(sleep 10) succeeds");
        if (!sr.is_err) {
            ProcessHandle *h = sr.ok;
            ASSERT(process_is_running(h) == 1, "28.5.2 is_running=1 before kill");
            kill(h->pid, SIGKILL);
            /* Wait for signal to be delivered */
            process_wait(h);
            ASSERT(process_is_running(h) == 0, "28.5.2 is_running=0 after kill");
            process_handle_free(h);
        }
    }

    /* --- Test 15: process_timeout kills long-running process ------------- */
    {
        const char *cmd[] = {"sleep", "10", NULL};
        SpawnResult sr = process_spawn(cmd);
        ASSERT(!sr.is_err, "28.5.2 spawn(sleep 10) for timeout");
        if (!sr.is_err) {
            I32ProcessResult tr = process_timeout(sr.ok, 50);
            ASSERT(!tr.is_err, "28.5.2 timeout no error");
            /* Process was killed — exit code should be non-zero (negative = signal) */
            ASSERT(tr.ok != 0, "28.5.2 timeout exit code non-zero after kill");
            process_handle_free(sr.ok);
        }
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
