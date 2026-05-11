/*
 * process_glue.c — i64-ABI wrappers for std.process module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.process.
 */

#include <stdint.h>

int64_t tk_process_spawn_w(int64_t cmd) { (void)cmd; return 0; }
int64_t tk_process_isalive_w(int64_t pid) { (void)pid; return 0; }
int64_t tk_process_exit_w(int64_t code) { (void)code; return 0; }
int64_t tk_process_stdout_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_stderr_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_kill_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_badhandle_w(void) { return 0; }
int64_t tk_process_wait_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_exitcode_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_hasexited_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_poll_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_print_w(int64_t v) { (void)v; return 0; }
int64_t tk_process_sleep_w(int64_t ms) { (void)ms; return 0; }
