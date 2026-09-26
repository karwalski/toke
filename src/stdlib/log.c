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
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>   /* flock — 127.191 */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *k_month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/*
 * 127.191 — rotation is shared state, so it is decided under a lock and from
 * the FILE, never from a per-process counter.
 *
 * Three defects lost lines here, each on its own:
 *   1. the archive name had one-second resolution, so a second rotation in
 *      the same second renamed onto, and gzipped over, the first archive;
 *   2. a pre-fork server opens the log once and forks, so every worker held
 *      its own count and its own FILE*: when one rotated, the others went on
 *      writing into the renamed inode, which the gzip step then unlinked;
 *   3. the count started at 0 on every open, whatever the file already held.
 *
 * So every write now takes an exclusive flock on a sidecar lock file (the
 * live file cannot carry the lock, because rotation replaces its inode), and
 * under that lock: reopens the live file if its inode is not the one this
 * process holds; counts the newlines appended since this process last looked
 * (its own and every other writer's) — so the count is the shared file's,
 * and on first open it is the whole existing file's; writes; and rotates
 * when the count reaches max_lines.  Archive names carry microseconds and
 * the pid, and a name already taken (plain or .gz) is never reused.
 *
 * flock() locks belong to the open file description, which fork() SHARES, so
 * the lock fd is reopened in any process that did not open it (lock_pid).
 */
struct TkAccessLog {
    char *path;          /* full path to live log file */
    char *dir;           /* directory component          */
    char *stem;          /* filename stem (no extension) */
    char *ext;           /* extension incl. dot, or ""   */
    char *lock_path;     /* sidecar lock file (127.191)  */
    FILE *fp;
    int   lock_fd;       /* -1 = not open in this process */
    pid_t lock_pid;      /* process that opened lock_fd  */
    dev_t cur_dev;       /* identity of the inode fp holds */
    ino_t cur_ino;
    off_t counted_to;    /* bytes of that inode already counted */
    long  line_count;    /* newlines in that inode up to counted_to */
    int   max_lines;     /* 0 = no rotation              */
    int   max_files;     /* 0 = no limit                 */
    int   max_age_days;  /* 0 = disabled; >0 wins over max_files */
};

static TkAccessLog *g_access_log = NULL;
static TkAccessLog *g_error_log  = NULL;

/* ── Access log format flag (Story 47.1.3) ────────────────────────────── */

#define TK_ACCESS_FMT_COMBINED 0
#define TK_ACCESS_FMT_JSON     1

static int g_access_format = TK_ACCESS_FMT_COMBINED;

void tk_access_log_set_global(TkAccessLog *log) { g_access_log = log; }
TkAccessLog *tk_access_log_get_global(void)     { return g_access_log; }

void tk_error_log_set_global(TkAccessLog *log)  { g_error_log = log; }
TkAccessLog *tk_error_log_get_global(void)      { return g_error_log; }

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

/* ── 127.191: the lock, the inode check and the count ─────────────────── */

