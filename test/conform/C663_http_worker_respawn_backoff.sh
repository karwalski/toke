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
#      lives (the schedule allows ~7 per slot; main: one per request);
#   2. the schedule: crashing continues until a line reaches CAP; every line's
#      delay is exactly min(BASE * 2^(streak-2), CAP) -- so none exceeds CAP --
#      and one reaches CAP;
#   3. recovers: /ok answers 200 again after the crashes end;
#   4. resets: after STABLE of uptime, the next backoff line is the BASE delay
#      at streak 2, not a continuation of the old streak.
# 2 and 4 read the delays the supervisor computed and logged, not wall-clock
# gaps, and every wait is "until it happens, under a 60 s hang guard", so host
# load cannot change the verdict (127.558; see "Timing" below).
#
# Negative control: C663_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib, 1 fails (72 / 59 lives), and 2 and 4 fail (no
# lines; 2 after its 60 s guard); 3 passes (main never stops respawning).
# 1 is the only wall-clock bound left, and load can only make it pass: on a
# busy host curl is slower, so the regression can make fewer than 40 lives
# (38 / 16 at load ~110 with respawn_delay forced to 0).  2 and 4 still fail
# on that regression whatever the load.
#
# Note: with every slot waiting out a backoff there is no child, and
# waitpid()'s ECHILD ends the supervision loop (on main it also did so when
# every fork failed, and the server returned).  ECHILD with a respawn pending
# now means "nothing yet"; check 3 is what catches a regression there.
#
# Story: 121.29, 127.558
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

# backoff_lines FILE -- the supervisor's backoff lines, as "slot streak delay".
backoff_lines() {
    sed -n 's/^DIAGNOSTIC: kind=worker_respawn_backoff slot=\([0-9]*\) streak=\([0-9]*\) delay_ms=\([0-9]*\)$/\1 \2 \3/p' "$1"
}
nlines() { grep -c 'kind=worker_respawn_backoff' "$1"; }
# live_workers PID -- the server's children that are not zombies.
live_workers() { ps -A -o ppid=,stat= | awk -v p="$1" '$1 == p && $2 !~ /Z/' | wc -l | tr -d ' '; }
crash_once() { curl -sk --max-time "${REQ_SECS}" -o /dev/null "$1/crash" 2>/dev/null; }

# Timing (127.558).  What is asserted is the schedule the supervisor COMPUTES,
# read from its own lines, never a wall-clock gap: under host load 60-250 the
# old fixed 3 s of crashing could end before any slot's streak reached the
# cap ("largest delay 400").  So the crashing goes on until a cap line is
# logged (or DEADLINE_SECS, a hang guard), each request may take REQ_SECS,
# and the reset check waits for both workers to EXIST before it starts the
# stable-uptime clock, instead of guessing how long the respawn fork takes.
REQ_SECS=2; DEADLINE_SECS=60

