/*
 * content.c — Content transformation, negotiation, and serving for std.http.
 *
 * Epic 64: Brotli/Zstandard compression (compile-time optional),
 * content negotiation, Range requests, custom error pages, directory index,
 * header manipulation middleware.
 */

#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── Optional compression library support ────────────────────────────── */

#ifdef TK_HAVE_BROTLI
#include <brotli/encode.h>
#include <brotli/decode.h>
#endif

#ifdef TK_HAVE_ZSTD
#include <zstd.h>
#endif

/* Already have zlib for gzip */
#include <zlib.h>

/* ── Accept-Encoding parsing (64.2.1) ────────────────────────────────── */

typedef struct {
    const char *encoding;
    float       quality;
} EncodingPref;

/* Parse Accept-Encoding header into preference list, sorted by quality.
 * Returns count of encodings parsed. */
static int parse_accept_encoding(const char *header, EncodingPref *out,
                                  int max_out)
{
    if (!header || !out) return 0;
    int count = 0;
    const char *p = header;

    while (*p && count < max_out) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Read encoding name */
        const char *name_start = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
        size_t name_len = (size_t)(p - name_start);

        float q = 1.0f;
        /* Check for quality value */
        while (*p == ' ') p++;
        if (*p == ';') {
            p++;
            while (*p == ' ') p++;
            if (*p == 'q' || *p == 'Q') {
                p++;
                if (*p == '=') {
                    p++;
                    q = (float)atof(p);
                    while (*p && *p != ',') p++;
                }
            }
        }

        if (name_len > 0 && name_len < 32) {
            char name[32];
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';
            out[count].encoding = strdup(name);
            out[count].quality = q;
            count++;
        }
    }

    /* Sort by quality (descending) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (out[j].quality > out[i].quality) {
                EncodingPref tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    return count;
}

/* Select best encoding from client preferences.
 * Priority: br > zstd > gzip > identity.
 * Returns encoding name or "identity". */
const char *content_select_encoding(const char *accept_encoding)
{
    EncodingPref prefs[16];
    int n = parse_accept_encoding(accept_encoding, prefs, 16);

    for (int i = 0; i < n; i++) {
        if (prefs[i].quality <= 0) continue;
#ifdef TK_HAVE_BROTLI
        if (strcasecmp(prefs[i].encoding, "br") == 0) {
            for (int j = 0; j < n; j++) free((void *)prefs[j].encoding);
            return "br";
        }
#endif
#ifdef TK_HAVE_ZSTD
        if (strcasecmp(prefs[i].encoding, "zstd") == 0) {
            for (int j = 0; j < n; j++) free((void *)prefs[j].encoding);
            return "zstd";
        }
#endif
        if (strcasecmp(prefs[i].encoding, "gzip") == 0) {
            for (int j = 0; j < n; j++) free((void *)prefs[j].encoding);
            return "gzip";
        }
    }

    for (int j = 0; j < n; j++) free((void *)prefs[j].encoding);
    return "identity";
}

/* ── Brotli compression (64.1.1) ─────────────────────────────────────── */

#ifdef TK_HAVE_BROTLI
uint8_t *content_compress_brotli(const uint8_t *input, size_t input_len,
                                  size_t *out_len)
{
    size_t max_out = BrotliEncoderMaxCompressedSize(input_len);
    uint8_t *output = malloc(max_out);
    if (!output) return NULL;

    *out_len = max_out;
    if (BrotliEncoderCompress(BROTLI_DEFAULT_QUALITY,
                              BROTLI_DEFAULT_WINDOW,
                              BROTLI_DEFAULT_MODE,
                              input_len, input,
                              out_len, output) != BROTLI_TRUE) {
        free(output);
        return NULL;
    }
    return output;
}

uint8_t *content_decompress_brotli(const uint8_t *input, size_t input_len,
                                    size_t *out_len)
{
    /* Estimate output size */
    size_t buf_size = input_len * 4;
    if (buf_size < 4096) buf_size = 4096;
    uint8_t *output = malloc(buf_size);
    if (!output) return NULL;

    *out_len = buf_size;
    if (BrotliDecoderDecompress(input_len, input,
                                out_len, output) != BROTLI_DECODER_RESULT_SUCCESS) {
        free(output);
        return NULL;
    }
    return output;
}
#endif

