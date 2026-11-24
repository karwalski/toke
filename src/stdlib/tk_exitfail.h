/* SPDX-License-Identifier: MIT */
/*
 * tk_exitfail.h — "this process has failed": a non-zero exit status that a
 * stdlib call can record for later (stories 127.231 item 2, 135.22).
 *
 * THE PROBLEM.  Two stdlib failures were reported to the caller and nowhere
 * else, and every caller in the tree ignored them:
 *
 *   - a failed test.eq / test.ok / test.assert* printed a DIAGNOSTIC line and
 *     returned false, and the program then returned 0 from main, so a test
 *     program could not fail through its exit status (127.231 item 2);
 *   - http.servetls with a certificate it could not load printed one line and
 *     returned -1, the program ran off the end of main, and the process
 *     exited 0 — a server that never started looked like a clean shutdown to
 *     a supervisor (135.22).
 *
 * THE MECHANISM.  tk_exitfail(reason) records the failure and, the first
 * time, registers an atexit() handler.  When the process later exits through
 * exit() — which is what returning from main does — the handler flushes
 * stdio, prints one line saying why, and ends the process with status 1
 * through _exit().  The failing call still returns its ordinary error value,
 * so a caller can see and react to it; what it can no longer do is exit 0.
 *
 * DECIDED: AT EXIT, NOT IMMEDIATELY.  A test program keeps running after a
 * failed assertion, so every failure in the run is reported, and only the
 * exit status changes.  (An immediate exit would report the first failure
 * and hide the rest.)  Consequences, stated so nobody has to rediscover them:
 *
 *   - the status is main's own when main returned a non-zero one, and 1
 *     otherwise: a program that returns 3 after a failed assertion exits 3
 *     (135.29).  An atexit handler cannot read the status exit() was given,
 *     so the emitted C `main` (src/llvm.c) publishes it first: it stores the
 *     truncated tk_main result in tk_exitfail_main_status just before its
 *     `ret`.  The entry module carries the STRONG definition of that global
 *     (as it does for the capability grants, capabilities.c); the weak one
 *     here is what a unit test or a void main links against, and reads 0.
 *     A status whose low 8 bits are 0 (256, …) would exit 0 and counts as 0;
 *   - os.exit(0) after a failure exits 1 as well, with the same line: os.exit
 *     is _exit() and skips atexit handlers, so tk_os_exit calls
 *     tk_exitfail_atexit() itself for a status of 0, and process.exit
 *     (process_glue.c tk_process_exit_w, 127.532) does the same;
 *   - handlers registered BEFORE the first failure do not run, because the
 *     handler ends the process with _exit().  In this tree that is
 *     tk_err_release (frees one buffer) and, under ASan, the leak check;
 *   - a process killed by a signal is unaffected.
 *
 * WHY A HEADER WITH WEAK DEFINITIONS.  The record must be ONE object shared
 * by tk_test.c, tk_web_glue.c and os.c, and the stdlib is linked per module
 * by basename (stdlib_deps.c) and compiled into unit tests in ones and twos,
 * so a new .c file would have to join every link line that reaches any of
 * the three.  A weak definition in a header is emitted by every includer and
 * merged by the linker into one — the same reason strbuf.h is header-only.
 */
#ifndef TK_EXITFAIL_H
#define TK_EXITFAIL_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__GNUC__) || defined(__clang__)
#  define TK_EXITFAIL_WEAK __attribute__((weak))
#else
#  error "tk_exitfail.h needs weak definitions (GCC or Clang)"
#endif

/* Number of failures recorded; 0 = none.  Shared across translation units. */
TK_EXITFAIL_WEAK volatile long tk_exitfail_count = 0;
/* The first failure's reason (a string literal the caller passes). */
TK_EXITFAIL_WEAK const char *volatile tk_exitfail_reason = NULL;
/* 135.29: the status main returned, published by the emitted C main just
 * before its `ret` (strong definition in the entry module); 0 until then. */
TK_EXITFAIL_WEAK volatile int tk_exitfail_main_status = 0;

/* 127.529: the TK_MEM_STATS exit report, which tk_runtime_init installs
 * when TK_MEM_STATS=1 (NULL otherwise).  An atexit handler registered before
 * this file's does not run once it _exit()s, nor does any after os.exit's
 * _exit(), so both call it first.  It prints at most once. */
TK_EXITFAIL_WEAK void (*volatile tk_exit_report)(void) = NULL;

TK_EXITFAIL_WEAK void tk_exitfail_atexit(void)
{
    if (tk_exitfail_count <= 0) return;
    int ms = tk_exitfail_main_status;
    int status = (ms & 0xff) ? (ms & 0xff) : 1;
    fflush(NULL);
    fprintf(stderr,
            "DIAGNOSTIC: kind=exit_status status=%d failures=%ld first=\"%s\"\n",
            status, (long)tk_exitfail_count,
            tk_exitfail_reason ? tk_exitfail_reason : "(unknown)");
    fflush(stderr);
    if (tk_exit_report) tk_exit_report();
    _exit(status);
}

/* Record a failure.  `reason` must outlive the process (a literal). */
TK_EXITFAIL_WEAK void tk_exitfail(const char *reason)
{
    if (tk_exitfail_count++ == 0) {
        tk_exitfail_reason = reason;
        atexit(tk_exitfail_atexit);
    }
}

#endif /* TK_EXITFAIL_H */
