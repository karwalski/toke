#ifndef TK_STDLIB_PATH_H
#define TK_STDLIB_PATH_H

/*
 * path.h — C interface for the std.path standard library module.
 *
 * Type mappings:
 *   Str  = const char *  (null-terminated UTF-8)
 *   bool = int           (0 = false, 1 = true)
 *
 * Story: 55.1  Branch: feature/stdlib-55.1-path
 */

const char *path_join(const char *a, const char *b);
const char *path_ext(const char *p);
const char *path_stem(const char *p);
const char *path_dir(const char *p);
const char *path_base(const char *p);
int         path_isabs(const char *p);

#endif /* TK_STDLIB_PATH_H */