/* ── Zstandard compression (64.1.2) ──────────────────────────────────── */

#ifdef TK_HAVE_ZSTD
uint8_t *content_compress_zstd(const uint8_t *input, size_t input_len,
                                size_t *out_len)
{
    size_t max_out = ZSTD_compressBound(input_len);
    uint8_t *output = malloc(max_out);
    if (!output) return NULL;

    *out_len = ZSTD_compress(output, max_out, input, input_len,
                              ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(*out_len)) {
        free(output);
        return NULL;
    }
    return output;
}

uint8_t *content_decompress_zstd(const uint8_t *input, size_t input_len,
                                  size_t *out_len)
{
    unsigned long long frame_size = ZSTD_getFrameContentSize(input, input_len);
    if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        frame_size == ZSTD_CONTENTSIZE_ERROR)
        return NULL;

    uint8_t *output = malloc((size_t)frame_size);
    if (!output) return NULL;

    *out_len = ZSTD_decompress(output, (size_t)frame_size,
                                input, input_len);
    if (ZSTD_isError(*out_len)) {
        free(output);
        return NULL;
    }
    return output;
}
#endif

/* ── Response decompression (64.1.3) ─────────────────────────────────── */

/* Decompress gzip data (always available since we have zlib) */
uint8_t *content_decompress_gzip(const uint8_t *input, size_t input_len,
                                  size_t *out_len)
{
    /* Estimate output size */
    size_t buf_size = input_len * 4;
    if (buf_size < 4096) buf_size = 4096;
    uint8_t *output = malloc(buf_size);
    if (!output) return NULL;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)input;
    strm.avail_in = (uInt)input_len;
    strm.next_out = (Bytef *)output;
    strm.avail_out = (uInt)buf_size;

    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        free(output);
        return NULL;
    }

    int ret = inflate(&strm, Z_FINISH);
    *out_len = strm.total_out;
    inflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        free(output);
        return NULL;
    }
    return output;
}

/* ── Pre-compressed file serving (64.1.4) ────────────────────────────── */

/* Check for pre-compressed variant of a file path.
 * Returns heap-allocated path to compressed file, or NULL. */
char *content_find_precompressed(const char *path,
                                  const char *accept_encoding)
{
    if (!path || !accept_encoding) return NULL;

    struct stat st;

    /* Try .br first */
    if (strstr(accept_encoding, "br")) {
        size_t len = strlen(path) + 4;
        char *br_path = malloc(len);
        if (br_path) {
            snprintf(br_path, len, "%s.br", path);
            if (stat(br_path, &st) == 0 && S_ISREG(st.st_mode))
                return br_path;
            free(br_path);
        }
    }

    /* Try .gz */
    if (strstr(accept_encoding, "gzip")) {
        size_t len = strlen(path) + 4;
        char *gz_path = malloc(len);
        if (gz_path) {
            snprintf(gz_path, len, "%s.gz", path);
            if (stat(gz_path, &st) == 0 && S_ISREG(st.st_mode))
                return gz_path;
            free(gz_path);
        }
    }

    /* Try .zst */
    if (strstr(accept_encoding, "zstd")) {
        size_t len = strlen(path) + 5;
        char *zst_path = malloc(len);
        if (zst_path) {
            snprintf(zst_path, len, "%s.zst", path);
            if (stat(zst_path, &st) == 0 && S_ISREG(st.st_mode))
                return zst_path;
            free(zst_path);
        }
    }

    return NULL;
}

/* ── Accept-Language negotiation (64.2.2) ─────────────────────────────── */

/* Parse Accept-Language and find best match from available languages.
 * Returns matched language tag or NULL. */
