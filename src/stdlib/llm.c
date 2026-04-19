/*
 * llm.c — Implementation of the std.llm standard library module.
 *
 * Implements an OpenAI-compatible chat completion client using POSIX sockets
 * over plain HTTP/1.1.  All TCP I/O is done with getaddrinfo + connect +
 * send/recv; no libcurl or other external HTTP library is used.
 *
 * HTTPS note: TLS wrapping (e.g. via OpenSSL BIO) is not implemented here.
 * If base_url begins with "https://" and TLS support is not compiled in,
 * llm_chat / llm_chatstream will return an is_err response.  For local
 * inference servers (http://localhost:…) plain HTTP works without TLS.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own all returned heap pointers.
 *
 * Story: 17.1.1
 */

#include "llm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* POSIX socket headers */
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* strdup_safe — portable strdup; returns NULL on NULL input or alloc failure. */
static char *strdup_safe(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* err_resp — build a TkLlmResp that signals an error. */
static TkLlmResp err_resp(const char *msg)
{
    TkLlmResp r;
    memset(&r, 0, sizeof(r));
    r.is_err  = 1;
    r.err_msg = strdup_safe(msg);
    return r;
}

/* err_stream — build a TkLlmStream that signals an error. */
static TkLlmStream err_stream(const char *msg)
{
    TkLlmStream s;
    memset(&s, 0, sizeof(s));
    s.is_err  = 1;
    s.err_msg = strdup_safe(msg);
    return s;
}

/* -----------------------------------------------------------------------
 * URL parsing
 * ----------------------------------------------------------------------- */

/*
 * parse_url — split base_url + path_suffix into host, port, and full path.
 * Writes NUL-terminated strings into host_out (max host_cap bytes) and
 * path_out (max path_cap bytes).  Returns 0 on success, -1 on error.
 *
 * Supports:
 *   http://host/path        → port 80
 *   http://host:port/path   → explicit port
 *   https://…               → port 443 (caller should check for TLS)
 */
static int parse_url(const char *base_url, const char *path_suffix,
                     char *host_out, size_t host_cap,
                     char *port_out, size_t port_cap,
                     char *path_out, size_t path_cap,
                     int  *is_https_out)
{
    const char *p = base_url;
    *is_https_out = 0;

    if (strncmp(p, "https://", 8) == 0) {
        *is_https_out = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        return -1;
    }

    /* find end of host[:port] */
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : (p + strlen(p));

    /* check for explicit port */
    const char *colon = NULL;
    for (const char *q = p; q < hostend; q++) {
        if (*q == ':') { colon = q; break; }
    }

    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_cap) return -1;
        memcpy(host_out, p, hlen);
        host_out[hlen] = '\0';

        size_t plen = (size_t)(hostend - colon - 1);
        if (plen >= port_cap) return -1;
        memcpy(port_out, colon + 1, plen);
        port_out[plen] = '\0';
    } else {
        size_t hlen = (size_t)(hostend - p);
        if (hlen >= host_cap) return -1;
        memcpy(host_out, p, hlen);
        host_out[hlen] = '\0';
        snprintf(port_out, port_cap, "%s", *is_https_out ? "443" : "80");
    }

    /* build path: base path component + suffix */
    const char *base_path = slash ? slash : "";
    snprintf(path_out, path_cap, "%s%s", base_path, path_suffix ? path_suffix : "");
    if (path_out[0] == '\0') {
        snprintf(path_out, path_cap, "/");
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * POSIX TCP connect
 * ----------------------------------------------------------------------- */

/* tcp_connect — open a TCP socket to host:port.
 * Returns a connected fd >= 0, or -1 on error. */
static int tcp_connect(const char *host, const char *port, uint32_t timeout_ms)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* set receive / send timeout */
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec  = (long)(timeout_ms / 1000);
            tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* -----------------------------------------------------------------------
 * HTTP request / response
 * ----------------------------------------------------------------------- */

/* http_post — send a POST request and return the full response body.
 * Returns heap-allocated string (caller frees) or NULL on error.
 * http_status_out receives the numeric HTTP status code. */
static char *http_post(const char *host, const char *port, const char *path,
                        const char *api_key, const char *body,
                        uint32_t timeout_ms, int *http_status_out)
{
    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) return NULL;

    size_t body_len = body ? strlen(body) : 0;

    /* build request */
    char req_buf[8192];
    int req_len;
    if (api_key && api_key[0]) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, api_key, body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, body_len);
    }

    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        close(fd); return NULL;
    }

    if (send(fd, req_buf, (size_t)req_len, 0) < 0) { close(fd); return NULL; }
    if (body && body_len > 0) {
        if (send(fd, body, body_len, 0) < 0) { close(fd); return NULL; }
    }

    /* read full response */
    size_t cap = 65536, used = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return NULL; }

    ssize_t n;
    while ((n = recv(fd, resp + used, cap - used - 1, 0)) > 0) {
        used += (size_t)n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(resp, cap);
            if (!tmp) { free(resp); close(fd); return NULL; }
            resp = tmp;
        }
    }
    resp[used] = '\0';
    close(fd);

    /* parse status line: "HTTP/1.x NNN ..." */
    if (http_status_out) {
        *http_status_out = 0;
        const char *sp = strchr(resp, ' ');
        if (sp) *http_status_out = atoi(sp + 1);
    }

    /* find body after \r\n\r\n */
    const char *sep = strstr(resp, "\r\n\r\n");
    if (!sep) { free(resp); return NULL; }
    char *body_copy = strdup_safe(sep + 4);
    free(resp);
    return body_copy;
}

