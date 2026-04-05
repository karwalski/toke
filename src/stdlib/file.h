#ifndef TK_STDLIB_FILE_H
#define TK_STDLIB_FILE_H

/*
 * file.h — C interface for the std.file standard library module.
 *
 * Type mappings:
 *   FileErr          = struct { FileErrKind kind; const char *msg; }
 *   Str!FileErr      = StrFileResult
 *   bool!FileErr     = BoolFileResult
 *   [Str]!FileErr    = StrArrayFileResult
 *
 * Story: 1.3.5  Branch: feature/stdlib-file
 */

#include <stdint.h>

typedef enum {
    FILE_ERR_NOT_FOUND,
    FILE_ERR_PERMISSION,
    FILE_ERR_IO,
    FILE_ERR_INVALID
} FileErrKind;

typedef struct { FileErrKind kind; const char *msg; } FileErr;

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

typedef struct { const char *ok; int is_err; FileErr err; } StrFileResult;
typedef struct { int ok;         int is_err; FileErr err; } BoolFileResult;
typedef struct { StrArray ok;    int is_err; FileErr err; } StrArrayFileResult;
typedef struct { uint64_t ok;    int is_err; FileErr err; } U64FileResult;

StrFileResult    file_read(const char *path);
BoolFileResult   file_write(const char *path, const char *content);
BoolFileResult   file_append(const char *path, const char *content);
int              file_exists(const char *path);
BoolFileResult   file_delete(const char *path);
StrArrayFileResult file_list(const char *dir);

/* 28.2.1 — directory operations */
BoolFileResult   file_mkdir(const char *path);
BoolFileResult   file_mkdir_p(const char *path);
BoolFileResult   file_rmdir(const char *path);
BoolFileResult   file_rmdir_r(const char *path);
int              file_is_dir(const char *path);
int              file_is_file(const char *path);

/* 28.2.2 — copy, move, and metadata */
BoolFileResult   file_copy(const char *src, const char *dst);
BoolFileResult   file_move(const char *src, const char *dst);
U64FileResult    file_size(const char *path);
U64FileResult    file_mtime(const char *path);

/* 28.2.3 — path utilities */
const char      *file_join(const char *a, const char *b);
const char      *file_basename(const char *path);
const char      *file_dirname(const char *path);
const char      *file_absolute(const char *path);
const char      *file_ext(const char *path);
StrArrayFileResult file_readlines(const char *path);
StrArrayFileResult file_glob(const char *pattern);

#endif /* TK_STDLIB_FILE_H */
