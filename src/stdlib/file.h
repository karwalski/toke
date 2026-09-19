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
    FILE_ERR_INVALID,
    /* 135.10 — the byte calls distinguish more cases than the three the
     * $fileerr sum type can carry.  The compiled T!E ABI has no payload
     * (127.97), so the $err arm is a bare 0 and these kinds are readable
     * only through file.lasterrkind(); they exist so that "could not read
     * it" is never returned as an empty byte array. */
    FILE_ERR_IS_DIR,       /* the path is a directory                      */
    FILE_ERR_NOT_REGULAR,  /* fifo, socket, device — not a readable file   */
    FILE_ERR_SYMLINK,      /* final component is a symlink (AMB-07)        */
    FILE_ERR_TOO_LARGE,    /* over TK_FILE_MAX_BYTES                       */
    FILE_ERR_NO_MEM,       /* allocation failed for a file that DOES fit   */
    FILE_ERR_BAD_ARG       /* null path or null byte array from the caller */
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

/*
 * 135.10 — binary file access.
 *
 * file_read returns a NUL-terminated char*, so every caller that handed it a
 * zip, a PDF, an image or a spreadsheet got the bytes up to the first zero and
 * no indication that the rest was dropped.  These two calls are the byte-exact
 * pair: length is carried beside the buffer and never inferred from a NUL.
 *
 * TK_FILE_MAX_BYTES is a REAL limit, not a formality: a toke `@(byte)` stores
 * one byte per i64 slot (bytes_rt.h), so a file of N bytes costs 8N in the
 * array plus N in the staging buffer.  At the cap that is ~576 MiB resident for
 * a single call, which is already generous; a larger file is refused as
 * FILE_ERR_TOO_LARGE rather than attempted and truncated or OOM-killed.
 */
#define TK_FILE_MAX_BYTES ((uint64_t)64 * 1024 * 1024)   /* 67108864 */

typedef struct { uint8_t *data; uint64_t len; } FileBytes;
typedef struct { FileBytes ok;  int is_err; FileErr err; } BytesFileResult;

BytesFileResult  file_readbytes(const char *path);
BoolFileResult   file_writebytes(const char *path, const uint8_t *data, uint64_t len);

/* Why the last byte call failed, and which case it was.  The $err arm of a
 * compiled T!E binds nothing (127.97), so without these six distinct failures
 * would be one indistinguishable 0.  "" / "ok" after a call that succeeded. */
const char      *file_lasterr(void);
const char      *file_lasterrkind(void);

/* Record a failure from outside file.c (the glue's own argument checks). */
void             file_setlasterr(FileErrKind kind, const char *msg);

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

/* 55.3.4 — recursive listing */
StrArrayFileResult file_listall(const char *dir);

#endif /* TK_STDLIB_FILE_H */