/* -----------------------------------------------------------------------
 * JSON builder
 * ----------------------------------------------------------------------- */

/* json_escape — write JSON-escaped version of src into dst (max dst_cap).
 * Returns number of bytes written (not counting NUL), or -1 if truncated. */
static int json_escape(const char *src, char *dst, size_t dst_cap)
{
    size_t out = 0;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char esc[8];
        int esc_len = 0;
        if      (c == '"')  { esc[0] = '\\'; esc[1] = '"';  esc_len = 2; }
        else if (c == '\\') { esc[0] = '\\'; esc[1] = '\\'; esc_len = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n';  esc_len = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r';  esc_len = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't';  esc_len = 2; }
        else if (c < 0x20)  { snprintf(esc, sizeof(esc), "\\u%04x", c); esc_len = 6; }
        else                { esc[0] = (char)c; esc_len = 1; }

        if (out + (size_t)esc_len + 1 >= dst_cap) return -1;
        memcpy(dst + out, esc, (size_t)esc_len);
        out += (size_t)esc_len;
    }
    dst[out] = '\0';
    return (int)out;
}

const char *llm_build_request(TkLlmMsg *msgs, uint64_t nmsgs, const char *model,
                               double temperature, int stream)
{
    /* Estimate buffer size: generous upper bound */
    size_t cap = 256 + (model ? strlen(model) : 0);
    for (uint64_t i = 0; i < nmsgs; i++) {
        cap += 32;
        if (msgs[i].role)    cap += strlen(msgs[i].role)    * 2 + 4;
        if (msgs[i].content) cap += strlen(msgs[i].content) * 6 + 4; /* worst case escaping */
    }

    char *buf = malloc(cap);
    if (!buf) return NULL;

    int pos = 0;

#define APPEND(fmt, ...) \
    do { int _n = snprintf(buf + pos, cap - (size_t)pos, fmt, __VA_ARGS__); \
         if (_n < 0 || (size_t)_n >= cap - (size_t)pos) { free(buf); return NULL; } \
         pos += _n; } while (0)
#define APPENDS(s) APPEND("%s", s)

    APPEND("{\"model\":\"%s\",\"messages\":[", model ? model : "");

    for (uint64_t i = 0; i < nmsgs; i++) {
        if (i > 0) APPENDS(",");
        APPENDS("{\"role\":\"");

        if (msgs[i].role) {
            /* role shouldn't need escaping but be safe */
            char esc[256];
            if (json_escape(msgs[i].role, esc, sizeof(esc)) < 0) { free(buf); return NULL; }
            APPEND("%s", esc);
        }
        APPENDS("\",\"content\":\"");

        if (msgs[i].content) {
            /* content may be large — escape into a separate heap buffer */
            size_t clen = strlen(msgs[i].content);
            size_t esc_cap = clen * 6 + 4;
            char *esc = malloc(esc_cap);
            if (!esc) { free(buf); return NULL; }
            if (json_escape(msgs[i].content, esc, esc_cap) < 0) { free(esc); free(buf); return NULL; }
            /* make sure esc fits in remaining buf */
            size_t esc_len = strlen(esc);
            if ((size_t)pos + esc_len + 64 >= cap) {
                cap = (size_t)pos + esc_len + 256;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(esc); free(buf); return NULL; }
                buf = tmp;
            }
            memcpy(buf + pos, esc, esc_len);
            pos += (int)esc_len;
            buf[pos] = '\0';
            free(esc);
        }
        APPENDS("\"}");
    }

    APPEND("],\"temperature\":%.2f,\"stream\":%s}",
           temperature,
           stream ? "true" : "false");

#undef APPENDS
#undef APPEND

    return buf;
}

/* -----------------------------------------------------------------------
 * JSON response parsing (minimal, no external library)
 * ----------------------------------------------------------------------- */

/*
 * find_json_string — locate the value of the first occurrence of "key":"..."
 * in json_text.  Returns a heap-allocated copy of the string value, or NULL.
 * Only handles simple strings (no nested objects in the value).
 */
static char *find_json_string(const char *json_text, const char *key)
{
    /* build search pattern  "key":" */
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json_text, pat);
    if (!p) return NULL;
    p += strlen(pat);
    /* skip whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++; /* skip opening quote */

    /* decode JSON string */
    size_t cap = 4096;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t outpos = 0;

    while (*p && *p != '"') {
        if (outpos + 8 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  out[outpos++] = '"';  break;
                case '\\': out[outpos++] = '\\'; break;
                case '/':  out[outpos++] = '/';  break;
                case 'n':  out[outpos++] = '\n'; break;
                case 'r':  out[outpos++] = '\r'; break;
                case 't':  out[outpos++] = '\t'; break;
                case 'b':  out[outpos++] = '\b'; break;
                case 'f':  out[outpos++] = '\f'; break;
                default:   out[outpos++] = *p;   break;
            }
        } else {
            out[outpos++] = *p;
        }
        p++;
    }
    out[outpos] = '\0';
    return out;
}

