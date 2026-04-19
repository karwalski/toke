/*
 * fuzz_http_parse.c — libFuzzer entry point for HTTP request parsing.
 *
 * Embeds a copy of the parse_request logic from http.c to test it
 * in isolation without requiring OpenSSL or networking dependencies.
 *
 * Build:  make fuzz-http-parse   (requires clang with -fsanitize=fuzzer)
 * Story:  57.4.6
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Minimal struct matching http.c's Req for parsing */
typedef struct {
    const char *key;
    const char *val;
} FuzzStrPair;

typedef struct {
    char *method;
    char *path;
    char *body;
    struct { FuzzStrPair *data; uint64_t len; } headers;
} FuzzReq;

static FuzzReq fuzz_parse_request(const char *raw)
{
    FuzzReq req;
    memset(&req, 0, sizeof(req));
    if (!raw) return req;

    char *buf = malloc(strlen(raw) + 1);
    if (!buf) return req;
    strcpy(buf, raw);

    char *nl = strstr(buf, "\r\n");
    if (!nl) { free(buf); return req; }
    *nl = '\0';

    char *sp1 = strchr(buf, ' ');
    if (!sp1) { free(buf); return req; }
    *sp1 = '\0';
    req.method = strdup(buf);

    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2) *sp2 = '\0';
    req.path = strdup(sp1 + 1);

    FuzzStrPair *hdrs = malloc(64 * sizeof(FuzzStrPair));
    int hc = 0;
    char *p = nl + 2;
    while (1) {
        char *e = strstr(p, "\r\n");
        if (!e || e == p) break;
        *e = '\0';
        char *col = strchr(p, ':');
        if (col && hc < 64) {
            *col = '\0';
            hdrs[hc].key = strdup(p);
            hdrs[hc].val = strdup(col + 1 + (*(col + 1) == ' ' ? 1 : 0));
            hc++;
        }
        p = e + 2;
    }
    req.headers.data = hdrs;
    req.headers.len = (uint64_t)hc;

    const char *bs = strstr(raw, "\r\n\r\n");
    req.body = bs ? strdup(bs + 4) : strdup("");

    free(buf);
    return req;
}

static void fuzz_free_req(FuzzReq *req)
{
    free(req->method);
    free(req->path);
    free(req->body);
    for (uint64_t i = 0; i < req->headers.len; i++) {
        free((char *)req->headers.data[i].key);
        free((char *)req->headers.data[i].val);
    }
    free(req->headers.data);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Limit input size */
    if (size > 8192) return 0;

    /* Null-terminate */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    FuzzReq req = fuzz_parse_request(input);
    fuzz_free_req(&req);
    free(input);
    return 0;
}
