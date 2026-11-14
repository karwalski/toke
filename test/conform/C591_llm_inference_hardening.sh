#!/usr/bin/env bash
# C591_llm_inference_hardening.sh -- std.llm transport, the MLX bridge
# requests, the storage probe's subprocess and vecstore's file loader
# (story 121.23; audit-120 inference.md).  Local servers only: every
# listener here is on this host, and nothing is sent anywhere else.
#
# WHAT IS ASSERTED, and what main (f605aec9) did
#   llm   INF-03  https:// works, with the chain verified against the trust
#                 store (SSL_CERT_FILE points it at a test CA) -- main refused
#                 every https:// URL ("not compiled in")
#         INF-03  a certificate from another CA, and a host name the
#                 certificate does not name, are refused before any request
#         INF-03  an API key is not sent over cleartext http:// to a
#                 non-loopback address (this host's own LAN address, so no
#                 packet leaves the machine) -- main sent "Bearer <key>"
#         INF-06  a CR/LF in the API key or base_url is refused -- main wrote
#                 it into the header block
#   e2e   INF-03  a toke program (llm.client/llm.chat) built by tkc reaches
#                 the https:// endpoint -- proves std.llm now links OpenSSL
#   mlx   INF-02  a prompt/text/model path carrying '"' reaches a mock bridge
#                 as the same string, with no field injected -- main
#                 interpolated them raw
#   infer INF-01  the diskutil probe passes the device as one argv element --
#                 main ran it through a shell, so  x'; touch F; '  created F
#   vec   INF-04  a .vecs header claiming 2^31 entries over a 20-entry file is
#                 refused -- main allocated 16 entries and wrote 20 (ASan
#                 heap-buffer-overflow)
#         INF-05  a failed allocation while replacing an entry leaves the
#                 entry intact, and the next flush works -- main left a NULL
#                 payload and crashed in strlen on flush
#
# Negative control: C591_STDLIB=<dir> points the suite at another stdlib tree
# (a copy of src/stdlib).  Main's llm.c, mlx.c and vecstore.c fail their
# cases; infer is controlled by putting the popen form back in a copy.
# Recorded in the 121.23 report.
#
# Story: 121.23
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C591_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c591_XXXXXX")"
PIDS=""
cleanup() { for p in ${PIDS}; do kill "${p}" 2>/dev/null; wait "${p}" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C591: std.llm transport + inference-stack hardening (121.23)"
echo "------------------------------------------------------------------------"

command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the local servers"; exit 0; }
OPENSSL_BIN=""
for c in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl openssl; do
    if command -v "${c}" >/dev/null 2>&1 && "${c}" version 2>/dev/null | grep -q '^OpenSSL 3'; then OPENSSL_BIN="${c}"; break; fi
done
OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ] || [ -z "${OPENSSL_BIN}" ]; then
    echo "CONFORM-SKIP: needs OpenSSL 3 headers and the openssl CLI (test CA)"
    exit 0
fi

# case NAME SOURCE-FILE LINK-ARGS... -- build + run; the program prints
# "OK <what>" / "BAD <what>" lines ("SKIP <what>" lines are echoed).
case_run() {
    local name="$1" src="$2"; shift 2
    # shellcheck disable=SC2086
    if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} ${CASE_CFLAGS:-} -iquote "${S}" -I"${S}" \
            -o "${WORK}/${name}" "${src}" "$@" >"${WORK}/${name}.cc" 2>&1; then
        fail "${name}: build: $(grep -a -m3 -E 'error' "${WORK}/${name}.cc" | head -c 400)"
        return
    fi
    (cd "${WORK}" && "./${name}") >"${WORK}/${name}.out" 2>&1
    local rc=$?
    local ok bad
    ok=$(grep -a -c '^OK ' "${WORK}/${name}.out")
    bad=$(grep -a -c '^BAD ' "${WORK}/${name}.out")
    grep -a -E '^(OK|BAD|SKIP) ' "${WORK}/${name}.out" | sed "s/^OK /  ok   ${name}: /; s/^BAD /  bad  ${name}: /; s/^SKIP /  skip ${name}: /"
    if [ "${rc}" -ne 0 ] && [ "${bad}" -eq 0 ]; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.out" | head -c 300)"
    elif [ "${bad}" -ne 0 ] || [ "${ok}" -eq 0 ]; then
        fail "${name}: ${bad} check(s) failed"
    else
        pass "${name}: ${ok} check(s)"
    fi
}