/*
 * find_json_uint64 — locate "key":N in json_text and return N.
 * Returns 0 if not found.
 */
static uint64_t find_json_uint64(const char *json_text, const char *key)
{
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json_text, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return 0;
    return (uint64_t)strtoull(p, NULL, 10);
}

/* -----------------------------------------------------------------------
 * SSE chunk parser
 * ----------------------------------------------------------------------- */

const char *llm_parse_sse_chunk(const char *sse_data)
{
    if (!sse_data) return NULL;

    /* strip leading whitespace */
    while (*sse_data == ' ' || *sse_data == '\t') sse_data++;

    /* "[DONE]" sentinel */
    if (strcmp(sse_data, "[DONE]") == 0) return NULL;

    /*
     * We want choices[0].delta.content.
     * Strategy: find "delta":{...} then "content":"..." within it.
     * A minimal approach: find the first "delta": then search for "content":
     * within the remaining text up to the matching closing brace.
     */
    const char *delta_start = strstr(sse_data, "\"delta\":");
    if (!delta_start) {
        /* no delta key — return empty string */
        return strdup_safe("");
    }
    /* move past "delta": */
    delta_start += strlen("\"delta\":");
    while (*delta_start == ' ' || *delta_start == '\t') delta_start++;
    if (*delta_start != '{') return strdup_safe("");

    /* find closing brace of delta object (naive: first '}') */
    const char *delta_end = strchr(delta_start, '}');
    if (!delta_end) return strdup_safe("");

    /* search for "content": within [delta_start, delta_end] */
    const char *content_key = strstr(delta_start, "\"content\":");
    if (!content_key || content_key > delta_end) {
        /* delta exists but has no content field (e.g. role-only chunk) */
        return strdup_safe("");
    }

    return find_json_string(delta_start, "content");
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

TkLlmClient *llm_client(const char *base_url, const char *api_key, const char *model)
{
    TkLlmClient *c = malloc(sizeof(TkLlmClient));
    if (!c) return NULL;
    c->base_url               = strdup_safe(base_url);
    c->api_key                = strdup_safe(api_key);
    c->model                  = strdup_safe(model);
    c->timeout_ms             = 0;
    c->max_retries            = 0;
    c->retry_base_delay_ms    = 0;
    c->retry_max_retries      = 0;
    c->total_input_tokens     = 0;
    c->total_output_tokens    = 0;
    return c;
}

void llm_client_free(TkLlmClient *c)
{
    if (!c) return;
    free((void *)c->base_url);
    free((void *)c->api_key);
    free((void *)c->model);
    free(c);
}

TkLlmResp llm_chat(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs, double temperature)
{
    if (!c) return err_resp("null client");

    /* reject HTTPS when TLS is not compiled in */
    if (c->base_url && strncmp(c->base_url, "https://", 8) == 0) {
        return err_resp("HTTPS requires TLS support (not compiled in); use http:// for local inference");
    }

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(c->base_url, "/chat/completions", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0) {
        return err_resp("failed to parse base_url");
    }

    const char *body = llm_build_request(msgs, nmsgs, c->model, temperature, 0);
    if (!body) return err_resp("failed to build request JSON");

    uint32_t timeout  = c->timeout_ms  ? c->timeout_ms  : 30000;
    uint32_t retries  = c->max_retries ? c->max_retries : 3;

    char *resp_body = NULL;
    int   http_status = 0;

    for (uint32_t attempt = 0; attempt <= retries; attempt++) {
        if (attempt > 0) {
            /* 1-second backoff between retries */
            sleep(1);
        }
        free(resp_body);
        resp_body = http_post(host, port, path, c->api_key, body, timeout, &http_status);
        if (!resp_body) continue;
        /* retry on 5xx */
        if (http_status >= 500 && http_status < 600) continue;
        break;
    }

    free((void *)body);

    if (!resp_body) {
        return err_resp("HTTP request failed after retries");
    }

    if (http_status >= 400) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", http_status);
        free(resp_body);
        return err_resp(errbuf);
    }

    /*
     * Parse:  {"choices":[{"message":{"role":"assistant","content":"..."}}],
     *          "usage":{"prompt_tokens":N,"completion_tokens":N}}
     *
     * We find "content": after "message": to avoid matching the input messages.
     */
    const char *msg_start = strstr(resp_body, "\"message\":");
    char *content = NULL;
    if (msg_start) {
        content = find_json_string(msg_start, "content");
    }
    if (!content) {
        /* fallback: try top-level content */
        content = find_json_string(resp_body, "content");
    }

    uint64_t prompt_tokens     = find_json_uint64(resp_body, "prompt_tokens");
    uint64_t completion_tokens = find_json_uint64(resp_body, "completion_tokens");

    free(resp_body);

    /* accumulate usage into client */
    c->total_input_tokens  += prompt_tokens;
    c->total_output_tokens += completion_tokens;

    TkLlmResp r;
    memset(&r, 0, sizeof(r));
    r.content       = content ? content : strdup_safe("");
    r.input_tokens  = prompt_tokens;
    r.output_tokens = completion_tokens;
    r.is_err        = 0;
    r.err_msg       = NULL;
    return r;
}

