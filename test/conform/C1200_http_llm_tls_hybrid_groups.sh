#!/usr/bin/env bash
# C1200_http_llm_tls_hybrid_groups.sh — every std.http and std.llm TLS
# context offers the post-quantum hybrid X25519MLKEM768 by its OWN explicit
# group list, falls back cleanly to classical groups when the library lacks
# the hybrid, and the negotiated group is observable (story 122.15).
#
# THE GAP.  The std.http server contexts (http_tls_ctx_new, the SNI contexts
# of http_tls_add_sni, http_tls_ctx_new_config), the std.http client and the
# std.llm client created their SSL_CTX and never set a group list.  On
# OpenSSL 3.5+ the library default happens to prefer X25519MLKEM768, so the
# traffic was hybrid by inheritance only: a system openssl.cnf `Groups` line
# silently made it classical, and nothing could see that.  Now each context
# calls tk_tls_set_groups() (tk_tls_groups.h) -- the same hybrid list with the
# same classical retry std.tls's build_ssl_ctx uses (TK_TLS_GROUPS_HYBRID /
# TK_TLS_GROUPS_CLASSICAL in tls.h, 122.5) -- and TK_TLS_TRACE_GROUP=1 makes
# each context and each completed handshake write one stderr line naming the
# list applied and the group negotiated.
#
# HOW IT IS SHOWN.  One C driver, compiled from http.c and llm.c three ways
# (as C1180 does for tls.c):
#   A. as shipped.  Every server context (plain, SNI, config) negotiates
#      X25519MLKEM768 with a raw OpenSSL peer; a config that names its own
#      curves still wins; the http and llm clients negotiate X25519MLKEM768
#      with a raw OpenSSL server, which reports the group itself -- the
#      trace is checked against that independent observation.
#   A under OPENSSL_CONF with `Groups = x25519` (the inheritance hazard).
#      Control: an unlisted bare OpenSSL client IS forced to x25519 by that
#      file.  The toke server and both toke clients still negotiate
#      X25519MLKEM768: their explicit list, not the library default, decides.
#   B. the hybrid list names a group this library does not know (what an
#      OpenSSL < 3.5 sees): every context still builds, says `classical`,
#      negotiates x25519, and carries its request -- a clean fallback.
#   C. neither list usable: the server contexts are refused (NULL) and both
#      clients fail closed with an error -- so B passed because the fallback
#      ran, not because unknown names are ignored.
# Plus: with TK_TLS_TRACE_GROUP unset nothing is written (production is
# silent).
#
# NO CLOCK IN THE CLIENT CASES (127.395).  The raw OpenSSL server used to
# write its port to a file that the script polled for 5 s, and the script
# then polled 5 s for its `srvgroup=` line; worse, the bare client left
# without reading the response, so the raw server's response write could
# meet a reset connection and SIGPIPE killed it before it printed the group
# (measured: 5 of 40 runs, rc 141).  Now the raw server ignores SIGPIPE (it
# is the test's peer, not code under test), the bare client reads the
# response to EOF, and the script reads the raw server's stdout through a
# FIFO: a blocking read of the `port=` line it prints only after listen(),
# then, after the client has exited and a `Q` connection has queued behind
# the client's (accept order is connection order), a read to EOF.  Its
# alarm(120) is a hang guard, not a readiness budget.
#
# NO CLOCK IN THE SERVER CASES EITHER (127.398).  serve() used to pick a
# port with a throwaway python socket (free, then racy), poll 5 s for the
# driver's `CTX-` line, and leave the raw peer to retry connect() for 5 s;
# a server slower than that to bind would read as `peer: no connect`.  Now
# the std.http server is started on port 0 and its stderr comes through a
# FIFO: a blocking read of the `DIAGNOSTIC: kind=server_listening port=`
# line http.c prints only after listen() -- which also yields the port, so
# none is pre-picked -- or EOF, which means it exited first (a refused
# context: `server not ready`).  The peer then connects once (the port is
# already listening), and after the server is stopped its stderr is read
# to EOF, so every trace line it wrote is in srv_<tag>.err.  tcp_to() makes
# one connect() attempt; the alarm(120) in serve and peer is a hang guard.
#
# Negative control: C1200_STDLIB=<src/stdlib of f8ca5638, before 122.15> --
# see the commit for the measured result.
#
# The minimum TLS version is NOT changed here: these contexts still accept
# TLS 1.2, which has no post-quantum key exchange.  Raising it is an owner
# policy decision (row 122.15).
#
# Needs OpenSSL headers and the openssl CLI; without them a named SKIP.
#
# Story: 122.15

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
# Negative control: C1200_STDLIB=<a src/stdlib from before 122.15>.
S="${C1200_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1200_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT

echo "C1200: std.http / std.llm TLS contexts set the hybrid-first group list"
echo "----------------------------------------------------------------------"

OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew /opt/homebrew/opt/openssl /usr/local/opt/openssl; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
OPENSSL_BIN=""
for c in /opt/homebrew/bin/openssl /opt/homebrew/opt/openssl/bin/openssl /usr/local/opt/openssl/bin/openssl openssl; do
    if command -v "$c" >/dev/null 2>&1; then OPENSSL_BIN="$c"; break; fi
done
if [ -z "${OSSL_LIBS}" ] || [ -z "${OPENSSL_BIN}" ]; then
    echo "SKIP: no OpenSSL headers or openssl CLI on this host"
    echo "CONFORM-SKIP: OpenSSL not installed"
    exit 0
fi

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
expect() {   # name got want
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1"; echo "        expected: $3"; echo "        got:      $2"; fi
}

cd "${WORK}" || exit 1
if ! "${OPENSSL_BIN}" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
        -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
        -keyout k.pem -out c.pem > cert.log 2>&1 || [ ! -s c.pem ]; then
    fail "test certificate: $(tail -2 cert.log)"; echo "C1200: ${PASS} passed, ${FAIL} failed"; exit 1
fi
# The inheritance hazard: a system configuration that makes every context
# without its own list classical-only.
cat > x25519.cnf <<'CNF'
openssl_conf = openssl_init
[openssl_init]
ssl_conf = ssl_sect
[ssl_sect]
system_default = system_default_sect
[system_default_sect]
Groups = x25519
CNF

cat > drv.c <<'C'
/* C1200 driver: one binary, six roles (argv[1]). */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "http.h"
#include "llm.h"
#include "capabilities.h"

/* The peer's own list: explicit, so an OPENSSL_CONF Groups line cannot
 * change what the PEER offers -- only what an unlisted toke context does. */
#define PEER_GROUPS "X25519MLKEM768:X25519"

static const char *gname(SSL *s) {
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
    const char *g = SSL_get0_group_name(s);
#else
    int nid = SSL_get_negotiated_group(s);
    const char *g = nid ? SSL_group_to_name(s, nid) : NULL;
#endif
    return g ? g : "(none)";
}

/* One connect(), no retry: every caller runs only after the script has
 * read the target's own after-listen() line, so the port is listening. */
static int tcp_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

static Res root(Req q) { (void)q; return http_Res_ok("c1200-ok"); }

/* serve PORT CERT KEY KIND -- a std.http TLS server, one worker.  The
 * script passes PORT 0 and learns the port from http.c's after-listen()
 * `DIAGNOSTIC: kind=server_listening port=` stderr line. */
static int do_serve(int port, const char *cert, const char *key, const char *kind) {
    alarm(120);                 /* hang guard only: the script never waits on a clock */
    TkTlsCtx *t = NULL;
    if (strcmp(kind, "plain") == 0 || strcmp(kind, "sni") == 0) {
        t = http_tls_ctx_new(cert, key);
        if (t && strcmp(kind, "sni") == 0 &&
            http_tls_add_sni(t, "sni.c1200.test", cert, key) != 0) {
            printf("SNI-FAIL\n"); return 3;
        }
    } else {
        TkTlsConfig c; memset(&c, 0, sizeof c);
        c.cert_path = cert; c.key_path = key; c.min_version = "1.2";
        c.session_tickets = 1;
        if (strcmp(kind, "config_curves") == 0) c.curves = "x25519";
        t = http_tls_ctx_new_config(&c);
    }
    if (!t) { printf("CTX-NULL\n"); return 3; }
    printf("CTX-OK\n");
    http_GET("/", root);
    TkHttpRouter *r = http_router_new();
    TkHttpErr e = http_serve_tls_workers(r, "127.0.0.1", (uint64_t)port, t, 1);
    printf("SERVE-RETURNED %d\n", (int)e);
    return 4;
}