static int alog_lock(TkAccessLog *log)
{
    if (!log->lock_path) return -1;      /* stream mode: never locked */
    if (log->lock_fd >= 0 && log->lock_pid != getpid()) {
        /* Inherited across fork: the description (and so the lock) is shared
         * with the parent.  Drop our copy and open our own. */
        close(log->lock_fd);
        log->lock_fd = -1;
    }
    if (log->lock_fd < 0) {
        log->lock_fd = open(log->lock_path, O_RDWR | O_CREAT, 0644);
        if (log->lock_fd < 0) return -1;
        log->lock_pid = getpid();
    }
    while (flock(log->lock_fd, LOCK_EX) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static void alog_unlock(TkAccessLog *log)
{
    if (log->lock_fd >= 0) flock(log->lock_fd, LOCK_UN);
}

/* (Re)open the live file and forget the count: it belongs to the old inode. */
static void alog_reopen(TkAccessLog *log)
{
    if (log->fp) { fclose(log->fp); log->fp = NULL; }
    log->counted_to = 0;
    log->line_count = 0;
    log->cur_dev = 0;
    log->cur_ino = 0;
    /* "a+" so the newline count can pread() what other writers appended. */
    log->fp = fopen(log->path, "a+");
    if (!log->fp) return;
    setvbuf(log->fp, NULL, _IOLBF, 0);
    struct stat st;
    if (fstat(fileno(log->fp), &st) == 0) {
        log->cur_dev = st.st_dev;
        log->cur_ino = st.st_ino;
    }
}

/* Make fp the file now at log->path.  Called with the lock held. */
static void alog_sync_inode(TkAccessLog *log)
{
    struct stat ps;
    if (!log->fp || stat(log->path, &ps) != 0 ||
        ps.st_dev != log->cur_dev || ps.st_ino != log->cur_ino)
        alog_reopen(log);
}

/* Count the newlines appended since counted_to — by anyone.  Lock held. */
static void alog_count(TkAccessLog *log)
{
    if (!log->fp) return;
    struct stat st;
    if (fstat(fileno(log->fp), &st) != 0) return;
    if (st.st_size < log->counted_to) {          /* truncated under us */
        log->counted_to = 0;
        log->line_count = 0;
    }
    char buf[65536];
    while (log->counted_to < st.st_size) {
        off_t want = st.st_size - log->counted_to;
        size_t n = want > (off_t)sizeof buf ? sizeof buf : (size_t)want;
        ssize_t r = pread(fileno(log->fp), buf, n, log->counted_to);
        if (r <= 0) break;
        for (ssize_t k = 0; k < r; k++) if (buf[k] == '\n') log->line_count++;
        log->counted_to += r;
    }
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* ── Rotate: rename → reopen → (unlock) → gz → retention ──────────────── */

/* Called with the lock held; returns with it RELEASED.  Only the rename and
 * the reopen need the lock — once the live name points at a fresh inode no
 * other writer can reach the old one — so the gzip, which is proportional to
 * the file, does not stall every other worker. */
static void rotate_access_log_locked(TkAccessLog *log)
{
    if (log->fp) fflush(log->fp);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm = gmtime(&now);
    char ts[20];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tm);

    /* rotated path: dir/stem.YYYYMMDD-HHMMSS.uuuuuu-PID[-N].ext — microseconds
     * and the pid make a collision unlikely; the probe makes it impossible,
     * because every rotator holds the lock while it chooses. */
    char rotated[1024], gzpath[1100];
    for (int n = 0; ; n++) {
        if (n == 0)
            snprintf(rotated, sizeof rotated, "%s/%s.%s.%06ld-%ld%s", log->dir,
                     log->stem, ts, (long)tv.tv_usec, (long)getpid(), log->ext);
        else
            snprintf(rotated, sizeof rotated, "%s/%s.%s.%06ld-%ld-%d%s", log->dir,
                     log->stem, ts, (long)tv.tv_usec, (long)getpid(), n, log->ext);
        snprintf(gzpath, sizeof gzpath, "%s.gz", rotated);
        if (!path_exists(rotated) && !path_exists(gzpath)) break;
        if (n > 10000) { alog_unlock(log); return; }   /* give up: keep writing */
    }

    int moved = rename(log->path, rotated) == 0;
    alog_reopen(log);          /* creates the new live file */
    alog_unlock(log);

    if (moved) {
        compress_to_gz(rotated, gzpath);
        apply_retention(log);
    }
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
    log->lock_fd      = -1;

    /* A path that exists and is not a regular file (a tty, /dev/stderr, a
     * FIFO) cannot be rotated, counted or given a sidecar lock beside it --
     * "/dev/.stderr.lock" cannot be created, which made 127.191's open fail
     * for the stderr access log ooke uses by default.  Open it for plain
     * append; with no lock_path the writer never takes the lock and so
     * never rotates (tk_access_log_write's unlocked path). */
    struct stat pst;
    if (stat(path, &pst) == 0 && !S_ISREG(pst.st_mode)) {
        log->fp = fopen(path, "a");
        if (!log->fp) { tk_access_log_close(log); return NULL; }
        return log;
    }

    split_path(path, &log->dir, &log->stem, &log->ext);
    mkdir_single(log->dir);

    /* dir/.stem.ext.lock — hidden, and not matched by apply_retention (which
     * takes only stem.*ext.gz). */
    size_t ll = strlen(log->dir) + strlen(log->stem) + strlen(log->ext) + 16;
    log->lock_path = malloc(ll);
    if (!log->path || !log->lock_path) { tk_access_log_close(log); return NULL; }
    snprintf(log->lock_path, ll, "%s/.%s%s.lock", log->dir, log->stem, log->ext);

    if (alog_lock(log) != 0) {
        /* No sidecar lock can be made beside this path (/dev/stderr, even
         * when it resolves to a redirected regular file; a read-only dir).
         * A log that cannot rotate safely must still log: drop to plain
         * append with no lock_path, which never rotates. */
        free(log->lock_path);
        log->lock_path = NULL;
        log->fp = fopen(path, "a");
        if (!log->fp) { tk_access_log_close(log); return NULL; }
        return log;
    }
    alog_reopen(log);
    alog_count(log);           /* 127.191(3): the file's lines count */
    alog_unlock(log);
    if (!log->fp) { tk_access_log_close(log); return NULL; }

    return log;
}

/* ── Access log format selection (Story 47.1.3) ───────────────────────── */

int tk_access_log_set_format(const char *fmt)
{
    if (fmt && strcmp(fmt, "json") == 0) {
        g_access_format = TK_ACCESS_FMT_JSON;
        return 0;
    }
    if (fmt && strcmp(fmt, "combined") == 0) {
        g_access_format = TK_ACCESS_FMT_COMBINED;
        return 0;
    }
    /* 127.191: an unknown format used to be ignored silently, so a typo
     * ("jsno") left the log in combined format with nothing to say why. */
    fprintf(stderr, "log.accessformat: unknown format \"%s\" "
                    "(expected \"combined\" or \"json\"); format unchanged\n",
            fmt ? fmt : "(null)");
    return -1;
}

/*
 * access_json_escape — minimal JSON escaping for access log string fields.
 * Escapes only " and \.  dst must be at least dst_size bytes.
 * Returns number of bytes written (excluding NUL).
 *
 * Story: 47.1.3
 */
static int access_json_escape(char *dst, size_t dst_size, const char *src)
{
    int w = 0;
    if (!src) { dst[0] = '\0'; return 0; }
    for (const char *p = src; *p && (size_t)(w + 3) < dst_size; p++) {
        if (*p == '"')       { dst[w++] = '\\'; dst[w++] = '"'; }
        else if (*p == '\\') { dst[w++] = '\\'; dst[w++] = '\\'; }
        else                 { dst[w++] = *p; }
    }
    dst[w] = '\0';
    return w;
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
    /* 127.191: decide against the shared file, under the lock. */
    int locked = alog_lock(log) == 0;
    if (locked) alog_sync_inode(log);
    else if (!log->fp) alog_reopen(log);
    if (!log->fp) { if (locked) alog_unlock(log); return; }

    if (g_access_format == TK_ACCESS_FMT_JSON) {
        /* NDJSON format (Story 47.1.3) */
        char ts[32];
        current_iso8601(ts, sizeof ts);

        char ip_esc[256], path_esc[2048], ua_esc[1024], ref_esc[2048];
        access_json_escape(ip_esc,   sizeof ip_esc,   ip);
        access_json_escape(path_esc, sizeof path_esc, path);
        access_json_escape(ua_esc,   sizeof ua_esc,   ua);
        access_json_escape(ref_esc,  sizeof ref_esc,  referer);

        fprintf(log->fp,
                "{\"ts\":\"%s\",\"ip\":\"%s\",\"method\":\"%s\","
                "\"path\":\"%s\",\"status\":%d,\"size\":%zu,"
                "\"ua\":\"%s\",\"ref\":\"%s\"}\n",
                ts,
                ip      ? ip_esc   : "-",
                method  ? method   : "-",
                path    ? path_esc : "-",
                status,
                bytes,
                ua      ? ua_esc   : "-",
                referer ? ref_esc  : "-");
    } else {
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
    }
    fflush(log->fp);
    if (!locked) return;       /* no lock, no shared count: never rotate blind */

    alog_count(log);           /* this line and every other writer's */
    if (log->max_lines > 0 && log->line_count >= log->max_lines)
        rotate_access_log_locked(log);   /* releases the lock */
    else
        alog_unlock(log);
}

void tk_access_log_close(TkAccessLog *log)
{
    if (!log) return;
    if (log->fp) { fflush(log->fp); fclose(log->fp); }
    if (log->lock_fd >= 0) close(log->lock_fd);
    free(log->lock_path);
    free(log->path);
    free(log->dir);
    free(log->stem);
    free(log->ext);
    free(log);
}

/* ==========================================================================
 * Error log with rotation (Story 47.1.2)
 *
 * Reuses the TkAccessLog infrastructure — same struct, rotation, gzip,
 * and retention logic.  A separate global pointer (g_error_log) keeps
 * access and error streams independent.
 * ========================================================================== */

TkAccessLog *tk_error_log_open(const char *path,
                                int max_lines,
                                int max_files,
                                int max_age_days)
{
    /* Delegate to the same open logic used for access logs. */
    return tk_access_log_open(path, max_lines, max_files, max_age_days);
}

void tk_error_log_write(TkAccessLog *log,
                         const char *ip,
                         const char *method,
                         const char *path,
                         int         status,
                         size_t      bytes,
                         const char *referer,
                         const char *ua)
{
    /* Same Combined Log Format line, reusing the access-log writer. */
    tk_access_log_write(log, ip, method, path, status, bytes, referer, ua);
}

void tk_error_log_close(TkAccessLog *log)
{
    tk_access_log_close(log);
}
