#ifndef TK_STDLIB_ENV_H
#define TK_STDLIB_ENV_H

/*
 * env.h — C interface for the std.env standard library module.
 *
 * Type mappings:
 *   Str        = const char *   (null-terminated UTF-8)
 *   bool       = int            (0 = false, 1 = true)
 *   Str!EnvErr = EnvGetResult
 *
 * Story: 2.7.2  Branch: feature/stdlib-2.7-process-env
 * Story: 28.5.1 env_list, env_delete, env_expand, env_file_load
 */

#include <stdint.h>

/*
 * StrArray — heap-allocated array of C strings.
 * data[0..len-1] are individually heap-allocated; callers own the array.
 */
typedef struct { const char **data; uint64_t len; } EnvStrArray;

/* EnvErr variant enum */
typedef enum {
    ENV_ERR_NONE     = 0,
    ENV_ERR_NOT_FOUND = 1,  /* key is not set in the environment */
    ENV_ERR_INVALID   = 2   /* key or value contains an invalid character (e.g. '=', NUL) */
} EnvErrKind;

/* Result type for env.get */
typedef struct { const char *ok; int is_err; EnvErrKind err_kind; const char *err_msg; } EnvGetResult;

/*
 * env_get — look up an environment variable.
 * Returns EnvGetResult.ok (pointer into the live environ, do not free) on success,
 * or is_err=1 / err_kind=ENV_ERR_NOT_FOUND if the key is absent.
 */
EnvGetResult env_get(const char *key);

/*
 * env_get_or — look up a key; return default_val if the key is absent.
 * Never returns an error — missing key yields the default.
 */
const char  *env_get_or(const char *key, const char *default_val);

/*
 * env_set — set key=val in the process environment using setenv(3).
 * Returns 1 on success, 0 on failure (invalid key/value or OS error).
 */
int          env_set(const char *key, const char *val);

/*
 * env_list — return all environment variable names as a heap-allocated
 * EnvStrArray.  The caller owns the array and each string inside it.
 * Returns an EnvStrArray with len==0 and data==NULL on allocation failure.
 */
EnvStrArray  env_list(void);

/*
 * env_delete — unset an environment variable via unsetenv(3).
 * Returns 1 on success, 0 on error (invalid key or unsetenv failure).
 */
int          env_delete(const char *key);

/*
 * env_expand — substitute $VAR, ${VAR}, and $$ in tmpl.
 *   $VAR     ends at the first character that is not [A-Za-z0-9_]
 *   ${VAR}   ends at the closing '}'
 *   $$       expands to a single literal '$'
 *   unknown variable names expand to ""
 * Returns a heap-allocated NUL-terminated string; caller must free.
 * Returns NULL on allocation failure.
 */
char        *env_expand(const char *tmpl);

/*
 * env_file_load — parse a KEY=VALUE dotenv file at path.
 *   - Blank lines and lines beginning with '#' are skipped.
 *   - Leading/trailing whitespace is stripped from keys and unquoted values.
 *   - Values may be double- or single-quoted; quotes are stripped.
 *   - Inside double-quoted values, \n \t \\ are recognised.
 *   - Each pair is installed via setenv(3) (overwrites existing).
 * Returns the number of variables set, or -1 on file-open failure.
 */
int          env_file_load(const char *path);

#endif /* TK_STDLIB_ENV_H */