# ── test PKI: a CA, a server cert for DNS:localhost only, an unrelated CA ──
cd "${WORK}" || exit 1
{
"${OPENSSL_BIN}" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
    -subj "/CN=C591 test CA" -keyout ca.key -out ca.pem
"${OPENSSL_BIN}" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
    -subj "/CN=C591 other CA" -keyout other.key -out other_ca.pem
"${OPENSSL_BIN}" req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -subj "/CN=localhost" -keyout srv.key -out srv.csr
printf 'subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n' > ext.cnf
"${OPENSSL_BIN}" x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -days 2 -extfile ext.cnf -out srv.pem
} >pki.log 2>&1 || { fail "test PKI: $(tail -3 pki.log)"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }

# ── local servers ──────────────────────────────────────────────────────────
# srv.py MODE PORTFILE LOG: MODE tls = HTTPS on 127.0.0.1 (cert for
# localhost); plain = HTTP on 0.0.0.0 (so this host's LAN address reaches it);
# mlx = the bridge's /generate /embed /load.  Each request's Authorization
# header, a marker for any X-Injected/X-Evil header, and the raw body are
# appended to LOG, one JSON line per request.
cat > srv.py <<'PY'
import sys, json, ssl, http.server, socketserver
import socketserver as _ss
def _bind(self):  # 127.439 (Z205): HTTPServer.server_bind does a reverse-DNS getfqdn() that stalls for seconds on the macos-15-intel runner
    _ss.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
http.server.HTTPServer.server_bind = _bind
mode, portfile, log = sys.argv[1], sys.argv[2], sys.argv[3]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        rec = {"path": self.path, "auth": self.headers.get("Authorization", ""),
               "injected": any(k.lower() in ("x-injected", "x-evil") for k in self.headers.keys()),
               "body": raw}
        with open(log, "a") as f: f.write(json.dumps(rec) + "\n")
        if mode == "mlx":
            out = {"text": "mlx-ok", "embedding": [0.5, 0.5], "id": "m1"}
            b = json.dumps(out).encode(); ctype = "application/json"
        else:
            try: stream = json.loads(raw).get("stream")
            except Exception: stream = False
            if stream:
                b = ('data: {"choices":[{"delta":{"content":"%s-stream"}}]}\n\ndata: [DONE]\n\n' % mode).encode()
                ctype = "text/event-stream"
            else:
                b = json.dumps({"choices": [{"message": {"role": "assistant", "content": mode + "-reply"}}],
                                "usage": {"prompt_tokens": 1, "completion_tokens": 1}, "model": "m"}).encode()
                ctype = "application/json"
        self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.send_header("Connection", "close")
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; allow_reuse_address = True
if mode == "plain":
    s = S(("0.0.0.0", 0), H)
elif mode == "mlx":
    s = S(("127.0.0.1", int(sys.argv[4])), H)
