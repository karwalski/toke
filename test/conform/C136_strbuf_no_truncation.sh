#!/usr/bin/env bash
# C136_strbuf_no_truncation.sh — the shared string buffer, and the stdlib
# accumulators moved onto it, neither overflow nor truncate (story 122.2).
#
# THE DEFECT CLASS.  The stdlib carried a dozen private "append to a buffer"
# copies.  The `pos += snprintf(buf+pos, cap-pos, …)` ones wrote past the end
# once pos passed cap (cap-pos underflows); the 121.x fixes turned several into
# a clamp, which truncated silently instead.  Measured on main before 122.2:
#   toon    toon_from_json, one object with a 6000-byte value: ASan
#           heap-buffer-overflow (the single-object path never grew);
#   yaml    yaml_from_json of a 150-deep object: 2056 of 22950 bytes;
#   proxy   a 6000-byte body dropped, "Content-Length: 6000" still sent;
#   ws      ws_connect with a 1500-byte path: ASan stack-buffer-overflow
#           (snprintf's would-be length was the byte count sent);
#   acme    newOrder with 200 domains: ASan stack-buffer-overflow;
#   svg     a 2000-byte path "d" cut to fit a char[256];
#   canvas  a 3000-byte fillText cut, the next statement swallowed.
#
# WHAT THIS DRIVES (every binary built with -fsanitize=address,undefined and
# -fno-sanitize-recover, so a memory error is a hard failure):
#   strbuf.h itself — growth, appendf past any fixed size, insertn, dup of
#   bytes containing NUL, sticky OOM (reserve(SIZE_MAX)), take() == NULL
#   after OOM; then each module above at an input past its old limit,
#   asserting the output is COMPLETE, not merely that nothing crashed.
#   acme needs OpenSSL headers; without them that one case is a named SKIP
#   line (the others still run).
#
# Negative control: C136_STDLIB=<dir> points the suite at another stdlib
# tree.  A copy of src/stdlib/ with main's toon.c yaml.c proxy.c ws.c acme.c
# svg.c canvas.c put back fails every module case (ASan aborts or
# "incomplete").
#
# Story: 122.2

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C136_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c136_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C136: shared strbuf; stdlib accumulators neither overflow nor truncate"
echo "----------------------------------------------------------------------"

# case NAME SOURCE-FILE LINK-ARGS...  — build + run; the program prints
# "OK <what>" lines for passes and "BAD <what>" for failures.
case_run() {
    local name="$1" src="$2"; shift 2
    if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
            -o "${WORK}/${name}" "${src}" "$@" >"${WORK}/${name}.cc" 2>&1; then
        fail "${name}: build: $(grep -a -m3 -E 'error' "${WORK}/${name}.cc" | head -c 400)"
        return
    fi
    "${WORK}/${name}" >"${WORK}/${name}.out" 2>&1
    local rc=$?
    local ok bad
    ok=$(grep -a -c '^OK ' "${WORK}/${name}.out")
    bad=$(grep -a -c '^BAD ' "${WORK}/${name}.out")
    grep -a -E '^(OK|BAD) ' "${WORK}/${name}.out" | sed "s/^OK /  ok   ${name}: /; s/^BAD /  bad  ${name}: /"
    if [ "${rc}" -ne 0 ] && [ "${bad}" -eq 0 ]; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.out" | head -c 300)"
    elif [ "${bad}" -ne 0 ] || [ "${ok}" -eq 0 ]; then
        fail "${name}: ${bad} check(s) failed"
    else
        pass "${name}: ${ok} check(s)"
    fi
}

