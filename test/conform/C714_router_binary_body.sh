#!/usr/bin/env bash
# C714_router_binary_body.sh -- std.router carries a response body as bytes,
# not a C string: a served binary file, a byte range of one and a gzip-encoded
# body all leave the router whole, and the ETag middleware hashes every byte
# (story 121.88).
#
# THE DEFECT, measured on main (eb4bdb5b): TkRouteResp.body was a bare
# `const char *` with no length, and everything downstream of a handler
# measured it with strlen() -- the class of defect 121.86 fixed in http.c:
#   - router_send_response (the writer of router.serve and router_handle_fd)
#     sent strlen(body) bytes with a Content-Length to match, so a static file
#     of bytes 0x00..0xFF went out as 0 bytes and a PNG as its 8-byte
#     signature;
#   - a byte range (206) of such a file had no length a caller could read;
#   - gzip_middleware compressed strlen(body) bytes and handed back a gzip
#     stream with no length, and a gzip stream always holds a 0x00 (byte 3,
#     the FLG field): a caller could recover 3 bytes of it;
#   - the ETag middleware hashed up to the first NUL, so two different binary
#     files that agree before their first 0x00 shared an ETag -- and a request
#     for one carrying the other's ETag in If-None-Match got 304 Not Modified.
#
# THE FIX.  TkRouteResp gains `body_len`, 0 meaning strlen(body) (so every
# zero-initialised response built from a string means what it meant), and
# router_resp_body_len() reads it.  router_static_serve and gzip_middleware
# set it; the writer, the gzip input and the ETag hash use it.
#
# WHAT IS ASSERTED, by a C harness under ASan/UBSan:
#   wire (router_static + router_handle_fd over a socketpair, a forked child
#   serving): bytes.bin (0x00..0xFF x 64, 16384 B) and a generated 4x4 PNG
#   arrive byte-identical with a Content-Length that counts every byte;
#   API (router_dispatch_ex): a 4 KiB text response through the gzip
#   middleware with Accept-Encoding: gzip carries Content-Encoding: gzip and
#   inflates whole to the original; the PNG through the same middleware is
#   not re-encoded and is whole; router_static_serve with Range bytes=0-99 of
#   bytes.bin is 100 bytes, identical; the ETag middleware gives a.bin and
#   b.bin (same length, same bytes before their first NUL) different ETags,
#   and b.bin with a.bin's ETag in If-None-Match is 200, not 304.
# The harness reads a body's length with router_resp_body_len() where
# router.h has it (TK_ROUTER_RESP_BODY_LEN), else with strlen() -- the only
# length main's API offered.
#
# Negative control: C714_STDLIB=<dir> builds the harness against another
# stdlib tree.  Against main's src/stdlib (eb4bdb5b) it is 4 passed, 9 failed:
# every byte and length case fails; the content-type and Content-Encoding
# checks pass, as they should.
#
# Story: 121.88
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C714_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C714: std.router serves binary, ranged and gzip bodies whole (121.88)"
echo "----------------------------------------------------------------------"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found (fixtures)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c714_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/pub"
python3 - "${WORK}/pub" <<'PY'
import struct, sys, zlib
d = sys.argv[1]
open(d + "/bytes.bin", "wb").write(bytes(range(256)) * 64)
def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
raw = b"".join(b"\x00" + b"".join(bytes([x * 60, y * 60, 0]) for x in range(4)) for y in range(4))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
open(d + "/img.png", "wb").write(png)
# same length, same bytes before the first NUL, different after it
open(d + "/a.bin", "wb").write(b"HDR\x00" + b"A" * 60)
open(d + "/b.bin", "wb").write(b"HDR\x00" + b"B" * 60)
PY

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "router.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <zlib.h>

#ifdef TK_ROUTER_RESP_BODY_LEN
#define BODYLEN(r) router_resp_body_len(r)
#else
#define BODYLEN(r) ((r).body ? strlen((r).body) : (size_t)0)  /* main: all there was */
#endif

