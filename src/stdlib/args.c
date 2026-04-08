/*
 * args.c — Implementation of the std.args standard library module.
 *
 * Captures argc/argv at runtime startup (before main() runs) and
 * exposes them via a read-only API.  Thread-safe after init.
 *
 * Story: 55.2  Branch: feature/stdlib-55.2-args
 */

#include "args.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Set by args_init() at startup; read-only thereafter. */
static int    tk_argc = 0;
static char **tk_argv = NULL;

/*
 * args_init — Called by the runtime before toke main() to capture the
 * process argument vector.  Must be called exactly once.  argv must
 * remain valid for the lifetime of the process (the OS guarantees this
 * for the argv passed to C main()).
 */
void args_init(int argc, char **argv)
{
    tk_argc = argc;
    tk_argv = argv;
}

/*
 * args_count — Returns the number of command-line arguments (including
 * argv[0], the program name) as a u64.
 */
uint64_t args_count(void)
{
    return (uint64_t)tk_argc;
}

/*
 * args_get — Returns the nth argument string.  argv[0] is the program
 * name.  Returns an error result if n is out of bounds.  The returned
 * pointer is into the original argv array and must not be freed.
 */
StrArgsResult args_get(uint64_t n)
{
    StrArgsResult r = {NULL, 0, NULL};

    if (tk_argv == NULL || n >= (uint64_t)tk_argc) {
        r.is_err  = 1;
        r.err_msg = "index out of bounds";
        return r;
    }

    r.ok = tk_argv[n];
    return r;
}

/*
 * args_all — Returns all argv entries as a StrArray.  The data pointer
 * points directly into the original argv array; callers must not free it.
 * Returns an empty StrArray when args_init has not yet been called.
 */
StrArray args_all(void)
{
    StrArray a;
    a.data = (tk_argv != NULL) ? (const char **)tk_argv : NULL;
    a.len  = (uint64_t)tk_argc;
    return a;
}