# ── strbuf.h ────────────────────────────────────────────────────────────────
cat > "${WORK}/t_strbuf.c" <<'C'
#include <stdint.h>
#include "strbuf.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    TkStrBuf b = TK_STRBUF_INIT;
    CHECK(strbuf_cstr(&b)[0] == '\0' && b.buf == NULL, "empty buffer reads as \"\"");
    for (int i = 0; i < 10000; i++) strbuf_putc(&b, (char)('a' + i % 26));
    CHECK(b.len == 10000 && b.buf[10000] == '\0' && b.buf[9999] == 'a' + 9999 % 26, "10000 putc grow and stay NUL-terminated");
    strbuf_free(&b);
    static char big[100001]; memset(big, 'x', 100000); big[100000] = 0;
    strbuf_appendf(&b, "<%s|%d>", big, 42);
    CHECK(b.len == 100005 && !strcmp(b.buf + 100001, "|42>") && b.buf[0] == '<', "appendf of 100 KB is not truncated");
    strbuf_free(&b);
    strbuf_append(&b, "ace"); strbuf_insertn(&b, 1, "b", 1); strbuf_insertn(&b, 3, "d", 1); strbuf_insertn(&b, 99, "!", 1);
    CHECK(!strcmp(strbuf_cstr(&b), "abcde!"), "insertn in the middle and past the end");
    strbuf_free(&b);
    strbuf_appendn(&b, "a\0b", 3);
    char *d = strbuf_dup(&b);
    CHECK(d && b.len == 3 && !memcmp(d, "a\0b", 4) && b.buf != NULL, "dup copies NUL bytes and leaves the buffer");
    free(d);
    CHECK(strbuf_reserve(&b, SIZE_MAX) == -1 && b.oom, "reserve(SIZE_MAX) fails and sets oom");
    size_t before = b.len; strbuf_append(&b, "more"); strbuf_appendf(&b, "%d", 1); strbuf_putc(&b, 'x');
    CHECK(b.len == before, "appends after OOM are no-ops");
    CHECK(strbuf_take(&b) == NULL && b.buf == NULL && !b.oom, "take after OOM is NULL and resets");
    char *e = strbuf_take(&b);
    CHECK(e && e[0] == '\0', "take of an empty buffer is an owned \"\"");
    free(e);
    return 0;
}
C
case_run strbuf "${WORK}/t_strbuf.c"

# ── toon ───────────────────────────────────────────────────────────────────
cat > "${WORK}/t_toon.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "toon.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    size_t n = 6000;
    char *v = malloc(n + 1); memset(v, 'A', n); v[n] = 0;
    char *j = malloc(n + 64); snprintf(j, n + 64, "{\"k\":\"%s\"}", v);
    const char *o = toon_from_json(j);
    CHECK(strstr(o, v) && !strcmp(o + strlen(o) - n - 1, v) == 0 && strlen(o) == n + 6, "one object, 6000-byte value, complete");
    snprintf(j, n + 64, "[{\"k\":\"%s\"}]", v);
    o = toon_from_json(j);
    CHECK(strstr(o, v) && o[strlen(o) - 1] == '\n', "array of one object, 6000-byte value, complete");
    char *t = malloc(3 * n + 64);
    int p = snprintf(t, 64, "data[1]{");
    memset(t + p, 'f', n); p += (int)n; p += snprintf(t + p, 8, "}:\n");
    memset(t + p, 'v', 2 * n); p += (int)(2 * n); t[p] = '\n'; t[p + 1] = 0;
    o = toon_to_json(t);
    CHECK(strlen(o) == n + 2 * n + 9 && !strcmp(o + strlen(o) - 3, "\"}]"), "toon_to_json with a 6000-byte field name, complete");
    return 0;
}
C
case_run toon "${WORK}/t_toon.c" "${S}/toon.c" "${S}/file.c"

