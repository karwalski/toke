#!/usr/bin/env bash
# C502_capability_serve_download_servedir.sh -- router.serve (and chart.serve
# and dashboard.serve through it) take the net grant; http.downloadfile takes
# fs.write as well as net; http.servedir and http.servepages take fs.read when
# they register (ADR-0010; story 121.42 items 1, 3 and 4).
#
# THE DEFECTS, found by scripts/check_capability_sinks.py (121.22) and
# measured on main (e1cef7da) with binaries compiled --cap-enforce:
#   * router.serve with NO grant bound and listened -- router_serve called
#     tk_bind_exclusive() with no TK_REQUIRE(TK_CAP_NET); dashboard.serve the
#     same, through router_serve;
#   * http.downloadfile with --allow-net only created the destination file --
#     it was gated on net (the connect) and nothing else;
#   * http.servedir and http.servepages with --allow-net only registered and
#     returned 0; the static handler then read files per request (fopen,
#     stat, realpath) and servepages read templates at registration, all with
#     no fs.read.
# Each gap was a dated `ungated` line in scripts/check_capability_sinks_
# skiplist.txt; the gates below delete those lines (the list is self-failing,
# so a stale line would now fail the gate).
#
# WHAT IS ASSERTED
#   1. the sink gate is OK on this tree
#   2-4. NEGATIVE CONTROLS, in a copy of the tree: delete the new TK_REQUIRE
#      in router_serve / http_downloadfile / tk_http_serve_staticdir_w -> the
#      gate FAILS naming tk_router_serve_w / tk_http_downloadfile_w /
#      tk_http_servedir_w.  Each injection is asserted to have landed.
#   5. runtime, no grant: router.serve exits 1 with CAP001 'net' and no
#      listener appears; with --allow-net it listens
#   6. runtime, no grant: dashboard.serve exits 1 with CAP001 'net'
#   7. runtime, --allow-net only: http.downloadfile exits 1 with CAP001
#      'fs.write' and creates nothing; with --allow-net --allow-write it
#      downloads (from a python3 http.server on 127.0.0.1)
#   8. runtime, --allow-net only: http.servedir exits 1 with CAP001 'fs.read';
#      with --allow-read it registers (exit 0)
#   9. runtime, --allow-net only: http.servepages exits 1 with CAP001
#      'fs.read'; with --allow-read it registers
#
# Negative control for 5-9: on main each red case exits 0 (or keeps
# listening) with the grant withheld -- recorded in the 121.42 report.
#
# Story: 121.42
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
GATE="${REPO_ROOT}/scripts/check_capability_sinks.py"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c502_XXXXXX")"
PIDFILE="${WORK}/pids"; : >"${PIDFILE}"
# every background PID this script starts is recorded here and killed by
# exact PID (never by name) on exit
cleanup() { for p in $(cat "${PIDFILE}" 2>/dev/null); do kill "${p}" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
echo "C502: serve / downloadfile / servedir / servepages take their grants (121.42)"
echo "------------------------------------------------------------------------"

# ── 1: the gate on this tree ──────────────────────────────────────────────
if python3 "${GATE}" >"${WORK}/g1.out" 2>&1; then
    pass "1 gate OK: $(tail -1 "${WORK}/g1.out")"
else
    fail "1 gate failed on this tree"; sed 's/^/      | /' "${WORK}/g1.out" | head -20
fi

# ── 2-4: negative controls in a copy ──────────────────────────────────────
mkcopy() {
    rm -rf "${WORK}/tree"; mkdir -p "${WORK}/tree/src" "${WORK}/tree/scripts"
    cp -R "${REPO_ROOT}/src/stdlib" "${WORK}/tree/src/stdlib"
    cp "${REPO_ROOT}/src/stdlib_tables.c" "${REPO_ROOT}/src/llvm.c" "${WORK}/tree/src/"
    cp -R "${REPO_ROOT}/stdlib" "${WORK}/tree/stdlib"
}
control() {  # $1 label  $2 file  $3 function  $4 expected entry
    local label="$1" file="$2" fn="$3" want="$4"
    mkcopy
    local f="${WORK}/tree/src/stdlib/${file}"
    awk -v fn="${fn}" '
        index($0, fn "(") && /^[a-zA-Z]/ { infn = 1 }
        infn && !done && /TK_REQUIRE\(/ { done = 1; infn = 0; next }
        { print }' "${f}" >"${f}.new" && mv "${f}.new" "${f}"
    if cmp -s "${f}" "${REPO_ROOT}/src/stdlib/${file}"; then
        fail "${label} injection did not land (${file}:${fn} unchanged) -- control proves nothing"
        return
    fi
    if python3 "${GATE}" --root "${WORK}/tree" >"${WORK}/g.out" 2>&1; then
        fail "${label} gate stayed OK with ${fn}'s TK_REQUIRE removed"
    elif /usr/bin/grep -a -q "^UNGATED: ${want} " "${WORK}/g.out"; then
        pass "${label} ${fn}'s TK_REQUIRE removed -> gate FAILS: $(/usr/bin/grep -a -m1 "^UNGATED: ${want} " "${WORK}/g.out" | cut -c1-90)"
    else
        fail "${label} gate failed but did not name ${want}"; sed 's/^/      | /' "${WORK}/g.out" | head -8
    fi
}
control 2 router.c router_serve tk_router_serve_w
control 3 http.c http_downloadfile tk_http_downloadfile_w
control 4 tk_web_glue.c tk_http_serve_staticdir_w tk_http_servedir_w

# ── 5-9: the runtime ──────────────────────────────────────────────────────
cd "${WORK}"
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }
listening() { python3 -c 'import socket,sys;s=socket.socket();s.settimeout(1);sys.exit(0 if s.connect_ex(("127.0.0.1",int(sys.argv[1])))==0 else 1)' "$1"; }
# run_bg <name> <cmd...>: start, wait up to 5 s for an exit; echo the exit
# code, or "running" (the PID is in <name>.pid for stopbg)
run_bg() {
    local out="$1"; shift
    "$@" >"${out}.out" 2>"${out}.err" & local pid=$!
    echo "${pid}" >"${out}.pid"; echo "${pid}" >>"${PIDFILE}"
    for _ in $(seq 1 50); do
        if ! kill -0 "${pid}" 2>/dev/null; then wait "${pid}"; echo $?; return; fi
        sleep 0.1
    done
    echo running
}
stopbg() { local pid; pid="$(cat "$1.pid")"; kill "${pid}" 2>/dev/null; }

RPORT="$(freeport)"; DPORT="$(freeport)"; SPORT="$(freeport)"
mkdir -p site pages tmpl dl
printf 'hello\n' >site/a.txt
printf 'payload-121-42\n' >dl/f.txt
printf '<p>page</p>\n' >pages/index.tkt

cat >trouter.tk <<EOF
m=trouter;
i=router:std.router;
f=main():i64{
  let r=router.new();
  let x=mt router.serve(r;"127.0.0.1";${RPORT} as u64) {\$ok:v 0;\$err:e 3};
  <x
};
EOF
cat >tdash.tk <<EOF
m=tdash;
i=dashboard:std.dashboard;
f=main():i64{
  let d=dashboard.new("t";${DPORT} as u64);
  let x=mt dashboard.serve(d;${DPORT}) {\$ok:v 0;\$err:e 3};
  <x
};
EOF
cat >tdl.tk <<EOF
m=tdl;
i=http:std.http;
f=main():i64{
  let c=http.client("http://127.0.0.1:${SPORT}");
  let r=http.downloadfile(c;"/f.txt";"${WORK}/got.txt");
  <r
};
EOF
cat >tdir.tk <<EOF
m=tdir;
i=http:std.http;
f=main():i64{
  let r=http.servedir("/s";"${WORK}/site");
  <r
};
EOF
cat >tpages.tk <<EOF
m=tpages;
i=http:std.http;
f=main():i64{
  let r=http.servepages("${WORK}/pages";"${WORK}/tmpl");
  <0
};
EOF
built=1
for p in trouter tdash tdl tdir tpages; do
    if ! "${TKC}" --cap-enforce "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        fail "compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; built=0
    fi
done
capdenied() { [ "$1" = 1 ] && /usr/bin/grep -a -q "CAP001: capability '$2'" "$3"; }

if [ "${built}" -eq 1 ]; then
    # 5 router.serve
    rc="$(run_bg r1 ./trouter)"
    if capdenied "${rc}" net r1.err && ! listening "${RPORT}"; then
        pass "5a router.serve, no grant: exit 1, CAP001 net, no listener"
    else
        fail "5a router.serve, no grant: ${rc}, listening=$(listening "${RPORT}" && echo yes || echo no)"
        sed 's/^/      | /' r1.err | head -3
    fi
    [ "${rc}" = running ] && stopbg r1
    rc="$(run_bg r2 ./trouter --allow-net)"
    if [ "${rc}" = running ] && listening "${RPORT}"; then
        pass "5b router.serve --allow-net: listening on ${RPORT}"
    else
        fail "5b router.serve --allow-net: ${rc}"; sed 's/^/      | /' r2.err | head -3
    fi
    [ "${rc}" = running ] && stopbg r2

    # 6 dashboard.serve
    rc="$(run_bg d1 ./tdash)"
    if capdenied "${rc}" net d1.err && ! listening "${DPORT}"; then
        pass "6 dashboard.serve, no grant: exit 1, CAP001 net, no listener"
    else
        fail "6 dashboard.serve, no grant: ${rc}"; sed 's/^/      | /' d1.err | head -3
    fi
    [ "${rc}" = running ] && stopbg d1

    # 7 http.downloadfile, against a local python3 http.server
    python3 -m http.server --bind 127.0.0.1 --directory "${WORK}/dl" "${SPORT}" \
        >srv.log 2>&1 & SRVPID=$!; echo "${SRVPID}" >>"${PIDFILE}"
    for _ in $(seq 1 50); do listening "${SPORT}" && break; sleep 0.1; done
    rc=0; ./tdl --allow-net >t7a.out 2>t7a.err || rc=$?
    if capdenied "${rc}" fs.write t7a.err && [ ! -e got.txt ]; then
        pass "7a http.downloadfile --allow-net only: exit 1, CAP001 fs.write, nothing created"
    else
        fail "7a http.downloadfile --allow-net only: rc=${rc}, created=$([ -e got.txt ] && echo yes || echo no)"
        sed 's/^/      | /' t7a.err | head -3
    fi
    rm -f got.txt
    rc=0; ./tdl --allow-net --allow-write >t7b.out 2>t7b.err || rc=$?
    if [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q payload-121-42 got.txt 2>/dev/null; then
        pass "7b http.downloadfile --allow-net --allow-write: exit 0, file downloaded"
    else
        fail "7b http.downloadfile --allow-net --allow-write: rc=${rc}"; sed 's/^/      | /' t7b.err | head -3
    fi
    kill "${SRVPID}" 2>/dev/null; wait "${SRVPID}" 2>/dev/null

    # 8 http.servedir
    rc=0; ./tdir --allow-net >t8a.out 2>t8a.err || rc=$?
    if capdenied "${rc}" fs.read t8a.err; then
        pass "8a http.servedir --allow-net only: exit 1, CAP001 fs.read"
    else
        fail "8a http.servedir --allow-net only: rc=${rc}"; sed 's/^/      | /' t8a.err | head -3
    fi
    rc=0; ./tdir --allow-net --allow-read >t8b.out 2>t8b.err || rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "8b http.servedir --allow-net --allow-read: exit 0"
    else
        fail "8b http.servedir --allow-net --allow-read: rc=${rc}"; sed 's/^/      | /' t8b.err | head -3
    fi

    # 9 http.servepages
    rc=0; ./tpages --allow-net >t9a.out 2>t9a.err || rc=$?
    if capdenied "${rc}" fs.read t9a.err; then
        pass "9a http.servepages --allow-net only: exit 1, CAP001 fs.read"
    else
        fail "9a http.servepages --allow-net only: rc=${rc}"; sed 's/^/      | /' t9a.err | head -3
    fi
    rc=0; ./tpages --allow-net --allow-read >t9b.out 2>t9b.err || rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "9b http.servepages --allow-net --allow-read: exit 0"
    else
        fail "9b http.servepages --allow-net --allow-read: rc=${rc}"; sed 's/^/      | /' t9b.err | head -3
    fi
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
