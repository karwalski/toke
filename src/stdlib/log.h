#ifndef TK_STDLIB_LOG_H
#define TK_STDLIB_LOG_H

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

#endif /* TK_STDLIB_LOG_H */
