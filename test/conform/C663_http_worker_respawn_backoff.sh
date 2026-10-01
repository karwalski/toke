#!/usr/bin/env bash
# C663_http_worker_respawn_backoff.sh -- a pre-fork worker that keeps crashing
# is respawned with bounded exponential backoff, not in a fork loop, and the
# backoff resets after stable uptime (story 121.29: audit-120 REL-07).
#
# THE DEFECT, measured on main (437dc471) with this script: both supervisors
# (http_serve_workers, http_serve_tls_workers) forked a replacement the moment
# a worker died abnormally.  A handler that crashes on every request, hit in a
# loop for 3 s, cost one fork per request -- 72 worker lives plain and 59 TLS
# on macOS/arm64, against 12 and 14 now -- with nothing logged and no limit.
# A failed fork left its slot empty for good.
#
# NOW: per slot, the first abnormal death respawns at once; each further death
# within HTTP_RESPAWN_STABLE_MS of the worker's start waits BASE, 2*BASE, ...
# up to CAP (defaults 100 ms / 10 s / 30 s); a worker that stayed up STABLE
# resets the slot.  Each delayed respawn writes
# "DIAGNOSTIC: kind=worker_respawn_backoff slot=N streak=S delay_ms=D".
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c with
# BASE=100 ms, CAP=800 ms, STABLE=1500 ms, serving /ok and /crash (abort()) on
# two pre-fork workers, plain and (when OpenSSL is here) TLS.  Asserted:
#   1. bounded: 3 s of back-to-back /crash requests make at most 40 worker
#      lives (the schedule allows ~7 per slot; main: one per request), and
#      backoff lines appear;
#   2. capped: no delay exceeds CAP, and the delay reaches CAP;
#   3. recovers: /ok answers 200 again within CAP + 3 s of the crashes ending;
#   4. resets: after STABLE of uptime, the next backoff line is the BASE delay
#      at streak 2, not a continuation of the old streak.
#
# Negative control: C663_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib, 1 fails (72 / 59 lives, no backoff lines), and
# 2 and 4 fail (no lines); 3 passes (main never stops respawning).
#
# Note: with every slot waiting out a backoff there is no child, and
# waitpid()'s ECHILD ends the supervision loop (on main it also did so when
# every fork failed, and the server returned).  ECHILD with a respawn pending
# now means "nothing yet"; check 3 is what catches a regression there.
#
# Story: 121.29
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
S="${C663_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c663_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C663: worker respawn backs off, is capped, and resets (121.29 REL-07)"
echo "----------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'EOF'
#define HTTP_RESPAWN_BASE_MS   100L
#define HTTP_RESPAWN_CAP_MS    800L
#define HTTP_RESPAWN_STABLE_MS 1500L
#include "http.c"
#include "capabilities.h"
static const char *g_lives;
static Res ok(Req r) { (void)r; return make_res(200, "ok"); }
static Res crash(Req r) {
    (void)r;
    FILE *f = fopen(g_lives, "a");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    abort();
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    g_lives = argv[1];
    char *grant[] = { argv[0], "--allow-net", NULL };
    tk_cap_init(2, grant);
    http_GET("/ok", ok);
    http_GET("/crash", crash);
#ifdef TK_HAVE_OPENSSL
    if (strcmp(argv[2], "tls") == 0 && argc >= 5) {
        TkTlsCtx *t = http_tls_ctx_new(argv[3], argv[4]);
        if (!t) return 3;
        return (int)http_serve_tls_workers(NULL, "127.0.0.1", 0, t, 2);
    }
#endif
    return (int)http_serve_workers(NULL, "127.0.0.1", 0, 2);
}
EOF