TkLlmStream llm_chatstream(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs, double temperature)
{
    if (!c) return err_stream("null client");

    /* reject HTTPS when TLS is not compiled in */
    if (c->base_url && strncmp(c->base_url, "https://", 8) == 0) {
        return err_stream("HTTPS requires TLS support (not compiled in); use http:// for local inference");
    }

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(c->base_url, "/chat/completions", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0) {
        return err_stream("failed to parse base_url");
    }

    const char *body = llm_build_request(msgs, nmsgs, c->model, temperature, 1);
    if (!body) return err_stream("failed to build request JSON");

    uint32_t timeout = c->timeout_ms ? c->timeout_ms : 30000;
    int fd = tcp_connect(host, port, timeout);
    if (fd < 0) { free((void *)body); return err_stream("TCP connect failed"); }

    /* send request */
    size_t body_len = strlen(body);
    char req_buf[8192];
    int req_len;
    if (c->api_key && c->api_key[0]) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, c->api_key, body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, body_len);
    }
    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        free((void *)body);
        close(fd); return err_stream("request header too large");
    }
    if (send(fd, req_buf, (size_t)req_len, 0) < 0 ||
        send(fd, body_len > 0 ? (char *)body : "", body_len, 0) < 0) {
        free((void *)body);
        close(fd); return err_stream("send failed");
    }
    free((void *)body);

    /* read full response */
    size_t cap = 65536, used = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return err_stream("out of memory"); }

    ssize_t n;
    while ((n = recv(fd, resp + used, cap - used - 1, 0)) > 0) {
        used += (size_t)n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(resp, cap);
            if (!tmp) { free(resp); close(fd); return err_stream("out of memory"); }
            resp = tmp;
        }
    }
    resp[used] = '\0';
    close(fd);

    /* skip HTTP headers */
    const char *sep = strstr(resp, "\r\n\r\n");
    const char *sse_body = sep ? sep + 4 : resp;

    /* collect chunks — parse SSE lines */
    size_t chunks_cap = 64;
    const char **chunks = malloc(chunks_cap * sizeof(const char *));
    if (!chunks) { free(resp); return err_stream("out of memory"); }
    uint64_t nchunks = 0;

    const char *line = sse_body;
    while (line && *line) {
        /* find end of line */
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        /* strip trailing \r */
        const char *line_end = line + line_len;
        if (line_end > line && *(line_end - 1) == '\r') line_end--;
        size_t trimmed_len = (size_t)(line_end - line);

        if (trimmed_len >= 6 && strncmp(line, "data: ", 6) == 0) {
            const char *data = line + 6;
            size_t data_len  = trimmed_len - 6;

            /* copy data value as NUL-terminated string */
            char *data_copy = malloc(data_len + 1);
            if (!data_copy) break;
            memcpy(data_copy, data, data_len);
            data_copy[data_len] = '\0';

            if (strcmp(data_copy, "[DONE]") == 0) {
                free(data_copy);
                break;
            }

            const char *chunk = llm_parse_sse_chunk(data_copy);
            free(data_copy);

            if (chunk) {
                /* skip empty deltas (role-only chunks) */
                if (chunk[0] != '\0') {
                    if (nchunks + 2 >= chunks_cap) {
                        chunks_cap *= 2;
                        const char **tmp = realloc(chunks, chunks_cap * sizeof(const char *));
                        if (!tmp) { free((void *)chunk); break; }
                        chunks = tmp;
                    }
                    chunks[nchunks++] = chunk;
                } else {
                    free((void *)chunk);
                }
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    free(resp);

    /* NULL-terminate the array */
    if (nchunks + 1 >= chunks_cap) {
        const char **tmp = realloc(chunks, (nchunks + 1) * sizeof(const char *));
        if (tmp) chunks = tmp;
    }
    chunks[nchunks] = NULL;

    TkLlmStream s;
    s.chunks  = chunks;
    s.nchunks = nchunks;
    s.is_err  = 0;
    s.err_msg = NULL;
    return s;
}

const char *llm_streamnext(TkLlmStream *s, uint64_t *idx)
{
    if (!s || !idx || s->is_err) return NULL;
    if (*idx >= s->nchunks) return NULL;
    return s->chunks[(*idx)++];
}

TkLlmResp llm_complete(TkLlmClient *c, const char *prompt, double temperature)
{
    TkLlmMsg msg;
    msg.role    = "user";
    msg.content = prompt;
    return llm_chat(c, &msg, 1, temperature);
}

uint64_t llm_countokens(TkLlmClient *c, const char *text)
{
    (void)c; /* client unused; approximation is model-independent */
    if (!text) return 0;
    return (uint64_t)(strlen(text) / 4);
}

/* -----------------------------------------------------------------------
 * Story 32.1.1 — Retry backoff
 * ----------------------------------------------------------------------- */

void llm_retry_backoff(TkLlmClient *client, uint64_t base_delay_ms, uint64_t max_retries)
{
    if (!client) return;
    client->retry_base_delay_ms = base_delay_ms;
    client->retry_max_retries   = max_retries;
}

/* backoff_sleep_ms — sleep for ms milliseconds using nanosleep. */
static void backoff_sleep_ms(uint64_t ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000L);
    nanosleep(&ts, NULL);
}

/* http_post_with_backoff — like http_post but honours the client's
 * retry_base_delay_ms / retry_max_retries fields.  Also retries on
 * HTTP 429 in addition to 5xx.
 * Returns the response body (caller frees) or NULL on permanent failure.
 * Sets *http_status_out to the final HTTP status code. */
static char *http_post_with_backoff(TkLlmClient *c,
                                     const char *host, const char *port,
                                     const char *path,  const char *body,
                                     int *http_status_out)
{
    uint32_t timeout = c->timeout_ms ? c->timeout_ms : 30000;

    /* determine retry parameters */
    uint64_t base_delay  = c->retry_base_delay_ms;
    uint64_t max_retries = c->retry_max_retries
                             ? c->retry_max_retries
                             : (c->max_retries ? (uint64_t)c->max_retries : 3u);

    char *resp_body  = NULL;
    int   http_status = 0;

    for (uint64_t attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0 && base_delay > 0) {
            /* exponential backoff: delay = base * 2^(attempt-1), cap 30 000 ms */
            uint64_t delay = base_delay;
            uint64_t shift = attempt - 1u;
            /* avoid overflow: cap shift at 14 (2^14 = 16384) */
            if (shift > 14u) shift = 14u;
            delay = base_delay << shift;
            if (delay > 30000u) delay = 30000u;
            backoff_sleep_ms(delay);
        } else if (attempt > 0) {
            /* no backoff configured — keep the original 1-second pause */
            backoff_sleep_ms(1000);
        }

        free(resp_body);
        resp_body = http_post(host, port, path, c->api_key, body, timeout, &http_status);
        if (!resp_body) continue;

        /* retry on 429 (rate-limit) and 5xx server errors */
        if (http_status == 429) continue;
        if (http_status >= 500 && http_status < 600) continue;
        break;
    }

    if (http_status_out) *http_status_out = http_status;
    return resp_body;
}