else:
    s = S(("127.0.0.1", 0), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("srv.pem", "srv.key")
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
open(portfile, "w").write(str(s.server_address[1]))
s.serve_forever()
PY
start_srv() {  # MODE PORTFILE LOG [PORT]
    python3 srv.py "$@" >/dev/null 2>&1 &
    PIDS="${PIDS} $!"
    for _ in $(seq 1 100); do [ -s "$2" ] && return 0; sleep 0.05; done
    return 1
}
: > tls.log; : > plain.log
start_srv tls tls.port tls.log || { fail "https server did not start"; }
start_srv plain plain.port plain.log || { fail "plain server did not start"; }
TLS_PORT="$(cat tls.port 2>/dev/null)"; PLAIN_PORT="$(cat plain.port 2>/dev/null)"
LAN_IP=""
for ifc in en0 en1 eth0; do
    LAN_IP="$( (ipconfig getifaddr "${ifc}" 2>/dev/null || ip -4 -o addr show "${ifc}" 2>/dev/null | awk '{print $4}' | cut -d/ -f1) | head -1)"
    [ -n "${LAN_IP}" ] && break
done
export C591_TLS_PORT="${TLS_PORT}" C591_PLAIN_PORT="${PLAIN_PORT}" C591_LAN_IP="${LAN_IP}" C591_DIR="${WORK}"

# ── llm ────────────────────────────────────────────────────────────────────
cat > t_llm.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llm.h"
#include "capabilities.h"
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)
static char g_dir[512];
static int log_lines(const char *log) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", g_dir, log);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    int n = 0, ch; while ((ch = fgetc(f)) != EOF) if (ch == '\n') n++;
    fclose(f); return n;
}
static int log_has(const char *log, const char *needle) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", g_dir, log);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    static char b[65536]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return strstr(b, needle) != NULL;
}
static TkLlmResp chat(const char *url, const char *key) {
    TkLlmClient *c = llm_client(url, key, "m");
    c->max_retries = 1;
    TkLlmMsg m = { "user", "hi" };
    return llm_chat(c, &m, 1, 0.0);
}
int main(void) {
    char *av[] = { "t", "--allow-net", NULL };
    tk_cap_init(2, av);
    snprintf(g_dir, sizeof g_dir, "%s", getenv("C591_DIR"));
    const char *tp = getenv("C591_TLS_PORT"), *pp = getenv("C591_PLAIN_PORT"), *lan = getenv("C591_LAN_IP");
    char url[256], ca[600], other[600];
    snprintf(ca, sizeof ca, "%s/ca.pem", g_dir);
    snprintf(other, sizeof other, "%s/other_ca.pem", g_dir);

    /* INF-03: https with the right trust anchor */
    setenv("SSL_CERT_FILE", ca, 1);
    snprintf(url, sizeof url, "https://localhost:%s/v1", tp);
    TkLlmResp r = chat(url, "k-tls");
    CHECK(!r.is_err && r.content && strcmp(r.content, "tls-reply") == 0 && log_has("tls.log", "Bearer k-tls"),
          "INF-03: https://localhost chat over verified TLS");
    if (r.is_err) printf("    (error: %s)\n", r.err_msg ? r.err_msg : "?");
    TkLlmClient *sc = llm_client(url, "k-tls", "m");
    TkLlmMsg m = { "user", "hi" };
    TkLlmStream st = llm_chatstream(sc, &m, 1, 0.0);
    CHECK(!st.is_err && st.nchunks == 1 && strcmp(st.chunks[0], "tls-stream") == 0,
          "INF-03: chatstream over verified TLS");

    int before = log_lines("tls.log");
    setenv("SSL_CERT_FILE", other, 1);
    r = chat(url, "k-wrongca");
    CHECK(r.is_err && log_lines("tls.log") == before, "INF-03: certificate from an untrusted CA refused, no request sent");
    if (!r.is_err || log_lines("tls.log") != before) printf("    (err=%d msg=%s)\n", r.is_err, r.err_msg ? r.err_msg : "");
    setenv("SSL_CERT_FILE", ca, 1);
    snprintf(url, sizeof url, "https://127.0.0.1:%s/v1", tp);
    r = chat(url, "k-wronghost");
    CHECK(r.is_err && log_lines("tls.log") == before && r.err_msg && strstr(r.err_msg, "verify"),
          "INF-03: host name the certificate does not name (127.0.0.1) refused");

    /* cleartext */
    snprintf(url, sizeof url, "http://127.0.0.1:%s/v1", pp);
    r = chat(url, "k-loop");
    CHECK(!r.is_err && log_has("plain.log", "Bearer k-loop"), "cleartext to loopback with a key: allowed (local model server)");
    if (lan && *lan) {
        /* This host's own LAN address: nothing leaves the machine.  (A
         * firewall may reset the connection -- which is why the refusal is
         * asserted by its message, not by the server's log alone.) */
        snprintf(url, sizeof url, "http://%s:%s/v1", lan, pp);
        before = log_lines("plain.log");
        r = chat(url, "k-lan-secret");
        CHECK(r.is_err && r.err_msg && strstr(r.err_msg, "refusing to send the API key") &&
              !log_has("plain.log", "k-lan-secret"),
              "INF-03: API key not sent over cleartext to a non-loopback address");
        if (!r.is_err || !r.err_msg || !strstr(r.err_msg, "refusing"))
            printf("    (err=%d msg=%s)\n", r.is_err, r.err_msg ? r.err_msg : "");
        r = chat(url, "");
        CHECK(!(r.is_err && r.err_msg && strstr(r.err_msg, "refusing")),
              "cleartext to a non-loopback address with NO key is attempted (nothing to leak)");
    } else {
        printf("SKIP INF-03 non-loopback cleartext: this host has no non-loopback IPv4 address\n");
    }

    /* INF-06 */
    before = log_lines("plain.log");
    snprintf(url, sizeof url, "http://127.0.0.1:%s/v1", pp);
    r = chat(url, "k\r\nX-Injected: yes");
    CHECK(r.is_err && log_lines("plain.log") == before, "INF-06: CR/LF in the API key refused, nothing sent");
    snprintf(url, sizeof url, "http://127.0.0.1:%s/v1 HTTP/1.1\r\nX-Evil: 1\r\nX:", pp);
    r = chat(url, "");
    CHECK(r.is_err && log_lines("plain.log") == before && !log_has("plain.log", "\"injected\": true"),
          "INF-06: CR/LF in base_url refused, nothing sent");
    return 0;
}
C
# 127.268: the stdlib link sets come from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not hand lists.
# 127.270: the llm set is cached objects built with case_run's flags for
# that case.  The mlx set stays sources: its build bakes in a per-run port.
# shellcheck disable=SC2206
CF_LLM=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL ${OSSL_CFLAGS} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" \
        --objects llm -- "${CF_LLM[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK_LLM=(); while IFS= read -r l; do LINK_LLM+=("$l"); done <<< "${LINKSET}"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" mlx)" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK_MLX=(); while IFS= read -r l; do LINK_MLX+=("$l"); done <<< "${LINKSET}"
# shellcheck disable=SC2086
CASE_CFLAGS="-DTK_HAVE_OPENSSL ${OSSL_CFLAGS}" case_run llm "${WORK}/t_llm.c" "${LINK_LLM[@]}" ${OSSL_LIBS} -lpthread -lm

# ── e2e: a toke program through tkc ────────────────────────────────────────
if [ -x "${TKC}" ]; then
    cat > p.tk <<EOF
m=p;
i=llm:std.llm;
i=io:std.io;
f=main():i64{
  let c=llm.client("https://localhost:${TLS_PORT}/v1";"k-e2e";"m");
  let msgusr=\$llmmsg{role:"user";content:"hi"};
  let msgs=@(msgusr);
  let r=mt llm.chat(c;msgs) {\$ok:x x.content;\$err:e "err"};
  io.println(r);
  <0
};
EOF
    if TKC_STDLIB_DIR="${S}" "${TKC}" p.tk --out p > p.cl 2>&1; then
        out="$(SSL_CERT_FILE="${WORK}/ca.pem" ./p --allow-net 2>&1 | head -c 300)"
        if [ "${out}" = "tls-reply" ] && grep -q "Bearer k-e2e" tls.log; then
            pass "e2e: tkc-built llm.chat reaches https://localhost over verified TLS"
        else
            fail "e2e: tkc-built llm.chat printed: ${out}"
        fi
    else
        fail "e2e: probe does not compile: $(head -c 400 p.cl)"
    fi
else
    fail "e2e: tkc not found at ${TKC} (run make)"
fi

# ── mlx: bridge requests (INF-02) ──────────────────────────────────────────
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
    MLX_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
    : > mlx.log
    start_srv mlx mlx.port mlx.log "${MLX_PORT}" || fail "mlx mock bridge did not start"
    cat > t_mlx.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mlx.h"
#include "capabilities.h"
int main(void) {
    char *av[] = { "t", "--allow-net", NULL };
    tk_cap_init(2, av);
    MlxModel m = { "m1", "/x" };
    MlxStrResult g = mlx_generate(&m, "hi\",\"max_tokens\":999999,\"x\":\"\\", 5);
    MlxEmbedResult e = mlx_embed(&m, "t\"},{\"evil\":1,\"a\":\"");
    MlxModelResult l = mlx_load("/m\",\"trust_remote_code\":true,\"z\":\"");
    printf("calls %d %d %d\n", g.is_err, e.is_err, l.is_err);
    return 0;
}
C
    CASE_CFLAGS="-DMLX_BRIDGE_PORT=${MLX_PORT}" "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DMLX_BRIDGE_PORT="${MLX_PORT}" \
        -iquote "${S}" -I"${S}" -o t_mlx t_mlx.c "${LINK_MLX[@]}" -lm -lpthread > t_mlx.cc 2>&1 \
        || fail "mlx: build: $(grep -m2 error t_mlx.cc | head -c 300)"
    if [ -x t_mlx ]; then
        ./t_mlx > t_mlx.out 2>&1
        if python3 - mlx.log <<'PY'
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
by = {r["path"]: r for r in recs}
ok = True
def chk(cond, msg):
    global ok
    print(("OK " if cond else "BAD ") + msg); ok = ok and cond
def body(p):
    try: return json.loads(by[p]["body"])
    except Exception as ex: return {"__unparsed__": by.get(p, {}).get("body", "(no request)")}
g = body("/generate"); e = body("/embed"); l = body("/load")
chk(set(g) == {"id", "prompt", "max_tokens"} and g["max_tokens"] == 5 and g["prompt"] == 'hi","max_tokens":999999,"x":"\\',
    "INF-02: generate prompt with '\"' arrives verbatim, max_tokens not overridden (got %r)" % (g,))
chk(set(e) == {"id", "text"} and e["text"] == 't"},{"evil":1,"a":"', "INF-02: embed text escaped (got %r)" % (e,))
chk(set(l) == {"model_path"} and l["model_path"] == '/m","trust_remote_code":true,"z":"', "INF-02: load model_path escaped (got %r)" % (l,))
sys.exit(0 if ok else 1)
PY
        then pass "mlx: 3 check(s)"; else fail "mlx: bridge received injected JSON"; fi
    fi
else
    echo "  skip mlx: not Apple silicon (std.mlx is a no-op elsewhere)"
fi

# ── infer: storage probe runs diskutil without a shell (INF-01) ────────────
if [ "$(uname)" = Darwin ]; then
    cat > t_infer.c <<C
#include "${S}/infer_stream.c"
C
    cat >> t_infer.c <<'C'
#include <sys/stat.h>
int main(void) {
    char dev[600], marker[520];
    snprintf(marker, sizeof marker, "%s/pwned", getenv("C591_DIR"));
    snprintf(dev, sizeof dev, "x'; touch %s; '", marker);
    int ssd = 0, nvme = 0;
    int rc = diskutil_probe(dev, &ssd, &nvme);
    struct stat st;
    printf("%s INF-01: a quote in the device name is not shell-evaluated (marker %s)\n",
           stat(marker, &st) != 0 ? "OK" : "BAD", stat(marker, &st) != 0 ? "absent" : "CREATED");
    printf("%s INF-01: the probe itself ran (rc %d)\n", rc == 0 ? "OK" : "BAD", rc);
    const char *t = tk_infer_detect_storage_type(getenv("C591_DIR"));
    printf("%s storage type of the work dir is a known answer (%s)\n",
           (!strcmp(t, "nvme") || !strcmp(t, "ssd") || !strcmp(t, "hdd")) ? "OK" : "BAD", t);
    return 0;
}
C
    CASE_CFLAGS="" case_run infer "${WORK}/t_infer.c" -lpthread
else
    echo "  skip infer: the diskutil probe is macOS-only"
fi

# ── vecstore: loader bounds (INF-04) and replace-on-OOM (INF-05) ───────────
cat > t_vec.c <<C
#include <stdlib.h>
#include <stdio.h>
static int g_fail_in = 0;   /* fail the Nth malloc from now (0 = never) */
static void *t_malloc(size_t n) { if (g_fail_in > 0 && --g_fail_in == 0) return NULL; return malloc(n); }
#define malloc(n) t_malloc(n)
#include "${S}/vecstore.c"
#undef malloc
C
cat >> t_vec.c <<'C'
#include <sys/stat.h>
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)
static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
int main(void) {
    char dir[600]; snprintf(dir, sizeof dir, "%s/vs", getenv("C591_DIR"));
    mkdir(dir, 0700);
    char p[700]; snprintf(p, sizeof p, "%s/big.vecs", dir);
    FILE *f = fopen(p, "wb");
    w32(f, VECS_MAGIC); w32(f, VECS_VERSION); w32(f, 0x80000000u); w32(f, 2);
    uint64_t pad = 0; fwrite(&pad, 8, 1, f);
    for (int i = 0; i < 20; i++) {
        char id[8]; snprintf(id, sizeof id, "e%02d", i);
        w32(f, 3); fwrite(id, 1, 3, f); w32(f, 1); fwrite("p", 1, 1, f);
        int64_t ts = 0; fwrite(&ts, 8, 1, f);
        float e[2] = {1.0f, 0.0f}; fwrite(e, 4, 2, f);
    }
    fclose(f);
    TkVecStore *vs = vecstore_open(dir);
    TkVecCollection *col = vecstore_collection(vs, "big");
    CHECK(col == NULL, "INF-04: header claiming 2^31 entries over a 20-entry file is refused");

    snprintf(p, sizeof p, "%s/len.vecs", dir);
    f = fopen(p, "wb");
    w32(f, VECS_MAGIC); w32(f, VECS_VERSION); w32(f, 1); w32(f, 2); fwrite(&pad, 8, 1, f);
    w32(f, 0xFFFFFFF0u);
    fclose(f);
    CHECK(vecstore_collection(vs, "len") == NULL, "INF-04: an id length beyond the file is refused");

    TkVecCollection *ok = vecstore_collection(vs, "ok");
    float e[2] = {1.0f, 0.0f};
    CHECK(ok && vecstore_upsert(ok, "a", e, 2, "orig") == 1, "upsert into a fresh collection");
    /* INF-05: fail the 2nd malloc of the replace (1st = normalised vector). */
    g_fail_in = 2;
    int rep = vecstore_upsert(ok, "a", e, 2, "replacement");
    g_fail_in = 0;
    CHECK(rep == 0, "INF-05: replace reports the allocation failure");
    TkSearchResultArray res = vecstore_search(ok, e, 2, 5, -1.0);
    CHECK(res.count == 1 && res.items[0].payload && strcmp(res.items[0].payload, "orig") == 0,
          "INF-05: the entry keeps its previous payload and embedding");
    vecstore_free_results(&res);
    CHECK(vecstore_flush(vs) == 1, "INF-05: flush after the failed replace succeeds (no NULL payload)");
    vecstore_close(vs);

    vs = vecstore_open(dir);
    ok = vecstore_collection(vs, "ok");
    CHECK(ok && vecstore_count(ok) == 1, "a well-formed file reloads");
    vecstore_close(vs);
    return 0;
}
C
# t_vec.c #includes vecstore.c (fault-injecting malloc); the rest of the
# std.vecstore set (str.c since 127.295's tk_strlen) comes from the table.
# shellcheck disable=SC2206
CF_VEC=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" \
        --except vecstore.c --objects vecstore -- "${CF_VEC[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK_VEC=(); while IFS= read -r l; do [ -n "$l" ] && LINK_VEC+=("$l"); done <<< "${LINKSET}"
CASE_CFLAGS="" case_run vec "${WORK}/t_vec.c" "${LINK_VEC[@]}" -lz -lpthread -lm

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