const char *content_negotiate_language(const char *accept_lang,
                                        const char **available,
                                        int navailable)
{
    if (!accept_lang || !available || navailable <= 0) return NULL;

    /* Parse Accept-Language with quality values */
    EncodingPref prefs[16];
    /* Reuse the Accept-Encoding parser — format is identical */
    int n = parse_accept_encoding(accept_lang, prefs, 16);

    const char *best = NULL;
    for (int i = 0; i < n && !best; i++) {
        if (prefs[i].quality <= 0) continue;
        for (int j = 0; j < navailable; j++) {
            /* Exact match or prefix match (e.g. "en" matches "en-US") */
            if (strcasecmp(prefs[i].encoding, available[j]) == 0 ||
                (strlen(prefs[i].encoding) <= strlen(available[j]) &&
                 strncasecmp(prefs[i].encoding, available[j],
                             strlen(prefs[i].encoding)) == 0 &&
                 (available[j][strlen(prefs[i].encoding)] == '-' ||
                  available[j][strlen(prefs[i].encoding)] == '\0'))) {
                best = available[j];
                break;
            }
        }
    }

    for (int j = 0; j < n; j++) free((void *)prefs[j].encoding);
    return best;
}

/* ── Range request support (64.2.3) ──────────────────────────────────── */

typedef struct {
    long start;
    long end;   /* -1 = to end of file */
} ByteRange;

/* Parse Range header. Returns number of ranges parsed, -1 on error. */
int content_parse_range(const char *range_header, long content_length,
                         ByteRange *ranges, int max_ranges)
{
    if (!range_header || !ranges) return -1;

    /* Must start with "bytes=" */
    if (strncasecmp(range_header, "bytes=", 6) != 0) return -1;

    const char *p = range_header + 6;
    int count = 0;

    while (*p && count < max_ranges) {
        while (*p == ' ') p++;

        long start = -1, end = -1;

        if (*p == '-') {
            /* Suffix range: -500 means last 500 bytes */
            p++;
            long suffix = atol(p);
            start = content_length - suffix;
            if (start < 0) start = 0;
            end = content_length - 1;
        } else {
            start = atol(p);
            while (*p >= '0' && *p <= '9') p++;
            if (*p != '-') return -1;
            p++;
            if (*p >= '0' && *p <= '9') {
                end = atol(p);
                while (*p >= '0' && *p <= '9') p++;
            } else {
                end = content_length - 1;
            }
        }

        /* Validate */
        if (start < 0 || start >= content_length) return -1;
        if (end < start || end >= content_length) end = content_length - 1;

        ranges[count].start = start;
        ranges[count].end = end;
        count++;

        while (*p == ' ') p++;
        if (*p == ',') p++;
        else break;
    }

    return count;
}

/* ── Custom error pages (64.2.4) ─────────────────────────────────────── */

#define MAX_ERROR_PAGES 32

typedef struct {
    uint32_t    status;
    char       *html;
    char       *vhost;  /* NULL = global */
} ErrorPage;

static ErrorPage g_error_pages[MAX_ERROR_PAGES];
static int       g_error_page_count = 0;

/* Register a custom error page */
void content_set_error_page(uint32_t status, const char *html,
                             const char *vhost)
{
    if (g_error_page_count >= MAX_ERROR_PAGES) return;
    ErrorPage *ep = &g_error_pages[g_error_page_count++];
    ep->status = status;
    ep->html = strdup(html);
    ep->vhost = vhost ? strdup(vhost) : NULL;
}

/* Look up a custom error page. Returns NULL if not configured. */
const char *content_get_error_page(uint32_t status, const char *vhost)
{
    /* Check vhost-specific first, then global */
    for (int i = 0; i < g_error_page_count; i++) {
        if (g_error_pages[i].status == status && g_error_pages[i].vhost &&
            vhost && strcasecmp(g_error_pages[i].vhost, vhost) == 0)
            return g_error_pages[i].html;
    }
    for (int i = 0; i < g_error_page_count; i++) {
        if (g_error_pages[i].status == status && !g_error_pages[i].vhost)
            return g_error_pages[i].html;
    }
    return NULL;
}

/* ── Directory index (64.2.5) ────────────────────────────────────────── */

static const char *g_index_files[] = {
    "index.html", "index.htm", "default.html", NULL
};

