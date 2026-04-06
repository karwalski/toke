/*
 * log.c — Implementation of the std.log standard library module.
 *
 * Emits newline-delimited JSON to stderr.  Output format:
 *   {"level":"info","msg":"...","fields":{"k1":"v1"},"ts":"<ISO8601>"}\n
 *
 * Log level is read once from TK_LOG_LEVEL on first use, then cached.
 * Levels (lowest to highest): DEBUG=0, INFO=1, WARN=2, ERROR=3.
 * Default when TK_LOG_LEVEL is absent or unrecognised: INFO (1).
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.
 *
 * Story: 3.8.1  Branch: feature/stdlib-3.8-log
 */

#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Level constants
 * ---------------------------------------------------------------------- */

#define TK_LOG_DEBUG 0
#define TK_LOG_INFO  1
#define TK_LOG_WARN  2
#define TK_LOG_ERROR 3

/* Sentinel: level has not been initialised yet. */
#define TK_LOG_UNINIT (-1)

static int g_level = TK_LOG_UNINIT;

/* -------------------------------------------------------------------------
 * Format constants — story 28.5.3
 * ---------------------------------------------------------------------- */

#define TK_LOG_FMT_JSON 0
#define TK_LOG_FMT_TEXT 1

static int g_format = TK_LOG_FMT_JSON; /* default: JSON */

/* -------------------------------------------------------------------------
 * Output destination — story 28.5.3
 *
 * NULL means write to stderr.  When non-NULL, this FILE* owns an open fd.
 * ---------------------------------------------------------------------- */

static FILE *g_outfile = NULL; /* NULL → use stderr */

/* Helper: return the active output stream. */
static FILE *output_stream(void)
{
    return (g_outfile != NULL) ? g_outfile : stderr;
}

/* -------------------------------------------------------------------------
 * Level initialisation
 * ---------------------------------------------------------------------- */

static int parse_level(const char *s)
{
    if (!s) return TK_LOG_INFO;
    if (strcmp(s, "DEBUG") == 0) return TK_LOG_DEBUG;
    if (strcmp(s, "INFO")  == 0) return TK_LOG_INFO;
    if (strcmp(s, "WARN")  == 0) return TK_LOG_WARN;
    if (strcmp(s, "ERROR") == 0) return TK_LOG_ERROR;
    return TK_LOG_INFO; /* unrecognised → default INFO */
}

static void ensure_level(void)
{
    if (g_level == TK_LOG_UNINIT) {
        g_level = parse_level(getenv("TK_LOG_LEVEL"));
    }
}

void tk_log_set_level(const char *level)
{
    int parsed = parse_level(level);
    /* parse_level returns INFO for unknown strings; only accept known ones. */
    if (level &&
        (strcmp(level, "DEBUG") == 0 || strcmp(level, "INFO")  == 0 ||
         strcmp(level, "WARN")  == 0 || strcmp(level, "ERROR") == 0)) {
        g_level = parsed;
    }
    /* If level is unrecognised, leave g_level as-is (or initialise). */
    if (g_level == TK_LOG_UNINIT) g_level = TK_LOG_INFO;
}

/* -------------------------------------------------------------------------
 * JSON escaping
 *
 * Writes the JSON-escaped form of src into dst (which must be large enough).
 * Returns the number of bytes written (not counting the NUL terminator).
 * ---------------------------------------------------------------------- */

static int json_escape(char *dst, size_t dst_size, const char *src)
{
    int written = 0;
    if (!src) {
        /* Write JSON null literal */
        if (dst_size >= 5) {
            memcpy(dst, "null", 4);
            dst[4] = '\0';
            return 4;
        }
        return 0;
    }
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* Ensure there is always room for the longest escape (6 bytes) + NUL */
        if ((size_t)written + 8 >= dst_size) break;
        switch (c) {
        case '"':  dst[written++] = '\\'; dst[written++] = '"';  break;
        case '\\': dst[written++] = '\\'; dst[written++] = '\\'; break;
        case '\n': dst[written++] = '\\'; dst[written++] = 'n';  break;
        case '\r': dst[written++] = '\\'; dst[written++] = 'r';  break;
        case '\t': dst[written++] = '\\'; dst[written++] = 't';  break;
        default:
            if (c < 0x20) {
                /* ASCII control character → \uXXXX */
                written += snprintf(dst + written,
                                    dst_size - (size_t)written,
                                    "\\u%04x", c);
            } else {
                dst[written++] = (char)c;
            }
            break;
        }
    }
    dst[written] = '\0';
    return written;
}