/* -----------------------------------------------------------------------
 * Story 32.1.1 — Embeddings
 * ----------------------------------------------------------------------- */

/* err_f64 — build a failed F64ArrayLlmResult. */
static F64ArrayLlmResult err_f64(const char *msg)
{
    F64ArrayLlmResult r;
    memset(&r, 0, sizeof(r));
    r.is_err  = 1;
    r.err_msg = strdup_safe(msg);
    return r;
}

/* err_f64s — build a failed F64ArraysLlmResult. */
static F64ArraysLlmResult err_f64s(const char *msg)
{
    F64ArraysLlmResult r;
    memset(&r, 0, sizeof(r));
    r.is_err  = 1;
    r.err_msg = strdup_safe(msg);
    return r;
}

/*
 * build_embedding_request — produce JSON for an /embeddings POST.
 * input_json must be a pre-built JSON value (quoted string or array).
 * Returns heap-allocated string (caller frees) or NULL on error.
 */
static char *build_embedding_request(const char *model, const char *input_json)
{
    /* {"model":"...","input":...} */
    size_t model_len = model ? strlen(model) : 0;
    size_t input_len = input_json ? strlen(input_json) : 2; /* "null" fallback */
    size_t cap = model_len * 2 + input_len + 64;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    int n = snprintf(buf, cap, "{\"model\":\"%s\",\"input\":%s}",
                     model ? model : "", input_json ? input_json : "null");
    if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
    return buf;
}

/*
 * parse_embedding_array — parse the float array starting at *pp (after '[').
 * Advances *pp past the closing ']'.
 * Returns heap-allocated double array via out_data/out_len; caller frees.
 * Returns 0 on success, -1 on error.
 */
static int parse_embedding_array(const char *p, double **out_data, uint64_t *out_len)
{
    /* skip to '[' */
    while (*p && *p != '[') p++;
    if (*p != '[') return -1;
    p++; /* skip '[' */

    size_t cap = 512;
    double *arr = malloc(cap * sizeof(double));
    if (!arr) return -1;
    uint64_t count = 0;

    while (*p) {
        /* skip whitespace and commas */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']') break;
        if (*p == '\0') break;

        /* read a number */
        char *end = NULL;
        double val = strtod(p, &end);
        if (end == p) break; /* not a number */
        p = end;

        if (count >= cap) {
            cap *= 2;
            double *tmp = realloc(arr, cap * sizeof(double));
            if (!tmp) { free(arr); return -1; }
            arr = tmp;
        }
        arr[count++] = val;
    }

    *out_data = arr;
    *out_len  = count;
    return 0;
}

/*
 * find_nth_embedding — locate the nth object in response["data"] array and
 * extract its "embedding" float array.
 * Returns 0 on success (caller frees *out_data), -1 on error.
 */