# scenario NAME SCHEME BIN [ARGS...]
scenario() {
    local name="$1" scheme="$2" bin="$3"; shift 3
    local d="${WORK}/run_${name}"; mkdir -p "${d}"
    "${bin}" "${d}/lives" "$@" > "${d}/out" 2> "${d}/err" &
    SRV=$!
    local port="" t0
    t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "${DEADLINE_SECS}" ]; do
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
    # 1. bounded: 3 s of back-to-back /crash requests.  An upper bound on
    #    lives, which load only makes easier to meet; main made one per request.
    local end=$(( $(date +%s) + 3 ))
    while [ "$(date +%s)" -lt "${end}" ]; do crash_once "${u}"; done
    local lives
    lives="$(wc -l < "${d}/lives" 2>/dev/null | tr -d ' ')"; lives="${lives:-0}"
    if [ "${lives}" -le 40 ] && [ "${lives}" -ge 1 ]; then
        pass "${name}: 3 s of crashing requests: ${lives} worker lives"
    else
        fail "${name}: 3 s of crashing requests: ${lives} worker lives (want 1..40)"
    fi
    # 2. the schedule: keep crashing until a slot's streak reaches the cap.
    #    Every line must carry delay = min(BASE * 2^(streak-2), CAP) -- a
    #    stable-uptime reset under load starts a streak over at streak=2, which
    #    the rule covers -- and one line must reach the cap.
    t0=$(date +%s)
    while ! grep -q 'kind=worker_respawn_backoff .*delay_ms=800$' "${d}/err" &&
          [ $(( $(date +%s) - t0 )) -lt "${DEADLINE_SECS}" ]; do
        crash_once "${u}"
    done
    local bad nl maxd
    nl="$(nlines "${d}/err")"
    bad="$(backoff_lines "${d}/err" | awk '{
            want = 100; for (i = 2; i < $2 && want < 800; i++) want *= 2;
            if (want > 800) want = 800;
            if ($2 < 2 || $3 != want) { print "slot=" $1 " streak=" $2 " delay_ms=" $3 " (want " want ")"; exit } }')"
    maxd="$(backoff_lines "${d}/err" | awk '$3 > m { m = $3 } END { print m + 0 }')"
    if [ "${nl}" -ge 2 ] && [ -z "${bad}" ] && [ "${maxd}" = 800 ]; then
        pass "${name}: ${nl} backoff lines follow 100*2^(streak-2) and reach the 800 ms cap"
    elif [ -n "${bad}" ]; then
        fail "${name}: backoff line off the schedule: ${bad}"
    else
        fail "${name}: ${nl} backoff lines, largest delay ${maxd} after ${DEADLINE_SECS} s of crashing, want the 800 ms cap"
    fi
    # 3. recovers: /ok answers 200 again once the crashes stop.  The pending
    #    respawn is due within CAP; the deadline only guards a supervisor that
    #    gave up (main's ECHILD exit), which never recovers.
    local code=""; t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "${DEADLINE_SECS}" ]; do
        code="$(curl -sk --max-time "${REQ_SECS}" -o /dev/null -w '%{http_code}' "${u}/ok" 2>/dev/null)"
        [ "${code}" = 200 ] && break
        sleep 0.1
    done
    if [ "${code}" = 200 ]; then pass "${name}: /ok is 200 again after the crashes stop"
    else fail "${name}: /ok is '${code}' ${DEADLINE_SECS} s after the crashes stopped"; fi
    # 4. resets after stable uptime.  Both workers must be running before the
    #    STABLE clock starts (a slot still in backoff would carry its streak);
    #    then let them live past STABLE.
    t0=$(date +%s)
    while [ "$(live_workers "${SRV}")" -lt 2 ] && [ $(( $(date +%s) - t0 )) -lt "${DEADLINE_SECS}" ]; do
        sleep 0.1
    done
    sleep 2.2
    local before first; before="$(nlines "${d}/err")"
    t0=$(date +%s)
    while [ "$(nlines "${d}/err")" -le "${before}" ] && [ $(( $(date +%s) - t0 )) -lt "${DEADLINE_SECS}" ]; do
        crash_once "${u}"
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
# comes from tkc's dependency table, not a hand list.  127.270: as cached
# objects, built with each harness build's own flags.
# objs CF... -- fill LINK with the set compiled with those flags.
objs() {
    local LINKSET l
    LINK=()
    LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c \
            --cc "${CC}" --objects http -- "$@" 2> "${WORK}/linkset.err")" || return 1
    while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
}
VENDOR_TOML="${REPO_ROOT}/stdlib/vendor/tomlc99"

CF=(-std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}" -I"${S}" -I"${VENDOR_TOML}")
if ! objs "${CF[@]}"; then
    fail "plain: harness link set: $(head -c 400 "${WORK}/linkset.err")"
elif ${CC} "${CF[@]}" -o "${WORK}/plain" "${WORK}/h.c" "${LINK[@]}" \
        -lz -lpthread -lm > "${WORK}/plain.cc" 2>&1; then
    scenario plain http "${WORK}/plain" plain
else
    fail "plain: harness build: $(grep -a -m2 error "${WORK}/plain.cc" | head -c 300)"
fi

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -DTK_HAVE_OPENSSL -Wno-deprecated-declarations
    -I"${OSSL_INC}" -iquote "${S}" -I"${S}" -I"${VENDOR_TOML}")
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: TLS supervisor -- no OpenSSL headers or openssl CLI on this host"
elif ! openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" -out "${WORK}/cert.pem" \
        -days 2 -nodes -subj "/CN=localhost" > /dev/null 2>&1; then
    fail "tls: could not generate a self-signed certificate"
elif ! objs "${CF[@]}"; then
    fail "tls: harness link set: $(head -c 400 "${WORK}/linkset.err")"
elif ${CC} "${CF[@]}" -o "${WORK}/tls" "${WORK}/h.c" "${LINK[@]}" \
        -L"$(dirname "${OSSL_INC}")/lib" -lssl -lcrypto -lz -lpthread -lm > "${WORK}/tls.cc" 2>&1; then
    scenario tls https "${WORK}/tls" tls "${WORK}/cert.pem" "${WORK}/key.pem"
else
    fail "tls: harness build: $(grep -a -m2 error "${WORK}/tls.cc" | head -c 300)"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
