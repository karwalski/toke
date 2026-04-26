#ifndef TK_STDLIB_LOG_H
#define TK_STDLIB_LOG_H

#include <stddef.h>

/*
 * log.h — C interface for the std.log standard library module.
 *
 * Named log.h; internally all symbols are prefixed tk_log_ to avoid
 * collisions with any system macros.
 *
 * Type mappings:
 *   Str      = const char *  (null-terminated UTF-8)
 *   bool     = int           (0 = false / filtered, 1 = true / emitted)
 *   [[Str]]  = const char ** (flat array of interleaved key/value pairs)
 *
 * Log levels (TK_LOG_LEVEL env var): DEBUG, INFO, WARN, ERROR
 * Default level when TK_LOG_LEVEL is unset: INFO
 *
 * Story: 3.8.1  Branch: feature/stdlib-3.8-log
 */

/*
 * tk_log_set_level — programmatically override the log level.
 * Accepts the same strings as TK_LOG_LEVEL: "DEBUG", "INFO", "WARN", "ERROR".
 * Unknown strings are silently ignored (level remains unchanged).
 */
void tk_log_set_level(const char *level);

/*
 * tk_log_info — emit an INFO-level structured log line to stderr.
 *
 * msg        — human-readable message string (JSON-escaped on output).
 * fields     — flat array of interleaved key/value strings:
 *              ["key1","val1","key2","val2", ...].  May be NULL if field_count==0.
 * field_count — total number of elements in fields (must be even).
 *
 * Returns 1 if the line was emitted, 0 if filtered by the current log level.
 */
int tk_log_info(const char *msg, const char **fields, int field_count);

/*
 * tk_log_warn — emit a WARN-level structured log line to stderr.
 * Same signature and return semantics as tk_log_info.
 */
int tk_log_warn(const char *msg, const char **fields, int field_count);

/*
 * tk_log_error — emit an ERROR-level structured log line to stderr.
 * Same signature and return semantics as tk_log_info.
 */
int tk_log_error(const char *msg, const char **fields, int field_count);

/*
 * tk_log_debug — emit a DEBUG-level structured log line.
 * Only emitted when the current log level threshold is DEBUG.
 * Same signature and return semantics as tk_log_info.
 *
 * Story: 28.5.3
 */
int tk_log_debug(const char *msg, const char **fields, int field_count);

/*
 * tk_log_set_format — select output format for all subsequent log calls.
 *
 * fmt == "json"  (default): newline-delimited JSON objects.
 *   {"level":"info","msg":"...","key":"val",...,"ts":"..."}
 * fmt == "text": human-readable text lines.
 *   [INFO] msg key=val key2=val2
 *
 * Unknown fmt strings are silently ignored.
 *
 * Story: 28.5.3
 */
void tk_log_set_format(const char *fmt);

/*
 * tk_log_set_output — redirect all log output to the file at path.
 * The file is opened for append; subsequent log calls write there instead
 * of stderr.  Passing NULL resets output back to stderr.
 *
 * Returns 1 on success, 0 on failure (file could not be opened).
 * Not thread-safe.
 *
 * Story: 28.5.3
 */
int tk_log_set_output(const char *path);

/*
 * tk_log_with_context — log msg at INFO level, merging fields from a JSON
 * object string.
 *
 * context_json must be a JSON object, e.g. {"request_id":"abc","user":"bob"}.
 * Key/value pairs are extracted (string values only) and appended as fields.
 * Malformed JSON is tolerated: any parseable pairs are used, the rest ignored.
 *
 * Returns 1 if the line was emitted, 0 if filtered by the current log level.
 *
 * Story: 28.5.3
 */
int tk_log_with_context(const char *msg, const char *context_json);

/* ── Access log with rotation ────────────────────────────────────────── */

/*
 * TkAccessLog — opaque handle for a rotating HTTP access log.
 *
 * Writes Apache/Nginx Combined Log Format lines.
 * Rotates when max_lines is reached: renames the file, gzips it, then
 * applies the retention policy.
 *
 * Retention priority (both may be set; max_age_days wins when > 0):
 *   max_age_days > 0  → delete rotated files older than this many days
 *   max_files    > 0  → keep only the N most-recent rotated files
 *   both zero         → keep all rotated files
 *
 * Story: 47.1.1
 */
typedef struct TkAccessLog TkAccessLog;

/*
 * tk_access_log_open — open (or create) a rotating access log at path.
 *
 * max_lines:    rotate after this many lines (0 = no rotation)
 * max_files:    keep at most this many rotated .gz files (0 = no limit)
 * max_age_days: delete rotated files older than N days (0 = disabled;
 *               when > 0 takes priority over max_files)
 *
 * Returns a TkAccessLog* on success, NULL on failure.
 * The parent directory is created (single level) if it does not exist.
 */
TkAccessLog *tk_access_log_open(const char *path,
                                 int max_lines,
                                 int max_files,
                                 int max_age_days);

/*
 * tk_access_log_write — append one Combined Log Format line.
 *
 * ip:       client IP string, e.g. "1.2.3.4" (NULL → "-")
 * method:   HTTP method, e.g. "GET"           (NULL → "-")
 * path:     request path, e.g. "/index.html"  (NULL → "-")
 * status:   HTTP status code, e.g. 200
 * bytes:    response body size in bytes (0 → "-" in log)
 * referer:  Referer header value         (NULL → "-")
 * ua:       User-Agent header value      (NULL → "-")
 *
 * Rotates the file automatically when max_lines is reached.
 */
void tk_access_log_write(TkAccessLog *log,
                          const char *ip,
                          const char *method,
                          const char *path,
                          int         status,
                          size_t      bytes,
                          const char *referer,
                          const char *ua);

/*
 * tk_access_log_close — flush, close, and free the access log handle.
 * Passing NULL is a no-op.
 */
void tk_access_log_close(TkAccessLog *log);

/*
 * tk_access_log_set_global / tk_access_log_get_global
 *
 * Register a TkAccessLog* as the process-global access log.  http.c
 * calls tk_access_log_get_global() on every request; if non-NULL it
 * writes the access-log entry automatically.
 */
void         tk_access_log_set_global(TkAccessLog *log);
TkAccessLog *tk_access_log_get_global(void);

/* ── Error log with rotation (Story 47.1.2) ──────────────────────────── */

/*
 * Separate error log for 4xx/5xx HTTP responses and server-level errors.
 * Uses the same TkAccessLog infrastructure (rotation, gzip, retention).
 *
 * tk_error_log_open — open (or create) a rotating error log at path.
 *   Same parameters and semantics as tk_access_log_open.
 *
 * tk_error_log_write — append one error line (Combined Log Format).
 *   Same signature as tk_access_log_write; called automatically by http.c
 *   when a 4xx/5xx response is sent.
 *
 * tk_error_log_close — flush, close, and free the error log handle.
 *
 * tk_error_log_set_global / tk_error_log_get_global — register/retrieve
 *   the process-global error log.  http.c calls get_global() on every
 *   4xx/5xx response; if non-NULL it writes the error-log entry.
 */
TkAccessLog *tk_error_log_open(const char *path,
                                int max_lines,
                                int max_files,
                                int max_age_days);
void tk_error_log_write(TkAccessLog *log,
                         const char *ip,
                         const char *method,
                         const char *path,
                         int         status,
                         size_t      bytes,
                         const char *referer,
                         const char *ua);
void tk_error_log_close(TkAccessLog *log);
void         tk_error_log_set_global(TkAccessLog *log);
TkAccessLog *tk_error_log_get_global(void);

#endif /* TK_STDLIB_LOG_H */