static const char *dir;
static int bad;
static void report(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("%s ", ok ? "OK" : "BAD");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    if (!ok) bad++;
}

static unsigned char *slurp(const char *name, size_t *n)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { *n = 0; return NULL; }
    unsigned char *b = malloc(1 << 20);
    *n = fread(b, 1, 1 << 20, f);
    fclose(f);
    return b;
}

static const char *hdr(TkRouteResp r, const char *name, int last)
{
    const char *v = NULL;
    for (uint64_t i = 0; i < r.nheaders; i++)
        if (r.header_names && r.header_names[i] &&
            strcasecmp(r.header_names[i], name) == 0) {
            v = r.header_values[i];
            if (!last) break;
        }
    return v;
}

/* GET url through router_handle_fd in a forked child; compare with file. */
static void wire(TkRouter *r, const char *url, const char *file, const char *ct)
{
    size_t wl; unsigned char *want = slurp(file, &wl);
    int sv[2];
    if (!want || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { bad++; printf("BAD wire %s: setup\n", url); return; }
    char req[512];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\n\r\n", url);
    if (write(sv[0], req, (size_t)rl) != rl) { bad++; return; }
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); router_handle_fd(r, sv[1]); _exit(0); }
    close(sv[1]);
    size_t cap = 1 << 20, got = 0;
    unsigned char *buf = malloc(cap);
    ssize_t n;
    while ((n = read(sv[0], buf + got, cap - got)) > 0) got += (size_t)n;
    close(sv[0]);
    waitpid(pid, NULL, 0);
    unsigned char *sep = NULL;
    for (size_t i = 0; i + 3 < got; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { sep = buf + i; break; }
    if (!sep) { bad++; printf("BAD wire %s: no header block (%zu bytes)\n", url, got); free(buf); free(want); return; }
    *sep = '\0';
    const char *cl = strcasestr((char *)buf, "\r\nContent-Length:");
    size_t clv = cl ? (size_t)strtoull(cl + 17, NULL, 10) : (size_t)-1;
    int ctok = strcasestr((char *)buf, ct) != NULL;
    size_t bl = got - (size_t)(sep + 4 - buf);
    int same = bl == wl && memcmp(sep + 4, want, wl) == 0;
    report(same, "wire %s: %zu body bytes (want %zu), byte-identical", url, bl, wl);
    report(clv == wl, "wire %s: Content-Length %zu (want %zu)", url, clv, wl);
    report(ctok, "wire %s: content type %s", url, ct);
    free(buf); free(want);
}

static char big[4097];
static TkRouteResp text_handler(TkRouteCtx ctx) { (void)ctx; return router_resp_ok(big, "text/plain; charset=utf-8"); }