/* peer PORT [SNI] -- a raw OpenSSL client against the std.http server */
static int do_peer(int port, const char *sni) {
    alarm(120);                 /* hang guard only */
    int fd = tcp_to(port);
    if (fd < 0) { printf("peer: no connect\n"); return 1; }
    SSL_CTX *x = SSL_CTX_new(TLS_client_method());
    if (SSL_CTX_set1_groups_list(x, PEER_GROUPS) != 1) { printf("peer: groups\n"); return 1; }
    SSL *s = SSL_new(x); SSL_set_fd(s, fd);
    if (sni) SSL_set_tlsext_host_name(s, sni);
    if (SSL_connect(s) != 1) { printf("peer: handshake failed\n"); return 1; }
    const char *req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    SSL_write(s, req, (int)strlen(req));
    char buf[4096]; int n, tot = 0;
    while ((n = SSL_read(s, buf + tot, (int)sizeof buf - 1 - tot)) > 0) tot += n;
    buf[tot] = 0;
    printf("group=%s body_ok=%d\n", gname(s), strstr(buf, "c1200-ok") != NULL);
    SSL_free(s); SSL_CTX_free(x); close(fd);
    return 0;
}

/* rawsrv CERT KEY -- a raw OpenSSL server for the toke clients.  Prints
 * "port=P" once it is listening, one srvgroup= line per TLS connection, and
 * exits at the first connection whose first byte is 'Q' (see `quit`). */