# ── yaml ───────────────────────────────────────────────────────────────────
cat > "${WORK}/t_yaml.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yaml.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    int d = 150;
    char *j = malloc((size_t)d * 6 + 16), *q = j;
    for (int i = 0; i < d; i++) { memcpy(q, "{\"a\":", 5); q += 5; }
    *q++ = '7';
    for (int i = 0; i < d; i++) *q++ = '}';
    *q = 0;
    const char *o = yaml_from_json(j);
    size_t want = 0;                    /* "\n" + indent + "a: " per level, minus the trimmed first "\n", + "7" */
    for (int i = 0; i < d; i++) want += 1 + (size_t)(2 * i) + 3;
    want = want - 1 + 1;
    CHECK(strlen(o) == want && o[strlen(o) - 1] == '7', "150-deep object: every level and the final value");
    CHECK(strlen(o) > strlen(j) * 2 + 256, "the output really is past the old 2*len+256 cap");
    return 0;
}
C
case_run yaml "${WORK}/t_yaml.c" "${S}/yaml.c" "${S}/file.c" "${S}/str.c" "${S}/crypto.c" "${S}/tk_time.c" "${S}/encoding.c"

# ── proxy (static proxy_build_request, reached by including the file) ──────
cat > "${WORK}/t_proxy.c" <<C
#include "${S}/proxy.c"
C
cat >> "${WORK}/t_proxy.c" <<'C'
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    size_t n = 6000;
    char *body = malloc(n + 1); memset(body, 'Z', n); body[n] = 0;
    StrPair h[2] = { { "Accept", "*/*" }, { "Connection", "close" } };
    Req r; memset(&r, 0, sizeof r);
    r.method = "PUT"; r.path = "/p"; r.body = body; r.headers.data = h; r.headers.len = 2;
    char *s = proxy_build_request(&r, "be", 80, "ip");
    size_t L = s ? strlen(s) : 0;
    CHECK(s && strstr(s, "Content-Length: 6000\r\n") && L > n && !memcmp(s + L - n, body, n)
          && !memcmp(s + L - n - 4, "\r\n\r\n", 4), "6000-byte body forwarded whole, after the header block");
    CHECK(s && !strstr(s, "Connection: close"), "hop-by-hop header still dropped");
    free(s);
    static char longpath[5000]; longpath[0] = '/'; memset(longpath + 1, 'q', 4998); longpath[4999] = 0;
    r.path = longpath; r.body = NULL;
    s = proxy_build_request(&r, "be", 80, "ip");
    CHECK(s && strstr(s, longpath) && !strcmp(s + strlen(s) - 4, "\r\n\r\n"), "5000-byte path: request line whole, header block terminated");
    free(s);
    return 0;
}
C
case_run proxy "${WORK}/t_proxy.c" "${S}/http.c" "${S}/encoding.c" "${S}/str.c" "${S}/log.c" \
    "${S}/capabilities.c" "${S}/router.c" "${S}/ws.c" "${S}/crypto.c" "${S}/tk_time.c" -lz -lpthread

# ── ws_connect over loopback ───────────────────────────────────────────────
cat > "${WORK}/t_ws.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ws.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1) ||
        getsockname(ls, (struct sockaddr *)&a, &al)) { printf("BAD loopback listen\n"); return 1; }
    size_t n = 1500;
    char *url = malloc(n + 64);
    int k = snprintf(url, 64, "ws://127.0.0.1:%d/", ntohs(a.sin_port));
    memset(url + k, 'p', n); url[k + n] = 0;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { (void)ws_connect(url); _exit(0); }
    int c = accept(ls, NULL, NULL);
    char *got = calloc(1, 65536); size_t g = 0; ssize_t m;
    while (g < 65535 && (m = read(c, got + g, 65535 - g)) > 0) { g += (size_t)m; if (strstr(got, "\r\n\r\n")) break; }
    close(c);
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client exits cleanly (no ASan abort)");
    CHECK(strstr(got, url + k - 1) != NULL, "1500-byte path sent whole");
    CHECK(g >= 4 && !memcmp(got + g - 4, "\r\n\r\n", 4) && strstr(got, "Sec-WebSocket-Version: 13\r\n"), "request ends with its header block");
    return 0;
}
C
case_run ws "${WORK}/t_ws.c" "${S}/ws.c"