int main(int argc, char **argv)
{
    (void)argc;
    dir = argv[1];
    for (int i = 0; i < 4096; i++) big[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];

    /* ── wire: router_static served by router_handle_fd ── */
    TkRouter *r = router_new();
    router_static(r, "/s", dir);
    wire(r, "/s/bytes.bin", "bytes.bin", "application/octet-stream");
    wire(r, "/s/img.png", "img.png", "image/png");

    /* ── gzip middleware ── */
    TkRouter *g = router_new();
    router_get(g, "/t", text_handler);
    router_static(g, "/s", dir);
    router_use_gzip(g, 256);
    const char *hn[] = { "Accept-Encoding" }, *hv[] = { "gzip, deflate" };
    TkRouteResp t = router_dispatch_ex(g, "GET", "/t", NULL, NULL, hn, hv, 1);
    const char *ce = hdr(t, "Content-Encoding", 0);
    report(ce && strcmp(ce, "gzip") == 0, "gzip /t: Content-Encoding %s (want gzip)", ce ? ce : "(none)");
    {
        size_t zl = BODYLEN(t);
        unsigned char out[8192];
        z_stream zs; memset(&zs, 0, sizeof zs);
        int zr = inflateInit2(&zs, 15 + 16);
        zs.next_in = (Bytef *)(uintptr_t)t.body; zs.avail_in = (uInt)zl;
        zs.next_out = out; zs.avail_out = sizeof out;
        if (zr == Z_OK) zr = inflate(&zs, Z_FINISH);
        size_t ol = zs.total_out;
        inflateEnd(&zs);
        report(zr == Z_STREAM_END && ol == 4096 && memcmp(out, big, 4096) == 0,
               "gzip /t: %s %zu gzip bytes inflate to %zu (want 4096, identical)",
               zr == Z_STREAM_END ? "stream complete," : "stream TRUNCATED,", zl, ol);
    }
    TkRouteResp p = router_dispatch_ex(g, "GET", "/s/img.png", NULL, NULL, hn, hv, 1);
    {
        size_t wl; unsigned char *want = slurp("img.png", &wl);
        size_t pl = BODYLEN(p);
        report(p.status == 200 && !hdr(p, "Content-Encoding", 0), "gzip /s/img.png: %s (status %d, want 200)",
               hdr(p, "Content-Encoding", 0) ? "re-encoded" : "not re-encoded", p.status);
        report(pl == wl && memcmp(p.body, want, wl) == 0, "gzip /s/img.png: %zu bytes (want %zu, identical)", pl, wl);
        free(want);
    }

    /* ── a byte range ── */
    TkRouteResp rg = router_static_serve(dir, "bytes.bin", NULL, "bytes=0-99");
    {
        size_t wl; unsigned char *want = slurp("bytes.bin", &wl);
        size_t gl = BODYLEN(rg);
        report(rg.status == 206 && gl == 100 && memcmp(rg.body, want, 100) == 0,
               "range bytes=0-99: status %d, %zu bytes (want 206, 100, identical)", rg.status, gl);
        free(want);
    }

    /* ── ETag middleware over binary bodies ── */
    TkRouter *e = router_new();
    router_static(e, "/s", dir);
    router_use_etag(e);
    TkRouteResp ra = router_dispatch_ex(e, "GET", "/s/a.bin", NULL, NULL, NULL, NULL, 0);
    TkRouteResp rb = router_dispatch_ex(e, "GET", "/s/b.bin", NULL, NULL, NULL, NULL, 0);
    const char *ea = hdr(ra, "ETag", 1), *eb = hdr(rb, "ETag", 1);
    report(ea && eb && ea[0] == 'W' && strcmp(ea, eb) != 0,
           "etag: a.bin %s, b.bin %s (want two different weak ETags)", ea ? ea : "(none)", eb ? eb : "(none)");
    if (ea) {
        const char *in[] = { "If-None-Match" }, *iv[] = { ea };
        TkRouteResp c = router_dispatch_ex(e, "GET", "/s/b.bin", NULL, NULL, in, iv, 1);
        report(c.status == 200, "etag: b.bin with a.bin's ETag in If-None-Match -> %d (want 200)", c.status);
    }
    return bad ? 1 : 0;
}
EOF
if ! ASAN_OPTIONS=detect_leaks=0 "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" -lz >"${WORK}/h.cc" 2>&1; then
    fail "harness build: $(grep -a -m2 error "${WORK}/h.cc" | head -c 400)"
else
    ASAN_OPTIONS=detect_leaks=0 "${WORK}/h" "${WORK}/pub" >"${WORK}/h.out" 2>&1
    rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/h.out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/h.out" || fail "harness produced nothing: $(head -c 300 "${WORK}/h.out")"
    if grep -a -q -E 'Sanitizer|runtime error' "${WORK}/h.out"; then
        fail "sanitizer report: $(grep -a -m1 -E 'Sanitizer|runtime error' "${WORK}/h.out" | head -c 300)"
    elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/h.out"; then
        fail "harness exited ${rc}: $(tail -3 "${WORK}/h.out" | head -c 300)"
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