/* -------------------------------------------------------------------------
 * Timestamp helper
 *
 * Writes an ISO-8601 UTC timestamp with millisecond precision into buf.
 * e.g. "2026-03-28T12:34:56.789Z"
 * buf must be at least 32 bytes.
 * ---------------------------------------------------------------------- */

static void current_iso8601(char *buf, size_t buf_size)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(buf, buf_size, "1970-01-01T00:00:00.000Z");
        return;
    }
    struct tm *tm_info = gmtime(&ts.tv_sec);
    if (!tm_info) {
        snprintf(buf, buf_size, "1970-01-01T00:00:00.000Z");
        return;
    }
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", tm_info);
    int ms = (int)(ts.tv_nsec / 1000000L);
    snprintf(buf, buf_size, "%s.%03dZ", date_buf, ms);
}

/* -------------------------------------------------------------------------
 * Core emit function
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * emit_json — write a JSON-format log line
 * ---------------------------------------------------------------------- */

static int emit_json(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    FILE *out = output_stream();

    /* Timestamp */
    char ts_buf[32];
    current_iso8601(ts_buf, sizeof(ts_buf));

    /* Escaped message */
    size_t msg_esc_size = msg ? (strlen(msg) * 6 + 8) : 8;
    char *msg_esc = malloc(msg_esc_size);
    if (!msg_esc) return 0;
    json_escape(msg_esc, msg_esc_size, msg);

    /* Begin JSON line: level and msg */
    fprintf(out, "{\"level\":\"%s\",\"msg\":\"%s\"", level_str, msg_esc);
    free(msg_esc);

    /* Inline fields as top-level keys */
    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];

        size_t k_esc_size = k ? (strlen(k) * 6 + 8) : 8;
        size_t v_esc_size = v ? (strlen(v) * 6 + 8) : 8;

        char *k_esc = malloc(k_esc_size);
        char *v_esc = malloc(v_esc_size);
        if (!k_esc || !v_esc) {
            free(k_esc);
            free(v_esc);
            break;
        }
        json_escape(k_esc, k_esc_size, k);
        json_escape(v_esc, v_esc_size, v);

        fprintf(out, ",\"%s\":\"%s\"", k_esc, v_esc);

        free(k_esc);
        free(v_esc);
    }

    fprintf(out, ",\"ts\":\"%s\"}\n", ts_buf);
    fflush(out);
    return 1;
}

/* -------------------------------------------------------------------------
 * emit_text — write a human-readable text log line
 * ---------------------------------------------------------------------- */

static int emit_text(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    FILE *out = output_stream();

    /* Convert level string to uppercase tag, e.g. "info" → "[INFO]" */
    char tag[16];
    int ti = 0;
    tag[ti++] = '[';
    for (const char *p = level_str; *p && ti < 13; p++) {
        unsigned char c = (unsigned char)*p;
        tag[ti++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    tag[ti++] = ']';
    tag[ti]   = '\0';

    fprintf(out, "%s %s", tag, msg ? msg : "");

    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];
        fprintf(out, " %s=%s", k ? k : "", v ? v : "");
    }

    fprintf(out, "\n");
    fflush(out);
    return 1;
}

/* -------------------------------------------------------------------------
 * Core emit dispatcher
 * ---------------------------------------------------------------------- */

static int emit(int msg_level, const char *level_str,
                const char *msg, const char **fields, int field_count)
{
    ensure_level();
    if (msg_level < g_level) return 0;

    if (g_format == TK_LOG_FMT_TEXT) {
        return emit_text(level_str, msg, fields, field_count);
    }
    return emit_json(level_str, msg, fields, field_count);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int tk_log_info(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_INFO, "info", msg, fields, field_count);
}

int tk_log_warn(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_WARN, "warn", msg, fields, field_count);
}

int tk_log_error(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_ERROR, "error", msg, fields, field_count);
}

