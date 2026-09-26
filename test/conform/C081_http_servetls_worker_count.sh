#!/usr/bin/env bash
# C081_http_servetls_worker_count.sh — an HTTPS server's worker count is the
# caller's to choose (story 127.192).
#
# THE DEFECT.  http.servetls(port; cert; key) takes no worker count.  It
# reaches http_serve_tls(), which sizes the pre-fork pool from the CPU count
# or the TK_HTTP_WORKERS environment variable, so a program could not choose
# it: ooke's `[server].workers` was honoured by plain http.serveworkers and
# silently ignored under TLS.  The C http_serve_tls_workers() always took a
# count; nothing exposed it to toke.
#
# THE FIX IS ADDITIVE.  http.servetlsworkers(port; cert; key; workers), with
# http.servetls unchanged.  workers <= 1 is the single-process path, exactly
# as for http.serveworkers.
#
# WHAT IS ASSERTED.  The number of worker processes under the server is the
# number asked for (3 -> 3 children, 1 -> none), and each configuration
# answers an HTTPS request.  TK_HTTP_WORKERS is set to a DIFFERENT value
# (5) throughout, so a wrapper that fell back to http_serve_tls()'s sizing
# would be seen: it would fork 5.  A count is not load-dependent: a slow
# machine delays the forks, it does not change how many there are, and the
# check waits until the count is stable before reading it.
#
# Story: 127.192

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c081_XXXXXX)"
SRV=""
cleanup() {
    [ -n "${SRV}" ] && pkill -P "${SRV}" 2>/dev/null
    [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "C081: http.servetlsworkers forks the worker count it is given"
echo "--------------------------------------"

free_port() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

wait_up() {
    for _ in $(seq 1 80); do
        python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',$1))==0 else 1)" && return 0
        sleep 0.25
    done
    return 1
}

# Children of $1, once two readings 0.5 s apart agree.
stable_children() {
    local prev="x" cur
    for _ in $(seq 1 20); do
        cur="$(pgrep -P "$1" | wc -l | tr -d ' ')"
        [ "${cur}" = "${prev}" ] && { echo "${cur}"; return; }
        prev="${cur}"
        sleep 0.5
    done
    echo "${cur}"
}

if ! command -v openssl >/dev/null 2>&1; then
    echo "  NOT RUN: no openssl(1) on this host to make a test cert"
    echo "--------------------------------------"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" \
    -out "${WORK}/cert.pem" -days 2 -nodes -subj "/CN=localhost" \
    >"${WORK}/cert.log" 2>&1
if [ ! -s "${WORK}/cert.pem" ]; then
    echo "  FAIL: openssl could not make a test cert"
    FAIL=$((FAIL + 1))
fi

run_case() {   # $1 = workers, $2 = expected children
    local w="$1" want="$2" port
    port="$(free_port)"
    cat > "${WORK}/s${w}.tk" <<EOF
m=srv;
i=http:std.http;
f=hi(req:i64):i64{<http.resok("hi")};
f=main():i64{
  http.gethandler("/hi";&hi);
  http.servetlsworkers(${port};"${WORK}/cert.pem";"${WORK}/key.pem";${w});
  <0
};
EOF
    if ! (cd "${WORK}" && "${TKC}" --allow-all -o "s${w}.bin" "s${w}.tk" >"s${w}.log" 2>&1); then
        echo "  FAIL: workers=${w}: server did not build"
        sed 's/^/      /' "${WORK}/s${w}.log" | head -4
        FAIL=$((FAIL + 1))
        return
    fi
    TK_HTTP_WORKERS=5 "${WORK}/s${w}.bin" >/dev/null 2>&1 &
    SRV=$!
    if ! wait_up "${port}"; then
        echo "  FAIL: workers=${w}: server never came up"
        FAIL=$((FAIL + 1))
    else
        local n got
        n="$(stable_children "${SRV}")"
        got="$(curl -sk --max-time 10 "https://127.0.0.1:${port}/hi")"
        if [ "${n}" = "${want}" ] && [ "${got}" = "hi" ]; then
            echo "  PASS: workers=${w}: ${n} worker process(es) (TK_HTTP_WORKERS=5 ignored), answers over TLS"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: workers=${w}: ${n} worker process(es), expected ${want}; answered [${got}]"
            FAIL=$((FAIL + 1))
        fi
    fi
    pkill -P "${SRV}" 2>/dev/null; kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
}

if [ -s "${WORK}/cert.pem" ]; then
    run_case 3 3
    run_case 1 0
fi

# http.servetls is unchanged: same arity in the interface.
if python3 - "${REPO_ROOT}/stdlib/http.tki" <<'PY'
import json, sys
ex = {e["name"]: e for e in json.load(open(sys.argv[1]))["exports"] if e.get("kind") == "func"}
ok = ex.get("http.servetls", {}).get("params") == ["u64", "str", "str"] \
     and ex.get("http.servetlsworkers", {}).get("params") == ["u64", "str", "str", "u64"]
sys.exit(0 if ok else 1)
PY
then
    echo "  PASS: http.tki keeps servetls(u64;str;str) and adds servetlsworkers(u64;str;str;u64)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: http.tki servetls / servetlsworkers signatures are not as declared"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