# scenario NAME SCHEME BIN [ARGS...]
scenario() {
    local name="$1" scheme="$2" bin="$3"; shift 3
    local d="${WORK}/run_${name}"; mkdir -p "${d}"
    "${bin}" "${d}/lives" "$@" > "${d}/out" 2> "${d}/err" &
    SRV=$!
    local port=""
    for _ in $(seq 1 80); do
        port="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "${d}/err" | head -1)"
        [ -n "${port}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
    if [ -z "${port}" ]; then
        fail "${name}: server did not start: $(head -2 "${d}/err" | tr '\n' ' ')"
        stop "${SRV}"; SRV=""; return
    fi
    local u="${scheme}://127.0.0.1:${port}"
    # 1. hammer /crash for 3 s
    local end=$(( $(date +%s) + 3 ))
    while [ "$(date +%s)" -lt "${end}" ]; do
        curl -sk --max-time 0.3 -o /dev/null "${u}/crash" 2>/dev/null
    done
    local lives lines maxd
    lives="$(wc -l < "${d}/lives" 2>/dev/null | tr -d ' ')"; lives="${lives:-0}"
    lines="$(grep -c 'kind=worker_respawn_backoff' "${d}/err")"
    if [ "${lives}" -le 40 ] && [ "${lives}" -ge 2 ] && [ "${lines}" -ge 2 ]; then
        pass "${name}: 3 s of crashing requests: ${lives} worker lives, ${lines} backoff lines"
    else
        fail "${name}: 3 s of crashing requests: ${lives} worker lives (want 2..40), ${lines} backoff lines"
    fi
    # 2. capped
    maxd="$(sed -n 's/.*kind=worker_respawn_backoff .*delay_ms=\([0-9]*\).*/\1/p' "${d}/err" | sort -n | tail -1)"
    if [ "${maxd:-0}" = 800 ]; then pass "${name}: delay reaches and never exceeds the 800 ms cap"
    else fail "${name}: largest delay ${maxd:-none}, want exactly the 800 ms cap"; fi
    # 3. recovers
    local code="" t0; t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt 4 ]; do
        code="$(curl -sk --max-time 0.5 -o /dev/null -w '%{http_code}' "${u}/ok" 2>/dev/null)"
        [ "${code}" = 200 ] && break
        sleep 0.1
    done
    if [ "${code}" = 200 ]; then pass "${name}: /ok is 200 again after the crashes stop"
    else fail "${name}: /ok is '${code}' 4 s after the crashes stopped"; fi
    # 4. resets after stable uptime: let both workers live past STABLE
    sleep 2.2
    local before first; before="$(grep -c 'kind=worker_respawn_backoff' "${d}/err")"
    for _ in 1 2 3 4 5 6; do
        curl -sk --max-time 0.3 -o /dev/null "${u}/crash" 2>/dev/null
        [ "$(grep -c 'kind=worker_respawn_backoff' "${d}/err")" -gt "${before}" ] && break
        sleep 0.05
    done
    first="$(grep 'kind=worker_respawn_backoff' "${d}/err" | sed -n "$((before + 1))p")"
    case "${first}" in
        *"streak=2 delay_ms=100") pass "${name}: after stable uptime the backoff restarts at 100 ms" ;;
        *) fail "${name}: after stable uptime, first backoff line: '${first:-none}'" ;;
    esac
    stop "${SRV}"; SRV=""
}

# 127.268: the rest of std.http's link set (h.c #includes http.c itself)
# comes from tkc's dependency table, not a hand list.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http 2> "${WORK}/linkset.err")" \
    || { fail "harness link set: $(head -c 400 "${WORK}/linkset.err")"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

if ${CC} -std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}" -I"${S}" -o "${WORK}/plain" \
        -I"${REPO_ROOT}/stdlib/vendor/tomlc99" "${WORK}/h.c" "${LINK[@]}" \
        -lz -lpthread -lm > "${WORK}/plain.cc" 2>&1; then
    scenario plain http "${WORK}/plain" plain
else
    fail "plain: harness build: $(grep -a -m2 error "${WORK}/plain.cc" | head -c 300)"
fi

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: TLS supervisor -- no OpenSSL headers or openssl CLI on this host"
elif ! openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" -out "${WORK}/cert.pem" \
        -days 2 -nodes -subj "/CN=localhost" > /dev/null 2>&1; then
    fail "tls: could not generate a self-signed certificate"
elif ${CC} -std=c99 -D_GNU_SOURCE -g -O1 -DTK_HAVE_OPENSSL -Wno-deprecated-declarations \
        -I"${OSSL_INC}" -iquote "${S}" -I"${S}" -o "${WORK}/tls" "${WORK}/h.c" \
        -I"${REPO_ROOT}/stdlib/vendor/tomlc99" "${LINK[@]}" \
        -L"$(dirname "${OSSL_INC}")/lib" -lssl -lcrypto -lz -lpthread -lm > "${WORK}/tls.cc" 2>&1; then
    scenario tls https "${WORK}/tls" tls "${WORK}/cert.pem" "${WORK}/key.pem"
else
    fail "tls: harness build: $(grep -a -m2 error "${WORK}/tls.cc" | head -c 300)"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
