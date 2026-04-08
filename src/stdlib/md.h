#ifndef TK_STDLIB_MD_H
#define TK_STDLIB_MD_H

/*
 * md.h — C interface for the std.md standard library module.
 *
 * Type mappings:
 *   Str           = const char *  (null-terminated UTF-8)
 *   Str!MdErr     = MdFileResult
 *
 * Story: 55.5.2  Branch: feature/stdlib-55.5-md
 */

typedef struct { const char *ok; int is_err; const char *err_msg; } MdFileResult;

const char   *md_render(const char *src);
MdFileResult  md_render_file(const char *path);

#endif /* TK_STDLIB_MD_H */