/* -------------------------------------------------------------------------
 * Story 28.5.3 additions
 * ---------------------------------------------------------------------- */

int tk_log_debug(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_DEBUG, "debug", msg, fields, field_count);
}

void tk_log_set_format(const char *fmt)
{
    if (!fmt) return;
    if (strcmp(fmt, "json") == 0) {
        g_format = TK_LOG_FMT_JSON;
    } else if (strcmp(fmt, "text") == 0) {
        g_format = TK_LOG_FMT_TEXT;
    }
    /* Unknown fmt strings are silently ignored. */
}

int tk_log_set_output(const char *path)
{
    /* Close any previously opened output file. */
    if (g_outfile != NULL) {
        fclose(g_outfile);
        g_outfile = NULL;
    }
    if (path == NULL) {
        /* Caller is resetting back to stderr. */
        return 1;
    }
    g_outfile = fopen(path, "a");
    return (g_outfile != NULL) ? 1 : 0;
}

/*
 * tk_log_with_context — minimal hand-rolled JSON object parser.
 *
 * Parses a JSON object string of the form {"k":"v",...} and collects
 * string-valued key/value pairs, then emits them via tk_log_info.
 *
 * Limitations (intentional, no external deps):
 *   - Only top-level string-to-string pairs are extracted.
 *   - Unicode escape sequences in keys/values are passed through verbatim.
 *   - Nested objects/arrays are skipped.
 */
int tk_log_with_context(const char *msg, const char *context_json)
{
    /* Maximum number of context key/value pairs we support. */
#define MAX_CTX_PAIRS 32

    const char *keys[MAX_CTX_PAIRS];
    const char *vals[MAX_CTX_PAIRS];
    /* Buffers to hold the unescaped strings (generous sizes). */
    char key_bufs[MAX_CTX_PAIRS][256];
    char val_bufs[MAX_CTX_PAIRS][512];
    int pair_count = 0;

    if (!context_json) {
        return tk_log_info(msg, NULL, 0);
    }

    const char *p = context_json;

    /* Skip whitespace helper (inline). */
#define SKIP_WS(ptr) while (*(ptr) == ' ' || *(ptr) == '\t' || \
                            *(ptr) == '\n' || *(ptr) == '\r') (ptr)++

    SKIP_WS(p);
    if (*p != '{') {
        /* Not a JSON object; log without context fields. */
        return tk_log_info(msg, NULL, 0);
    }
    p++; /* consume '{' */

    while (*p && *p != '}' && pair_count < MAX_CTX_PAIRS) {
        SKIP_WS(p);
        if (*p != '"') { p++; continue; } /* skip non-string tokens */

        /* --- parse key --- */
        p++; /* consume opening '"' */
        int ki = 0;
        while (*p && *p != '"' && ki < 255) {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            key_bufs[pair_count][ki++] = *p++;
        }
        key_bufs[pair_count][ki] = '\0';
        if (*p == '"') p++; /* consume closing '"' */

        SKIP_WS(p);
        if (*p != ':') continue; /* malformed; skip */
        p++; /* consume ':' */
        SKIP_WS(p);

        /* Only handle string values; skip non-string values. */
        if (*p != '"') {
            /* Skip past the value token (number, bool, null, object, array). */
            int depth = 0;
            while (*p) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                }
                else if ((*p == ',' || *p == '}') && depth == 0) break;
                p++;
            }
            if (*p == ',') p++;
            continue;
        }

        /* --- parse string value --- */
        p++; /* consume opening '"' */
        int vi = 0;
        while (*p && *p != '"' && vi < 511) {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            val_bufs[pair_count][vi++] = *p++;
        }
        val_bufs[pair_count][vi] = '\0';
        if (*p == '"') p++; /* consume closing '"' */

        keys[pair_count] = key_bufs[pair_count];
        vals[pair_count] = val_bufs[pair_count];
        pair_count++;

        SKIP_WS(p);
        if (*p == ',') p++; /* consume comma between pairs */
    }