static int find_nth_embedding(const char *json, uint64_t n,
                               double **out_data, uint64_t *out_len)
{
    /* find "data": [ */
    const char *data_key = strstr(json, "\"data\":");
    if (!data_key) return -1;
    const char *arr_start = strchr(data_key, '[');
    if (!arr_start) return -1;
    arr_start++; /* skip '[' */

    /* walk through n objects to find the nth */
    const char *p = arr_start;
    uint64_t obj_idx = 0;
    while (*p) {
        /* skip whitespace and commas */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '{') break;

        if (obj_idx == n) {
            /* found our object — search for "embedding": within it */
            const char *emb_key = strstr(p, "\"embedding\":");
            if (!emb_key) return -1;
            emb_key += strlen("\"embedding\":");
            while (*emb_key == ' ' || *emb_key == '\t') emb_key++;
            return parse_embedding_array(emb_key, out_data, out_len);
        }

        /* skip over this object: find matching closing brace */
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        obj_idx++;
    }
    return -1;
}

F64ArrayLlmResult llm_embedding(TkLlmClient *client, const char *text)
{
    if (!client) return err_f64("null client");
    if (!text)   return err_f64("null text");

    /* reject HTTPS */
    if (client->base_url && strncmp(client->base_url, "https://", 8) == 0)
        return err_f64("HTTPS requires TLS support (not compiled in)");

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(client->base_url, "/embeddings", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0)
        return err_f64("failed to parse base_url");

    /* build JSON input: a quoted escaped string */
    size_t text_len = strlen(text);
    size_t esc_cap  = text_len * 6 + 4;
    char *esc_text  = malloc(esc_cap);
    if (!esc_text) return err_f64("out of memory");
    if (json_escape(text, esc_text, esc_cap) < 0) {
        free(esc_text);
        return err_f64("json_escape failed");
    }

    /* wrap in quotes: "\"<escaped>\"" */
    size_t quoted_cap = strlen(esc_text) + 4;
    char *quoted = malloc(quoted_cap);
    if (!quoted) { free(esc_text); return err_f64("out of memory"); }
    snprintf(quoted, quoted_cap, "\"%s\"", esc_text);
    free(esc_text);

    char *body = build_embedding_request(client->model, quoted);
    free(quoted);
    if (!body) return err_f64("failed to build embedding request");

    int http_status = 0;
    char *resp_body = http_post_with_backoff(client, host, port, path, body, &http_status);
    free(body);

    if (!resp_body)
        return err_f64("HTTP request failed");

    if (http_status >= 400) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", http_status);
        free(resp_body);
        return err_f64(errbuf);
    }

    double   *arr = NULL;
    uint64_t  arr_len = 0;
    if (find_nth_embedding(resp_body, 0, &arr, &arr_len) != 0) {
        free(resp_body);
        return err_f64("failed to parse embedding from response");
    }
    free(resp_body);

    F64ArrayLlmResult r;
    memset(&r, 0, sizeof(r));
    r.ok.data = arr;
    r.ok.len  = arr_len;
    r.is_err  = 0;
    return r;
}

F64ArraysLlmResult llm_embeddings_batch(TkLlmClient *client,
                                          const char *const *texts, uint64_t n)
{
    if (!client) return err_f64s("null client");
    if (!texts || n == 0) return err_f64s("empty texts array");

    /* reject HTTPS */
    if (client->base_url && strncmp(client->base_url, "https://", 8) == 0)
        return err_f64s("HTTPS requires TLS support (not compiled in)");

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(client->base_url, "/embeddings", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0)
        return err_f64s("failed to parse base_url");

    /* build JSON array of escaped strings */
    /* estimate size: sum of (len*6+4) per text + brackets + commas */
    size_t arr_cap = 4;
    for (uint64_t i = 0; i < n; i++) {
        arr_cap += texts[i] ? (strlen(texts[i]) * 6 + 4) : 6;
    }
    char *arr_json = malloc(arr_cap);
    if (!arr_json) return err_f64s("out of memory");

    int pos = 0;
    arr_json[pos++] = '[';

    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) arr_json[pos++] = ',';
        arr_json[pos++] = '"';

        const char *src = texts[i] ? texts[i] : "";
        size_t src_len  = strlen(src);
        size_t esc_cap2 = src_len * 6 + 4;
        char *esc = malloc(esc_cap2);
        if (!esc) { free(arr_json); return err_f64s("out of memory"); }
        int esc_len = json_escape(src, esc, esc_cap2);
        if (esc_len < 0) { free(esc); free(arr_json); return err_f64s("json_escape failed"); }

        /* ensure arr_json has room */
        if ((size_t)pos + (size_t)esc_len + 4 >= arr_cap) {
            arr_cap = (size_t)pos + (size_t)esc_len + 64;
            char *tmp = realloc(arr_json, arr_cap);
            if (!tmp) { free(esc); free(arr_json); return err_f64s("out of memory"); }
            arr_json = tmp;
        }
        memcpy(arr_json + pos, esc, (size_t)esc_len);
        pos += esc_len;
        free(esc);

        arr_json[pos++] = '"';
    }
    arr_json[pos++] = ']';
    arr_json[pos]   = '\0';

    char *body = build_embedding_request(client->model, arr_json);
    free(arr_json);
    if (!body) return err_f64s("failed to build embedding request");

    int http_status = 0;
    char *resp_body = http_post_with_backoff(client, host, port, path, body, &http_status);
    free(body);

    if (!resp_body) return err_f64s("HTTP request failed");

    if (http_status >= 400) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", http_status);
        free(resp_body);
        return err_f64s(errbuf);
    }

    F64Array *results = malloc(n * sizeof(F64Array));
    if (!results) { free(resp_body); return err_f64s("out of memory"); }

    for (uint64_t i = 0; i < n; i++) {
        double   *arr = NULL;
        uint64_t  arr_len = 0;
        if (find_nth_embedding(resp_body, i, &arr, &arr_len) != 0) {
            /* free previously allocated entries */
            for (uint64_t j = 0; j < i; j++) free((void *)results[j].data);
            free(results);
            free(resp_body);
            return err_f64s("failed to parse embedding from response");
        }
        results[i].data = arr;
        results[i].len  = arr_len;
    }
    free(resp_body);

    F64ArraysLlmResult r;
    memset(&r, 0, sizeof(r));
    r.data   = results;
    r.len    = n;
    r.is_err = 0;
    return r;
}