/* Check if a directory has an index file. Returns heap-allocated path or NULL. */
char *content_find_index(const char *dir_path)
{
    if (!dir_path) return NULL;

    struct stat st;
    for (int i = 0; g_index_files[i]; i++) {
        size_t len = strlen(dir_path) + strlen(g_index_files[i]) + 2;
        char *path = malloc(len);
        if (!path) continue;
        snprintf(path, len, "%s/%s", dir_path, g_index_files[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            return path;
        free(path);
    }
    return NULL;
}

/* Check if path is a directory and needs trailing slash redirect */
int content_needs_trailing_slash(const char *path)
{
    if (!path) return 0;
    size_t len = strlen(path);
    if (len == 0 || path[len - 1] == '/') return 0;

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;
    return 0;
}

/* ── Header manipulation middleware (64.2.6) ─────────────────────────── */

#define MAX_HEADER_RULES 64

typedef enum {
    HDR_ADD,    /* add header (append) */
    HDR_SET,    /* set header (replace) */
    HDR_REMOVE, /* remove header */
} HeaderRuleAction;

typedef struct {
    HeaderRuleAction action;
    char *name;
    char *value;   /* NULL for REMOVE */
    int   is_response; /* 1 = response header, 0 = request header */
} HeaderRule;

static HeaderRule g_hdr_rules[MAX_HEADER_RULES];
static int        g_hdr_rule_count = 0;

/* Parse a header rule string like "add:X-Custom:value" or "remove:Server" */
int content_add_header_rule(const char *rule, int is_response)
{
    if (!rule || g_hdr_rule_count >= MAX_HEADER_RULES) return -1;

    HeaderRule *hr = &g_hdr_rules[g_hdr_rule_count];
    hr->is_response = is_response;

    if (strncasecmp(rule, "add:", 4) == 0) {
        hr->action = HDR_ADD;
        rule += 4;
    } else if (strncasecmp(rule, "set:", 4) == 0) {
        hr->action = HDR_SET;
        rule += 4;
    } else if (strncasecmp(rule, "remove:", 7) == 0) {
        hr->action = HDR_REMOVE;
        rule += 7;
        hr->name = strdup(rule);
        hr->value = NULL;
        g_hdr_rule_count++;
        return 0;
    } else {
        return -1;
    }

    /* Parse name:value */
    const char *colon = strchr(rule, ':');
    if (!colon) {
        hr->name = strdup(rule);
        hr->value = strdup("");
    } else {
        hr->name = strndup(rule, (size_t)(colon - rule));
        hr->value = strdup(colon + 1);
    }

    g_hdr_rule_count++;
    return 0;
}

/* Apply header rules to a set of headers.
 * Modifies headers in place for SET/REMOVE, appends for ADD.
 * Returns number of headers after modification. */
int content_apply_header_rules(StrPair *headers, int count, int max_headers,
                                int is_response)
{
    for (int r = 0; r < g_hdr_rule_count; r++) {
        HeaderRule *hr = &g_hdr_rules[r];
        if (hr->is_response != is_response) continue;

        switch (hr->action) {
        case HDR_ADD:
            if (count < max_headers) {
                headers[count].key = hr->name;
                headers[count].val = hr->value;
                count++;
            }
            break;

        case HDR_SET:
            /* Find and replace, or add if not found */
            {
                int found = 0;
                for (int i = 0; i < count; i++) {
                    if (strcasecmp(headers[i].key, hr->name) == 0) {
                        headers[i].val = hr->value;
                        found = 1;
                        break;
                    }
                }
                if (!found && count < max_headers) {
                    headers[count].key = hr->name;
                    headers[count].val = hr->value;
                    count++;
                }
            }
            break;

        case HDR_REMOVE:
            for (int i = 0; i < count; i++) {
                if (strcasecmp(headers[i].key, hr->name) == 0) {
                    /* Shift remaining headers */
                    for (int j = i; j < count - 1; j++) {
                        headers[j] = headers[j + 1];
                    }
                    count--;
                    i--;
                }
            }
            break;
        }
    }
    return count;
}
