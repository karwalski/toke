/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_ARGS_H
#define TK_STDLIB_ARGS_H

/*
 * args.h — C interface for the std.args standard library module.
 *
 * Story: 55.2  Branch: feature/stdlib-55.2-args
 */

#include <stdint.h>

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

typedef struct { const char *ok; int is_err; const char *err_msg; } StrArgsResult;

void           args_init(int argc, char **argv);
uint64_t       args_count(void);
StrArgsResult  args_get(uint64_t n);
StrArray       args_all(void);

/* 127.470: args_get's out-of-range reason.  args_get returns THIS pointer as
 * err_msg, so a caller can recognise it by address (no strcmp per miss). */
extern const char ARGS_ERR_OOB[];

#endif /* TK_STDLIB_ARGS_H */