/* -----------------------------------------------------------------------
 * Story 32.1.2 — JSON mode, usage tracking, vision
 * ----------------------------------------------------------------------- */

LlmUsage llm_usage(TkLlmClient *client)
{
    LlmUsage u;
    memset(&u, 0, sizeof(u));
    if (!client) return u;
    u.input_tokens  = client->total_input_tokens;
    u.output_tokens = client->total_output_tokens;
    return u;
}

/*
 * build_request_extra — like llm_build_request but appends extra_json
 * (a JSON fragment such as "\"response_format\":{\"type\":\"json_object\"}") to the
 * top-level object before the closing brace.
 * Returns heap-allocated string (caller frees) or NULL on error.
 */
static const char *build_request_extra(TkLlmMsg *msgs, uint64_t nmsgs,
                                        const char *model, double temperature,
                                        const char *extra_json)
{
    const char *base = llm_build_request(msgs, nmsgs, model, temperature, 0);
    if (!base) return NULL;
    if (!extra_json || extra_json[0] == '\0') return base;

    /* base ends with '}'; we want to insert ,extra_json before it */
    size_t base_len  = strlen(base);
    size_t extra_len = strlen(extra_json);
    size_t new_cap   = base_len + extra_len + 4;
    char *buf = malloc(new_cap);
    if (!buf) { free((void *)base); return NULL; }

    /* copy everything except the trailing '}' */
    memcpy(buf, base, base_len - 1);
    free((void *)base);

    int n = snprintf(buf + base_len - 1, new_cap - (base_len - 1),
                     ",%s}", extra_json);
    if (n < 0 || (size_t)n >= new_cap - (base_len - 1)) {
        free(buf); return NULL;
    }
    return buf;
}

TkLlmResp llm_json_mode(TkLlmClient *client, TkLlmMsg *messages, uint64_t n)
{
    if (!client) return err_resp("null client");

    /* reject HTTPS */
    if (client->base_url && strncmp(client->base_url, "https://", 8) == 0)
        return err_resp("HTTPS requires TLS support (not compiled in)");

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(client->base_url, "/chat/completions", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0)
        return err_resp("failed to parse base_url");

    /*
     * Detect provider: if base_url contains "ollama" or "11434" use Ollama
     * format ("format":"json"), otherwise use OpenAI format.
     */
    int is_ollama = (client->base_url &&
                     (strstr(client->base_url, "11434") != NULL ||
                      strstr(client->base_url, "ollama") != NULL));

    const char *extra = is_ollama
        ? "\"format\":\"json\""
        : "\"response_format\":{\"type\":\"json_object\"}";

    const char *body = build_request_extra(messages, n, client->model, 0.7, extra);
    if (!body) return err_resp("failed to build request JSON");

    int http_status = 0;
    char *resp_body = http_post_with_backoff(client, host, port, path, body, &http_status);
    free((void *)body);

    if (!resp_body) return err_resp("HTTP request failed after retries");

    if (http_status >= 400) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", http_status);
        free(resp_body);
        return err_resp(errbuf);
    }

    const char *msg_start = strstr(resp_body, "\"message\":");
    char *content = NULL;
    if (msg_start) content = find_json_string(msg_start, "content");
    if (!content)  content = find_json_string(resp_body, "content");

    uint64_t prompt_tokens     = find_json_uint64(resp_body, "prompt_tokens");
    uint64_t completion_tokens = find_json_uint64(resp_body, "completion_tokens");
    free(resp_body);

    client->total_input_tokens  += prompt_tokens;
    client->total_output_tokens += completion_tokens;

    TkLlmResp r;
    memset(&r, 0, sizeof(r));
    r.content       = content ? content : strdup_safe("");
    r.input_tokens  = prompt_tokens;
    r.output_tokens = completion_tokens;
    r.is_err        = 0;
    r.err_msg       = NULL;
    return r;
}

/*
 * build_vision_request — build a chat completion request where the user
 * message content may contain an image URL.
 *
 * If a message's content begins with "http" or "data:", the content is
 * treated as an image URL/data URI and the user message is encoded as:
 *   {"role":"user","content":[{"type":"image_url","image_url":{"url":"..."}},
 *                              {"type":"text","text":""}]}
 * All other messages use the standard string content format.
 *
 * Returns heap-allocated JSON string (caller frees) or NULL on error.
 */