#undef SKIP_WS
#undef MAX_CTX_PAIRS

    /* Build the flat fields array expected by emit. */
    const char *flat[64]; /* 2 * MAX_CTX_PAIRS */
    for (int i = 0; i < pair_count; i++) {
        flat[i * 2]     = keys[i];
        flat[i * 2 + 1] = vals[i];
    }

    return emit(TK_LOG_INFO, "info", msg, flat, pair_count * 2);
}

/* ==========================================================================
 * Access log with rotation (Story 47.1.1)
 * ========================================================================== */

#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *k_month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

struct TkAccessLog {
    char *path;          /* full path to live log file */
    char *dir;           /* directory component          */
    char *stem;          /* filename stem (no extension) */
    char *ext;           /* extension incl. dot, or ""   */
    FILE *fp;
    int   line_count;
    int   max_lines;     /* 0 = no rotation              */
    int   max_files;     /* 0 = no limit                 */
    int   max_age_days;  /* 0 = disabled; >0 wins over max_files */
};

static TkAccessLog *g_access_log = NULL;

void tk_access_log_set_global(TkAccessLog *log) { g_access_log = log; }
TkAccessLog *tk_access_log_get_global(void)     { return g_access_log; }

/* ── Path splitting ───────────────────────────────────────────────────── */

static void split_path(const char *path,
                        char **dir_out, char **stem_out, char **ext_out)
{
    const char *slash = strrchr(path, '/');
    const char *filename = slash ? slash + 1 : path;

    if (slash) {
        size_t dlen = (size_t)(slash - path);
        *dir_out = malloc(dlen + 1);
        if (*dir_out) { memcpy(*dir_out, path, dlen); (*dir_out)[dlen] = '\0'; }
    } else {
        *dir_out = strdup(".");
    }

    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        size_t slen = (size_t)(dot - filename);
        *stem_out = malloc(slen + 1);
        if (*stem_out) { memcpy(*stem_out, filename, slen); (*stem_out)[slen] = '\0'; }
        *ext_out = strdup(dot);
    } else {
        *stem_out = strdup(filename);
        *ext_out  = strdup("");
    }
}

/* ── Create directory (single level, ignore EEXIST) ───────────────────── */

static void mkdir_single(const char *dir)
{
    if (!dir || !dir[0] || strcmp(dir, ".") == 0) return;
#if defined(_WIN32)
    (void)mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

/* ── gzip compress src_path → dst_path, unlink src on success ─────────── */

static int compress_to_gz(const char *src_path, const char *dst_path)
{
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;

    gzFile gz = gzopen(dst_path, "wb9");
    if (!gz) { fclose(src); return -1; }

    char buf[65536];
    int ok = 1;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        if (gzwrite(gz, buf, (unsigned)n) == 0) { ok = 0; break; }
    }
    if (ferror(src)) ok = 0;

    fclose(src);
    gzclose(gz);

    if (ok) {
        unlink(src_path);
    } else {
        unlink(dst_path);
    }
    return ok ? 0 : -1;
}

/* ── Retention: delete old rotated .gz files based on policy ───────────── */

static int str_ends_with(const char *s, const char *suffix)
{
    size_t slen   = strlen(s);
    size_t sflen  = strlen(suffix);
    if (sflen > slen) return 0;
    return strcmp(s + slen - sflen, suffix) == 0;
}

#define TK_ALOG_MAX_SCAN 4096

typedef struct { char path[1024]; time_t mtime; } AlogEntry;

static int alog_mtime_cmp(const void *a, const void *b)
{
    const AlogEntry *ea = (const AlogEntry *)a;
    const AlogEntry *eb = (const AlogEntry *)b;
    /* descending: newest first */
    if (eb->mtime > ea->mtime) return  1;
    if (eb->mtime < ea->mtime) return -1;
    return 0;
}

