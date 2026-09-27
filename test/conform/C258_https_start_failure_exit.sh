#!/usr/bin/env bash
# C258_https_start_failure_exit.sh — an HTTPS server that does not start makes
# the process exit non-zero, and tells its caller (story 135.22).
#
# THE DEFECT.  http.servetls with a certificate it could not load printed
# "http_tls_ctx_new: failed to load cert: ..." and returned -1.  Every caller
# in the tree discards that value (ooke's serve.tk, the example in
# docs/stdlib/http.md), so main ran off its end and the process exited 0: to
# a supervisor, a server that never started looked like a clean shutdown.
#
# THE FIX (src/stdlib/tk_web_glue.c, tk_exitfail.h).  A start failure of
# http.servetls / http.servetlsworkers still returns -1 to the caller, prints
# one `DIAGNOSTIC: kind=server_start_failed fn=... port=... stage=...` line,
# and records the failure, so the process exits 1 however main ends.
#
# WHAT IS ASSERTED:
#   1. servetls, missing certificate: the caller sees -1, the structured line
#      names stage "certificate/key", exit 1
#   2. servetlsworkers, good certificate but missing key: same, exit 1
#   3. servetls, good certificate, port already held by a listener: stage
#      "bind/fork", exit 1
#   4. servetls that starts and is stopped with SIGTERM: exit 0, no line
#      (the fix must not turn a clean shutdown into a failure)
#
# Negative control: C258_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's tk_web_glue.c fails cases 1-3 (exit 0, no structured line).
#
# Story: 135.22
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C258_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c258_XXXXXX")"
SRV=""; HOLD=""
cleanup() {
    [ -n "${SRV}" ] && { pkill -9 -P "${SRV}" 2>/dev/null; kill -9 "${SRV}" 2>/dev/null; }
    [ -n "${HOLD}" ] && kill -9 "${HOLD}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT
cd "${WORK}"
echo "C258: an HTTPS server that does not start exits non-zero (135.22)"
echo "------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"
         [ -n "${4:-}" ] && sed 's/^/        | /' "$4" | head -5
         FAIL=$((FAIL + 1)); fi
}

if ! command -v openssl >/dev/null 2>&1; then
    echo "  FAIL: no openssl(1) on this host to make a test certificate"
    echo "C258: 0 passed, 1 failed"; exit 1
fi
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 2 -nodes \
    -subj "/CN=localhost" > cert.log 2>&1
[ -s cert.pem ] || { echo "  FAIL: openssl could not make a test certificate"; echo "C258: 0 passed, 1 failed"; exit 1; }

build() {   # build NAME CALL
    cat > "$1.tk" <<TOKE
m=srv;
i=http:std.http;
i=io:std.io;
f=main():i64{
  http.getstatic("/ok"; "ok");
  let r=$2;
  io.println("r=\(r)");
  <0
};
TOKE
    if ! "${TKC}" --allow-all --out "$1_bin" "$1.tk" > "$1.log" 2>&1; then
        echo "  FAIL: $1 did not compile"; sed 's/^/        /' "$1.log" | head -4
        FAIL=$((FAIL + 1)); return 1
    fi
}
# result NAME -> "rc stdout-r structured-line-count exit_status-line-count"
result() {
    printf '%s %s %s %s' "$RC" "$(grep -a '^r=' "$1.out")" \
        "$(grep -a -c "kind=server_start_failed fn=\"$2\" port=$3 stage=\"$4\"" "$1.err")" \
        "$(grep -a -c 'kind=exit_status status=1' "$1.err")"
}

# 1. missing certificate
if build nocert 'http.servetls(18443;"/nonexistent/cert.pem";"/nonexistent/key.pem")'; then
    ./nocert_bin > nocert.out 2> nocert.err; RC=$?
    expect "servetls, missing certificate: caller sees -1, structured line, exit 1" \
        "$(result nocert http.servetls 18443 certificate/key)" "1 r=-1 1 1" nocert.err
fi

# 2. good certificate, missing key
if build nokey "http.servetlsworkers(18444;\"${WORK}/cert.pem\";\"/nonexistent/key.pem\";2)"; then
    ./nokey_bin > nokey.out 2> nokey.err; RC=$?
    expect "servetlsworkers, missing key: caller sees -1, structured line, exit 1" \
        "$(result nokey http.servetlsworkers 18444 certificate/key)" "1 r=-1 1 1" nokey.err
fi

# 3. a port already held by a listener (bound to port 0 and read back, so
#    there is no window in which another process can take it first)
python3 -c '
import socket, sys, time
s = socket.socket(); s.bind(("", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
time.sleep(120)' > hold.port &
HOLD=$!
for _ in $(seq 1 50); do [ -s hold.port ] && break; sleep 0.1; done
BUSY="$(head -1 hold.port)"
if [ -z "${BUSY}" ]; then
    echo "  FAIL: could not hold a port for case 3"; FAIL=$((FAIL + 1))
elif build busy "http.servetls(${BUSY};\"${WORK}/cert.pem\";\"${WORK}/key.pem\")"; then
    ./busy_bin > busy.out 2> busy.err; RC=$?
    expect "servetls on a held port: caller sees -1, stage bind/fork, exit 1" \
        "$(result busy http.servetls "${BUSY}" bind/fork)" "1 r=-1 1 1" busy.err
fi
kill -9 "${HOLD}" 2>/dev/null; wait "${HOLD}" 2>/dev/null; HOLD=""

# 4. a server that starts and is shut down cleanly still exits 0.  The port
#    is the one case 3 just released.  A window remains in which another
#    process could bind it; if one does, this case reads "never answered"
#    (a FAIL), never a false PASS.
if [ -n "${BUSY}" ] && build clean "http.servetlsworkers(${BUSY};\"${WORK}/cert.pem\";\"${WORK}/key.pem\";1)"; then
    ./clean_bin > clean.out 2> clean.err &
    SRV=$!
    up=1
    for _ in $(seq 1 80); do
        kill -0 "${SRV}" 2>/dev/null || break
        if [ "$(curl -sk --max-time 2 "https://127.0.0.1:${BUSY}/ok" 2>/dev/null)" = "ok" ]; then up=0; break; fi
        sleep 0.1
    done
    if [ "${up}" -ne 0 ]; then
        echo "  FAIL: the clean-shutdown server never answered on ${BUSY}"
        sed 's/^/        | /' clean.err | head -4; FAIL=$((FAIL + 1))
    else
        kill -TERM "${SRV}"; wait "${SRV}"; RC=$?; SRV=""
        expect "servetls that started, then SIGTERM: exit 0, r=0, no failure lines" \
            "$(printf '%s %s %s %s' "${RC}" "$(grep -a '^r=' clean.out)" \
                "$(grep -a -c 'server_start_failed' clean.err)" "$(grep -a -c 'kind=exit_status' clean.err)")" \
            "0 r=0 0 0" clean.err
    fi
fi

echo "------------------------------------------------------------------"
echo "C258: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