static int do_rawsrv(const char *cert, const char *key) {
    signal(SIGPIPE, SIG_IGN);   /* a client that has left is not this peer's failure */
    alarm(120);                 /* hang guard only: the script never waits on a clock */
    SSL_CTX *x = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_set1_groups_list(x, PEER_GROUPS) != 1 ||
        SSL_CTX_use_certificate_chain_file(x, cert) != 1 ||
        SSL_CTX_use_PrivateKey_file(x, key, SSL_FILETYPE_PEM) != 1) {
        printf("rawsrv: ctx\n"); return 1;
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) return 1;
    getsockname(ls, (struct sockaddr *)&a, &al);
    printf("port=%d\n", ntohs(a.sin_port)); fflush(stdout);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        char q;
        if (recv(c, &q, 1, MSG_PEEK) == 1 && q == 'Q') { close(c); break; }
        SSL *s = SSL_new(x); SSL_set_fd(s, c);
        if (SSL_accept(s) != 1) {
            printf("srvgroup=(handshake failed)\n"); fflush(stdout);
            SSL_free(s); close(c); continue;
        }
        /* read the request: headers, then Content-Length bytes */
        char buf[16384]; int tot = 0, n; char *eoh = NULL;
        while (!eoh && (n = SSL_read(s, buf + tot, (int)sizeof buf - 1 - tot)) > 0) {
            tot += n; buf[tot] = 0; eoh = strstr(buf, "\r\n\r\n");
        }
        long cl = 0; char *p = eoh ? strcasestr(buf, "Content-Length:") : NULL;
        if (p && p < eoh) cl = strtol(p + 15, NULL, 10);
        while (eoh && (long)(tot - (eoh + 4 - buf)) < cl &&
               (n = SSL_read(s, buf + tot, (int)sizeof buf - 1 - tot)) > 0) tot += n;
        const char *body = strncmp(buf, "POST", 4) == 0
            ? "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"c1200-llm\"}}],"
              "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1},\"model\":\"m\"}"
            : "c1200-get";
        char out[1024];
        int ol = snprintf(out, sizeof out,
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            body[0] == '{' ? "application/json" : "text/plain", strlen(body), body);
        SSL_write(s, out, ol);
        printf("srvgroup=%s\n", gname(s)); fflush(stdout);
        SSL_shutdown(s); SSL_free(s); close(c);
    }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char *av[] = { "drv", "--allow-net", NULL };
    tk_cap_init(2, av);
    if (argc < 2) return 2;
    const char *role = argv[1];
    if (strcmp(role, "serve") == 0 && argc == 6)
        return do_serve(atoi(argv[2]), argv[3], argv[4], argv[5]);
    if (strcmp(role, "peer") == 0 && argc >= 3)
        return do_peer(atoi(argv[2]), argc > 3 ? argv[3] : NULL);
    if (strcmp(role, "rawsrv") == 0 && argc == 4)
        return do_rawsrv(argv[2], argv[3]);
    if (strcmp(role, "quit") == 0 && argc == 3) {
        /* queued behind any client connection; tells rawsrv to exit */
        int fd = tcp_to(atoi(argv[2]));
        if (fd >= 0) { (void)write(fd, "Q", 1); close(fd); }
        return 0;
    }
    if (strcmp(role, "bareclient") == 0 && argc == 3) {
        /* no group list of its own: whatever the library default/config says */
        int fd = tcp_to(atoi(argv[2]));
        SSL_CTX *x = SSL_CTX_new(TLS_client_method());
        SSL *s = SSL_new(x); SSL_set_fd(s, fd);
        int ok = fd >= 0 && SSL_connect(s) == 1;
        if (ok) {
            char rb[4096];
            SSL_write(s, "GET / HTTP/1.1\r\n\r\n", 18);
            while (SSL_read(s, rb, (int)sizeof rb) > 0) {}   /* to EOF: leave politely */
        }
        printf("bare: %s %s\n", ok ? "ok" : "fail", ok ? gname(s) : "");
        return 0;
    }
    if (strcmp(role, "httpc") == 0 && argc == 3) {
        char url[128]; snprintf(url, sizeof url, "https://127.0.0.1:%s", argv[2]);
        HttpClient *c = http_client(url);
        HttpClientResp r = http_get(c, "/");
        if (r.is_err) printf("httpc: err %s\n", r.err_msg ? r.err_msg : "?");
        else printf("httpc: %llu %.*s\n", (unsigned long long)r.status,
                    (int)r.body_len, (const char *)r.body);
        return 0;
    }
    if (strcmp(role, "llmc") == 0 && argc == 3) {
        char url[128]; snprintf(url, sizeof url, "https://127.0.0.1:%s/v1", argv[2]);
        TkLlmClient *c = llm_client(url, "k-c1200", "m");
        c->max_retries = 1;
        TkLlmMsg m = { "user", "hi" };
        TkLlmResp r = llm_chat(c, &m, 1, 0.0);
        if (r.is_err) printf("llmc: err %s\n", r.err_msg ? r.err_msg : "?");
        else printf("llmc: ok %s\n", r.content ? r.content : "");
        return 0;
    }
    return 2;
}
C

CF=(-std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -g -O1 -Wno-deprecated-declarations -iquote "${S}" ${OSSL_CFLAGS})
# Everything http + llm link except http.c and llm.c, which each variant
# compiles itself with its own group-list macros.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" \
           --except http.c --except llm.c --objects http llm -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "C1200: ${PASS} passed, $((FAIL + 1)) failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

CLA="$(sed -n 's/^#define TK_TLS_GROUPS_CLASSICAL *"\(.*\)"$/\1/p' "${S}/tls.h")"
build() {   # name [extra -D flags...]
    local name="$1"; shift
    if ! "${CC}" "${CF[@]}" "$@" -o "drv_${name}" drv.c "${S}/http.c" "${S}/llm.c" "${LINK[@]}" \
            ${OSSL_LIBS} -lz -lpthread -lm > "cc_${name}.log" 2>&1; then
        fail "variant ${name}: driver does not build: $(grep -a -m2 -E 'undefined reference|error' "cc_${name}.log" | tr '\n' ' ')"
        return 1
    fi
}