static const char *build_vision_request(TkLlmMsg *msgs, uint64_t nmsgs,
                                          const char *model)
{
    /* estimate generous upper bound */
    size_t cap = 256 + (model ? strlen(model) : 0);
    for (uint64_t i = 0; i < nmsgs; i++) {
        cap += 64;
        if (msgs[i].role)    cap += strlen(msgs[i].role)    * 2 + 4;
        if (msgs[i].content) cap += strlen(msgs[i].content) * 6 + 128;
    }

    char *buf = malloc(cap);
    if (!buf) return NULL;

    int pos = 0;

#define VAPPEND(fmt, ...) \
    do { int _n = snprintf(buf + pos, cap - (size_t)pos, fmt, __VA_ARGS__); \
         if (_n < 0 || (size_t)_n >= cap - (size_t)pos) { free(buf); return NULL; } \
         pos += _n; } while (0)
#define VAPPENDC(c) \
    do { if ((size_t)pos + 2 >= cap) { free(buf); return NULL; } \
         buf[pos++] = (c); buf[pos] = '\0'; } while (0)

    VAPPEND("{\"model\":\"%s\",\"messages\":[", model ? model : "");

    for (uint64_t i = 0; i < nmsgs; i++) {
        if (i > 0) VAPPENDC(',');
        VAPPEND("{\"role\":\"%s\",\"content\":", msgs[i].role ? msgs[i].role : "user");

        const char *content = msgs[i].content ? msgs[i].content : "";
        int is_image = (strncmp(content, "http", 4) == 0 ||
                        strncmp(content, "data:", 5) == 0);

        if (is_image) {
            /* vision array format */
            size_t clen     = strlen(content);
            size_t esc_cap2 = clen * 6 + 4;
            char *esc = malloc(esc_cap2);
            if (!esc) { free(buf); return NULL; }
            if (json_escape(content, esc, esc_cap2) < 0) { free(esc); free(buf); return NULL; }

            /* ensure buf has room */
            size_t needed = strlen(esc) + 128;
            if ((size_t)pos + needed >= cap) {
                cap = (size_t)pos + needed + 256;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(esc); free(buf); return NULL; }
                buf = tmp;
            }
            int n2 = snprintf(buf + pos, cap - (size_t)pos,
                "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}},"
                "{\"type\":\"text\",\"text\":\"\"}]", esc);
            free(esc);
            if (n2 < 0 || (size_t)n2 >= cap - (size_t)pos) { free(buf); return NULL; }
            pos += n2;
        } else {
            /* standard string content */
            size_t clen     = strlen(content);
            size_t esc_cap2 = clen * 6 + 4;
            char *esc = malloc(esc_cap2);
            if (!esc) { free(buf); return NULL; }
            if (json_escape(content, esc, esc_cap2) < 0) { free(esc); free(buf); return NULL; }

            size_t esc_len = strlen(esc);
            if ((size_t)pos + esc_len + 4 >= cap) {
                cap = (size_t)pos + esc_len + 64;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(esc); free(buf); return NULL; }
                buf = tmp;
            }
            buf[pos++] = '"';
            memcpy(buf + pos, esc, esc_len);
            pos += (int)esc_len;
            buf[pos++] = '"';
            buf[pos]   = '\0';
            free(esc);
        }
        VAPPENDC('}');
    }

    VAPPEND("],\"temperature\":%.2f,\"stream\":false}", 0.7);

#undef VAPPEND
#undef VAPPENDC

    return buf;
}

TkLlmResp llm_vision(TkLlmClient *client, TkLlmMsg *messages, uint64_t n)
{
    if (!client) return err_resp("null client");

    /* reject HTTPS */
    if (client->base_url && strncmp(client->base_url, "https://", 8) == 0)
        return err_resp("HTTPS requires TLS support (not compiled in)");

    char host[256], port[16], path[512];
    int is_https = 0;
    if (parse_url(client->base_url, "/chat/completions", host, sizeof(host),
                  port, sizeof(port), path, sizeof(path), &is_https) != 0)
        return err_resp("failed to parse base_url");

    const char *body = build_vision_request(messages, n, client->model);
    if (!body) return err_resp("failed to build vision request JSON");

    int http_status = 0;
    char *resp_body = http_post_with_backoff(client, host, port, path, body, &http_status);
    free((void *)body);

    if (!resp_body) return err_resp("HTTP request failed after retries");

    if (http_status >= 400) {
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", http_status);
        free(resp_body);
        return err_resp(errbuf);
    }

    const char *msg_start = strstr(resp_body, "\"message\":");
    char *content = NULL;
    if (msg_start) content = find_json_string(msg_start, "content");
    if (!content)  content = find_json_string(resp_body, "content");

    uint64_t prompt_tokens     = find_json_uint64(resp_body, "prompt_tokens");
    uint64_t completion_tokens = find_json_uint64(resp_body, "completion_tokens");
    free(resp_body);

    client->total_input_tokens  += prompt_tokens;
    client->total_output_tokens += completion_tokens;

    TkLlmResp r;
    memset(&r, 0, sizeof(r));
    r.content       = content ? content : strdup_safe("");
    r.input_tokens  = prompt_tokens;
    r.output_tokens = completion_tokens;
    r.is_err        = 0;
    r.err_msg       = NULL;
    return r;
}
