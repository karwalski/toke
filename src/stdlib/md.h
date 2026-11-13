/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_MD_H
#define TK_STDLIB_MD_H

/*
 * md.h — C interface for the std.md standard library module.
 *
 * Type mappings:
 *   Str           = const char *  (null-terminated UTF-8)
 *   Str!MdErr     = MdFileResult
 *
 * Ownership (127.295, runtime-abi §3.1): md_render's result and
 * md_render_file's .ok are sealed strings from the string heap
 * (tk_strhdr.h).  Never free, write or realloc them -- C callers included;
 * copy one first to modify it.  .err_msg is a string literal.
 *
 * Story: 55.5.2  Branch: feature/stdlib-55.5-md
 */

typedef struct { const char *ok; int is_err; const char *err_msg; } MdFileResult;

const char   *md_render(const char *src);
MdFileResult  md_render_file(const char *path);

#endif /* TK_STDLIB_MD_H */