# serve BIN KIND TAG [ENV...] -> starts the std.http server on port 0; SRV
# set, PORT set once it is listening, or empty if it exited first.
# No clock (127.398): its stderr comes through a FIFO on fd 4, read (into
# srv_<tag>.err) until the after-listen() line or EOF.  serve_end stops it
# and reads the rest to EOF.
serve() {
    local bin="$1" kind="$2" tag="$3"; shift 3
    local fifo="srv_${tag}.fifo" line
    PORT=""
    rm -f "${fifo}"; : > "srv_${tag}.err"; mkfifo "${fifo}"
    env "$@" "./${bin}" serve 0 c.pem k.pem "${kind}" > "srv_${tag}.out" 2> "${fifo}" &
    SRV=$!
    exec 4< "${fifo}"
    while IFS= read -r line <&4; do
        echo "${line}" >> "srv_${tag}.err"
        case "${line}" in
            "DIAGNOSTIC: kind=server_listening port="*) PORT="${line##*port=}"; break ;;
        esac
    done
}
serve_end() {   # TAG
    local line
    stop "${SRV}"; SRV=""
    while IFS= read -r line <&4; do echo "${line}" >> "srv_$1.err"; done
    exec 4<&-
}
# server_case BIN KIND TAG WANT_GROUP [SNI] [ENV...]: server + raw peer
server_case() {
    local bin="$1" kind="$2" tag="$3" want="$4" sni="$5"; shift 5
    serve "${bin}" "${kind}" "${tag}" "$@"
    local got
    if [ -n "${PORT}" ]; then
        got="$(env "$@" "./${bin}" peer "${PORT}" ${sni} 2>&1)"
    else
        got="server not ready: $(cat "srv_${tag}.out" "srv_${tag}.err" | tr '\n' ' ')"
    fi
    serve_end "${tag}"
    expect "${tag}: the peer sees ${want} and the response" "${got}" "group=${want} body_ok=1"
}
# client_case BIN ROLE TAG [ENV...]: raw server + toke client; prints
# "<server-observed group>|<client output>"
# No clock (127.395): the raw server's stdout comes through a FIFO.  The
# first read blocks until it prints port= (after listen) or exits (EOF: "not
# ready"); the last reads to EOF, which comes after `quit` because its
# connection queues behind the client's.
client_case() {
    local bin="$1" role="$2" tag="$3"; shift 3
    local fifo="raw_${tag}.fifo" line port="" out
    rm -f "${fifo}" "raw_${tag}.out"; mkfifo "${fifo}"
    env "$@" "./${bin}" rawsrv c.pem k.pem > "${fifo}" 2>&1 &
    SRV=$!
    exec 3< "${fifo}"
    while IFS= read -r line <&3; do
        echo "${line}" >> "raw_${tag}.out"
        case "${line}" in port=*) port="${line#port=}"; break ;; esac
    done
    if [ -n "${port}" ]; then
        out="$(env SSL_CERT_FILE="${WORK}/c.pem" "$@" "./${bin}" "${role}" "${port}" 2> "cli_${tag}.err")"
        "./${bin}" quit "${port}"
    else
        out="raw server not ready: $(tr '\n' ' ' < "raw_${tag}.out")"
    fi
    while IFS= read -r line <&3; do echo "${line}" >> "raw_${tag}.out"; done
    exec 3<&-
    wait "${SRV}" 2>/dev/null; SRV=""
    echo "$(sed -n 's/^srvgroup=//p' "raw_${tag}.out" | head -1)|${out}"
}
has() { grep -c -- "$2" "$1" 2>/dev/null | tr -d ' '; }