static void apply_retention(TkAccessLog *log)
{
    /* Build match prefix/suffix: "stem." and "ext.gz" */
    char prefix[512], suffix[128];
    snprintf(prefix, sizeof prefix, "%s.", log->stem);
    snprintf(suffix, sizeof suffix, "%s.gz", log->ext);

    AlogEntry *files = malloc(TK_ALOG_MAX_SCAN * sizeof *files);
    if (!files) return;
    int nfiles = 0;

    DIR *d = opendir(log->dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && nfiles < TK_ALOG_MAX_SCAN) {
            const char *name = ent->d_name;
            if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
            if (!str_ends_with(name, suffix))               continue;
            snprintf(files[nfiles].path, sizeof files[nfiles].path,
                     "%s/%s", log->dir, name);
            struct stat st;
            if (stat(files[nfiles].path, &st) != 0) continue;
            files[nfiles].mtime = st.st_mtime;
            nfiles++;
        }
        closedir(d);
    }

    if (log->max_age_days > 0) {
        time_t cutoff = time(NULL) - (time_t)log->max_age_days * 86400;
        for (int i = 0; i < nfiles; i++) {
            if (files[i].mtime < cutoff) unlink(files[i].path);
        }
    } else if (log->max_files > 0 && nfiles > log->max_files) {
        qsort(files, (size_t)nfiles, sizeof *files, alog_mtime_cmp);
        for (int i = log->max_files; i < nfiles; i++) {
            unlink(files[i].path);
        }
    }

    free(files);
}

/* ── Rotate: rename → gz → retention → reopen ─────────────────────────── */

static void rotate_access_log(TkAccessLog *log)
{
    if (log->fp) { fflush(log->fp); fclose(log->fp); log->fp = NULL; }
    log->line_count = 0;

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[20];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tm);

    /* rotated path: dir/stem.YYYYMMDD-HHMMSS.ext */
    char rotated[1024];
    snprintf(rotated, sizeof rotated,
             "%s/%s.%s%s", log->dir, log->stem, ts, log->ext);

    if (rename(log->path, rotated) == 0) {
        char gzpath[1024];
        snprintf(gzpath, sizeof gzpath, "%s.gz", rotated);
        compress_to_gz(rotated, gzpath);
        apply_retention(log);
    }
    /* If rename fails (ENOENT), another worker already rotated — that's fine */

    log->fp = fopen(log->path, "a");
}

/* ── Public API ───────────────────────────────────────────────────────── */

TkAccessLog *tk_access_log_open(const char *path,
                                 int max_lines,
                                 int max_files,
                                 int max_age_days)
{
    if (!path || !path[0]) return NULL;

    TkAccessLog *log = calloc(1, sizeof *log);
    if (!log) return NULL;

    log->path         = strdup(path);
    log->max_lines    = max_lines;
    log->max_files    = max_files;
    log->max_age_days = max_age_days;

    split_path(path, &log->dir, &log->stem, &log->ext);
    mkdir_single(log->dir);

    log->fp = fopen(path, "a");
    if (!log->fp) { tk_access_log_close(log); return NULL; }

    return log;
}

void tk_access_log_write(TkAccessLog *log,
                          const char *ip,
                          const char *method,
                          const char *path,
                          int         status,
                          size_t      bytes,
                          const char *referer,
                          const char *ua)
{
    if (!log) return;
    if (!log->fp) {
        log->fp = fopen(log->path, "a");
        if (!log->fp) return;
    }

    /* Combined Log Format timestamp: [06/Apr/2026:15:23:01 +0000] */
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[40];
    snprintf(ts, sizeof ts, "%02d/%s/%04d:%02d:%02d:%02d +0000",
             tm->tm_mday, k_month_names[tm->tm_mon], 1900 + tm->tm_year,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    char bytes_str[24];
    if (bytes > 0) snprintf(bytes_str, sizeof bytes_str, "%zu", bytes);
    else           strcpy(bytes_str, "-");

    fprintf(log->fp,
            "%s - - [%s] \"%s %s HTTP/1.1\" %d %s \"%s\" \"%s\"\n",
            ip      ? ip      : "-",
            ts,
            method  ? method  : "-",
            path    ? path    : "-",
            status,
            bytes_str,
            referer ? referer : "-",
            ua      ? ua      : "-");
    fflush(log->fp);

    log->line_count++;
    if (log->max_lines > 0 && log->line_count >= log->max_lines)
        rotate_access_log(log);
}

void tk_access_log_close(TkAccessLog *log)
{
    if (!log) return;
    if (log->fp) { fflush(log->fp); fclose(log->fp); }
    free(log->path);
    free(log->dir);
    free(log->stem);
    free(log->ext);
    free(log);
}