# ── acme (needs OpenSSL) ───────────────────────────────────────────────────
OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in "$(brew --prefix openssl@3 2>/dev/null)" /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
        if [ -n "${pfx}" ] && [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ]; then
    echo "  SKIP: acme case -- no OpenSSL headers on this host (the other cases ran)"
else
    cat > "${WORK}/t_acme.c" <<C
#include "${S}/acme.c"
C
    cat >> "${WORK}/t_acme.c" <<'C'
#include "capabilities.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    char *av[] = { "t", "--allow-net", NULL };
    tk_cap_init(2, av);
    enum { N = 200 };
    static char names[N][64]; const char *d[N];
    for (int i = 0; i < N; i++) {
        snprintf(names[i], 64, "host-%03d.a-rather-long-subdomain.example.test", i);
        d[i] = names[i];
    }
    AcmeClient ac; memset(&ac, 0, sizeof ac);
    ac.account_url = "http://127.0.0.1:9/acct"; ac.new_nonce_url = "/nonce"; ac.new_order_url = "/order";
    ac.client = http_client("http://127.0.0.1:9");
    /* Builds the identifiers first (the old overflow), then fails at the
     * nonce fetch: nothing listens on port 9. */
    CHECK(acme_order_certificate(&ac, d, N, "/nonexistent/c", "/nonexistent/k") == -1,
          "200-domain order: identifiers built, fails cleanly at the nonce fetch");
    return 0;
}
C
    # shellcheck disable=SC2086
    case_run acme "${WORK}/t_acme.c" -DTK_HAVE_OPENSSL ${OSSL_CFLAGS} "${S}/http.c" "${S}/http2.c" \
        "${S}/encoding.c" "${S}/str.c" "${S}/log.c" "${S}/capabilities.c" "${S}/router.c" "${S}/ws.c" \
        "${S}/crypto.c" "${S}/tk_time.c" ${OSSL_LIBS} -lz -lpthread
fi

# ── svg ────────────────────────────────────────────────────────────────────
cat > "${WORK}/t_svg.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svg.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    size_t n = 2000;
    char *pd = malloc(n + 1); strcpy(pd, "M0 0");
    for (size_t i = 4; i + 4 <= n; i += 4) memcpy(pd + i, " L1 ", 4);
    pd[n] = 0;
    TkSvgDoc *d = svg_doc(1, 1);
    svg_append(d, svg_path(pd, svg_style("red", "blue", 1)));
    char tid[400]; memset(tid, 'i', 399); tid[399] = 0;
    svg_animate(d, tid, "opacity", "0", "1", 1.0);
    const char *o = svg_render(d);
    char *want = malloc(n + 16); snprintf(want, n + 16, "d=\"%s\"", pd);
    CHECK(strstr(o, want) != NULL, "2000-byte path d rendered whole");
    CHECK(!strcmp(o + strlen(o) - 6, "</svg>") && strstr(o, "<animate "), "document closed; animate for a 399-byte id still placed");
    return 0;
}
C
case_run svg "${WORK}/t_svg.c" "${S}/svg.c" -lm

# ── canvas ─────────────────────────────────────────────────────────────────
cat > "${WORK}/t_canvas.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    size_t n = 3000;
    char *t = malloc(n + 1); memset(t, 'T', n); t[n] = 0;
    TkCanvas *c = canvas_new("x", 1, 1);
    canvas_fill_text(c, t, 1, 2, "f", "c");
    canvas_begin_path(c);
    canvas_fill_style(c, t);
    const char *js = canvas_to_js(c);
    CHECK(strstr(js, t) != NULL, "3000-byte text rendered whole");
    CHECK(strstr(js, "',1,2);\n_c.beginPath();\n") != NULL, "the next statement is still a statement");
    CHECK(canvas_op_count(c) == 3 && !strcmp(js + strlen(js) - 3, "';\n"), "op count and final statement intact");
    canvas_free(c);
    return 0;
}
C
case_run canvas "${WORK}/t_canvas.c" "${S}/canvas.c"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
