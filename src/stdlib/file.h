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
    FILE_ERR_IO
} FileErrKind;

typedef struct { FileErrKind kind; const char *msg; } FileErr;

typedef struct { const char **data; uint64_t len; } StrArray;

typedef struct { const char *ok; int is_err; FileErr err; } StrFileResult;
typedef struct { int ok;         int is_err; FileErr err; } BoolFileResult;
typedef struct { StrArray ok;    int is_err; FileErr err; } StrArrayFileResult;

StrFileResult    file_read(const char *path);
BoolFileResult   file_write(const char *path, const char *content);
BoolFileResult   file_append(const char *path, const char *content);
int              file_exists(const char *path);
BoolFileResult   file_delete(const char *path);
StrArrayFileResult file_list(const char *dir);

#endif /* TK_STDLIB_FILE_H */