H=X25519MLKEM768
# ── A: as shipped ─────────────────────────────────────────────────────────
echo "  -- variant A (as shipped)"
if build A; then
    T=(TK_TLS_TRACE_GROUP=1)
    server_case drv_A plain A_plain "${H}" "" "${T[@]}"
    expect "A_plain: the trace names the hybrid list and the server's group" \
        "$(has srv_A_plain.err '^tk-tls-groups: hybrid$')/$(has srv_A_plain.err "^tk-tls-group: server ${H}$")" "1/1"
    server_case drv_A sni A_sni "${H}" sni.c1200.test "${T[@]}"
    expect "A_sni: the primary and the SNI context both got the hybrid list" \
        "$(has srv_A_sni.err '^tk-tls-groups: hybrid$')" "2"
    server_case drv_A config A_config "${H}" "" "${T[@]}"
    server_case drv_A config_curves A_curves x25519 "" "${T[@]}"
    expect "A_curves: a config that names its own curves (x25519) still wins over the default list" \
        "$(has srv_A_curves.err "^tk-tls-group: server x25519$")" "1"
    expect "A_httpc: std.http client negotiates ${H} (seen by the server) and gets its response" \
        "$(client_case drv_A httpc A_httpc "${T[@]}")" "${H}|httpc: 200 c1200-get"
    expect "A_httpc: the client trace agrees" "$(has cli_A_httpc.err "^tk-tls-group: client ${H}$")" "1"
    expect "A_llmc: std.llm client negotiates ${H} (seen by the server) and gets its reply" \
        "$(client_case drv_A llmc A_llmc "${T[@]}")" "${H}|llmc: ok c1200-llm"
    expect "A_llmc: the client trace agrees" "$(has cli_A_llmc.err "^tk-tls-group: client ${H}$")" "1"
    # production: no variable, no output
    server_case drv_A plain A_quiet "${H}" ""
    client_case drv_A httpc A_quiet >/dev/null
    expect "A_quiet: with TK_TLS_TRACE_GROUP unset nothing is written" \
        "$(cat srv_A_quiet.err cli_A_quiet.err | grep -c 'tk-tls-' | tr -d ' ')" "0"

    # ── A under a system config that forces x25519 ──────────────────────
    echo "  -- variant A under OPENSSL_CONF (Groups = x25519)"
    C=(OPENSSL_CONF="${WORK}/x25519.cnf")
    expect "control: that config forces an unlisted OpenSSL client to x25519" \
        "$(client_case drv_A bareclient conf_bare "${C[@]}" | cut -d'|' -f1)" "x25519"
    server_case drv_A plain conf_plain "${H}" "" "${C[@]}"
    expect "conf_httpc: the std.http client's own list beats the config" \
        "$(client_case drv_A httpc conf_httpc "${C[@]}")" "${H}|httpc: 200 c1200-get"
    expect "conf_llmc: the std.llm client's own list beats the config" \
        "$(client_case drv_A llmc conf_llmc "${C[@]}")" "${H}|llmc: ok c1200-llm"
fi

# ── B: the library does not know the hybrid ──────────────────────────────
echo "  -- variant B (hybrid absent from the library)"
if build B "-DTK_TLS_GROUPS_HYBRID=\"TkC1200NoSuchHybrid:${CLA}\""; then
    T=(TK_TLS_TRACE_GROUP=1)
    server_case drv_B plain B_plain x25519 "" "${T[@]}"
    expect "B_plain: the trace says the classical list was applied" \
        "$(has srv_B_plain.err '^tk-tls-groups: classical$')/$(has srv_B_plain.err '^tk-tls-group: server x25519$')" "1/1"
    server_case drv_B sni B_sni x25519 sni.c1200.test "${T[@]}"
    server_case drv_B config B_config x25519 "" "${T[@]}"
    expect "B_httpc: std.http client falls back to x25519 and still gets its response" \
        "$(client_case drv_B httpc B_httpc "${T[@]}")" "x25519|httpc: 200 c1200-get"
    expect "B_llmc: std.llm client falls back to x25519 and still gets its reply" \
        "$(client_case drv_B llmc B_llmc "${T[@]}")" "x25519|llmc: ok c1200-llm"
fi

# ── C: nothing usable -- fail closed ─────────────────────────────────────
echo "  -- variant C (no usable group list: negative control)"
if build C "-DTK_TLS_GROUPS_HYBRID=\"TkC1200NoSuchHybrid:TkC1200NoSuchClassical\"" \
           "-DTK_TLS_GROUPS_CLASSICAL=\"TkC1200NoSuchClassical\""; then
    for k in plain config; do
        serve drv_C "${k}" "C_${k}"
        serve_end "C_${k}"
        expect "C_${k}: the server context is refused" "$(head -1 "srv_C_${k}.out")" "CTX-NULL"
    done
    expect "C_httpc: the std.http client fails closed" \
        "$(client_case drv_C httpc C_httpc | cut -d'|' -f2)" "httpc: err TLS: no usable key-exchange group"
    expect "C_llmc: the std.llm client fails closed" \
        "$(client_case drv_C llmc C_llmc | cut -d'|' -f2)" "llmc: err TLS: no usable key-exchange group"
fi

echo ""
echo "C1200: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
