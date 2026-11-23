#!/usr/bin/env bash
# C2230_cap_os_enforce.sh -- OS-level enforcement behind the capability broker
# (story 138.44; D5 of docs/decisions/138.19-scoped-capability-grants.md;
# design docs/decisions/138.44-os-enforcement.md).
#
# THE GAP.  The broker is a policy layer in the standard library: native code
# does not pass through it.  A user `extern` C function (here libc's fopen)
# reads ~/.ssh under a grant meant for ./content, and a stdlib sink whose
# gate a defect removed opens, connects, listens and spawns with no check.
#
# THE CHANGE (opt-in).  `os_enforce = true` in tkc.toml [capabilities] (or
# `tkc --cap-os-enforce`) bakes @__tk_cap_baked_os; tk_cap_init then has the
# kernel refuse what the grants do not admit -- macOS sandbox_init(3) with a
# profile generated from the grants, Linux Landlock (+ a seccomp socket
# filter when net is not granted).  The broker is unchanged and still runs
# first: a call it refuses is CAP001/CAP002 exactly as before.  Where the
# kernel cannot carry a grant, CAP004 (a warning) says so and the program runs.
#
# WHAT IS ASSERTED
#   1. the extern-C bypass: without os_enforce, fopen reads a file outside the
#      fs.read scope and creates one outside the fs.write scope; with it,
#      both fail, and a file inside each scope still opens
#   2. a stdlib whose broker gates are removed (a test build: every
#      tk_cap_require_* returns at once): without os_enforce file.read /
#      file.write / process.spawn / net.listen / http.geturl all reach the OS
#      (the bypass is real); with it each is refused by the kernel outside
#      the grants and still works inside them
#   3. the broker still answers first: CAP002 for an fs read outside the
#      scope, byte-for-byte the same text with and without os_enforce
#   4. without os_enforce nothing changes: the IR has no @__tk_cap_baked_os
#      and differs from the os_enforce build by that one line only;
#      --cap-os-enforce bakes the same line
#   5. ALLOW_ALL (all = true) + os_enforce: nothing to restrict, no CAP004
#   6. CAP004: a scope the kernel cannot carry (a control character) lifts
#      that restriction with one warning line and a JSON record with no fix,
#      and the program runs
#   7. the manifest: os_enforce = "yes" is a parse error naming the line;
#      --show-limits prints cap-os; the --companion record says so
#   8. TK_CAP_OS_SHOW prints the generated rules (macOS profile / Linux
#      Landlock rules) with the scopes in them
# On Linux the kernel cases need Landlock (net: ABI 4); a kernel without it
# prints CAP004 and those cases are named skips.  Elsewhere, all are skips.
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- os_enforce is an unknown capability
# key and --cap-os-enforce an unknown flag: every os_enforce build fails, so
# 1, 2 (the with-cases), 4, 5, 6 and 7 FAIL.
#
# Ports 48230-48234 on 127.0.0.1 only (batch Z261); the server is stopped by PID.
# Story: 138.44
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
STDLIB_SRC="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2230_XXXXXX")"
SRV_PID=""
cleanup() { [ -n "${SRV_PID}" ] && kill "${SRV_PID}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
WORK="$(cd "${WORK}" && pwd -P)"
P_LISTEN_OK=48230; P_CONNECT_OK=48231; P_LISTEN_NO=48232; P_SERVER=48233
echo "C2230: OS-level enforcement behind the broker (138.44, opt-in)"
echo "------------------------------------------------------------------------"

case "$(uname -s)" in
    Darwin) OSK=darwin ;;
    Linux)  OSK=linux ;;
    *)      OSK=other ;;
esac

cat >"${WORK}/probe.tk" <<'EOF'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=net:std.net;
i=http:std.http;
i=process:std.process;
f=fopen(p:$str;m:$str):i64;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let p=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="read"){
    let r=mt file.read(p) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{};
  if(op=="write"){
    let r=mt file.write(p;"w") {$ok:v 1;$err:e 2};
    io.println("write-done \(r)");
  }el{};
  if(op=="ffiread"){
    let h=fopen(p;"r");
    io.println("ffiread-done \(h!=0)");
  }el{};
  if(op=="ffiwrite"){
    let h=fopen(p;"w");
    io.println("ffiwrite-done \(h!=0)");
  }el{};
  if(op=="spawn"){
    let r=mt process.spawn(@(p)) {$ok:h 1;$err:e 2};
    io.println("spawn-done \(r)");
  }el{};
  if(op=="listen"){
    let l=net.listen(p);
    io.println("listen-done \(l>0)");
  }el{};
  if(op=="geturl"){
    let r=mt http.geturl(p) {$ok:b 1;$err:e 2};
    io.println("geturl-done \(r)");
  }el{};
  <0
};
EOF
D="${WORK}/data"; O="${WORK}/out"; mkdir -p "${D}" "${O}"
echo a >"${D}/in.txt"; echo s >"${WORK}/secret.txt"
ECHO="$(type -P echo)"; [ -x "${ECHO}" ] || ECHO=/bin/echo   # the file a bare `echo` grant pins
NOTGRANTED=/usr/bin/true; [ -x "${NOTGRANTED}" ] || NOTGRANTED=/bin/true

grants() {   # the shared [capabilities] grant lines
    printf 'fs_read = ["%s"]\nfs_write = ["%s"]\n' "${D}" "${O}"
    printf 'net = ["listen:127.0.0.1:%s", "connect:127.0.0.1:%s"]\n' "${P_LISTEN_OK}" "${P_CONNECT_OK}"
    printf 'process_spawn = ["echo"]\n'
}
{ printf '[capabilities]\n'; grants; printf 'os_enforce = true\n'; } >"${WORK}/os.toml"
{ printf '[capabilities]\n'; grants; } >"${WORK}/plain.toml"

# The broker-removed stdlib: a copy whose gates return at once.
# tkc finds stdlib/vendor at <stdlib dir>/../../stdlib, so the copy sits at
# the same depth with a link to the real stdlib/ beside it.
NB="${WORK}/nbroot/src/stdlib"
mkdir -p "${WORK}/nbroot/src"
cp -R "${STDLIB_SRC}" "${NB}"
ln -s "$(cd "${STDLIB_SRC}/../../stdlib" && pwd)" "${WORK}/nbroot/stdlib"
perl -0pi -e 's/^(void tk_cap_require_\w+\([^)]*\)\s*\{)/$1 return;/mg;
              s/^(void tk_cap_deny\([^)]*\)\s*\{)/$1 return;/mg;
              s/^(int tk_cap_path_admitted\([^)]*\)\s*\{)/$1 return 1;/mg;
              s/^(int tk_cap_check\([^)]*\)\s*\{)/$1 return 1;/mg' "${NB}/capabilities.c"
npatched="$(/usr/bin/grep -a -c -E '^(void|int) tk_cap_[a-z_]+\(.*\{ return' "${NB}/capabilities.c")"

build() {   # build NAME [env...] -- [tkc args...]
    local name="$1"; shift
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    if ( cd "${WORK}" && env ${envs[@]+"${envs[@]}"} "${TKC}" "$@" --out "${WORK}/${name}" probe.tk ) >"${WORK}/${name}.cl" 2>&1; then
        return 0
    fi
    fail "build ${name}: $(/usr/bin/grep -a -v W8001 "${WORK}/${name}.cl" | head -1 | cut -c1-160)"; return 1
}
run() {   # run BIN ARGS... -> $OUT (stdout line), $ERR (stderr), $RC
    RC=0
    ( cd "${WORK}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || RC=$?
    OUT="$(head -1 "${WORK}/o")"; ERR="$(cat "${WORK}/e")"
}
expect_out() {   # LABEL WANT BIN ARGS...
    local label="$1" want="$2"; shift 2
    run "$@"
    if [ "${OUT}" = "${want}" ]; then pass "${label}: ${want}"
    else fail "${label}: wanted '${want}', got '${OUT}' rc=${RC}"; sed 's/^/      | /' "${WORK}/e" | head -3; fi
}

B_OS=0; B_PLAIN=0; B_NBOS=0; B_NBPLAIN=0
build os   -- "--config=${WORK}/os.toml" && B_OS=1
build plain -- "--config=${WORK}/plain.toml" && B_PLAIN=1
if [ "${npatched}" -ge 7 ]; then
    build nbos TKC_STDLIB_DIR="${NB}" TKC_RUNTIME_DIR="${NB}" -- "--config=${WORK}/os.toml" && B_NBOS=1
    build nbplain TKC_STDLIB_DIR="${NB}" TKC_RUNTIME_DIR="${NB}" -- "--config=${WORK}/plain.toml" && B_NBPLAIN=1
else
    fail "the broker-removed test build patched ${npatched} gates, wanted at least 7"
fi

# Does the kernel layer apply here?  KFS / KNET: 1 when it does.
KFS=0; KNET=0
if [ "${B_OS}" -eq 1 ]; then
    run "${WORK}/os" read "${D}/in.txt"
    case "${OSK}" in
        darwin) if ! printf '%s' "${ERR}" | /usr/bin/grep -a -q '^CAP004'; then KFS=1; KNET=1; fi ;;
        linux)
            if ! printf '%s' "${ERR}" | /usr/bin/grep -a -q -E '^CAP004: .*(no Landlock|Landlock is disabled|NO_NEW_PRIVS|landlock_)'; then KFS=1; fi
            if [ "${KFS}" -eq 1 ] && ! printf '%s' "${ERR}" | /usr/bin/grep -a -q '^CAP004: .*no TCP rules'; then KNET=1; fi ;;
    esac
    [ "${KFS}" -eq 1 ] || echo "  (no kernel layer here: $(printf '%s' "${ERR}" | head -1 | cut -c1-140))"
fi

# 1: the extern-C bypass
if [ "${B_PLAIN}" -eq 1 ]; then
    expect_out "1a no os_enforce: extern fopen reads outside the fs.read scope" "ffiread-done true" "${WORK}/plain" ffiread "${WORK}/secret.txt"
    expect_out "1b no os_enforce: extern fopen creates outside the fs.write scope" "ffiwrite-done true" "${WORK}/plain" ffiwrite "${WORK}/made-plain.txt"
fi
if [ "${B_OS}" -eq 1 ] && [ "${KFS}" -eq 1 ]; then
    expect_out "1c os_enforce: extern fopen outside the fs.read scope is refused by the kernel" "ffiread-done false" "${WORK}/os" ffiread "${WORK}/secret.txt"
    expect_out "1d os_enforce: extern fopen inside the fs.read scope still opens" "ffiread-done true" "${WORK}/os" ffiread "${D}/in.txt"
    expect_out "1e os_enforce: extern fopen outside the fs.write scope cannot create" "ffiwrite-done false" "${WORK}/os" ffiwrite "${WORK}/made-os.txt"
    expect_out "1f os_enforce: extern fopen inside the fs.write scope creates" "ffiwrite-done true" "${WORK}/os" ffiwrite "${O}/made.txt"
    [ ! -e "${WORK}/made-os.txt" ] && pass "1g ... and the refused file does not exist" || fail "1g made-os.txt exists"
elif [ "${B_OS}" -eq 1 ]; then
    skip "1c-1g need the kernel layer (${OSK})"
fi

# 2: the broker-removed stdlib
if [ "${B_NBPLAIN}" -eq 1 ]; then
    expect_out "2a broker removed, no os_enforce: file.read outside the scope succeeds" "read-done 1" "${WORK}/nbplain" read "${WORK}/secret.txt"
    expect_out "2b broker removed, no os_enforce: file.write outside the scope succeeds" "write-done 1" "${WORK}/nbplain" write "${WORK}/w-plain.txt"
    expect_out "2c broker removed, no os_enforce: an ungranted command spawns" "spawn-done 1" "${WORK}/nbplain" spawn "${NOTGRANTED}"
    expect_out "2d broker removed, no os_enforce: listen on an ungranted port" "listen-done true" "${WORK}/nbplain" listen "127.0.0.1:${P_LISTEN_NO}"
fi
if [ "${B_NBOS}" -eq 1 ] && [ "${KFS}" -eq 1 ]; then
    expect_out "2e broker removed, os_enforce: file.read outside the scope is refused" "read-done 2" "${WORK}/nbos" read "${WORK}/secret.txt"
    expect_out "2f broker removed, os_enforce: file.read inside the scope" "read-done 1" "${WORK}/nbos" read "${D}/in.txt"
    expect_out "2g broker removed, os_enforce: file.write outside the scope is refused" "write-done 2" "${WORK}/nbos" write "${WORK}/w-os.txt"
    expect_out "2h broker removed, os_enforce: file.write inside the scope" "write-done 1" "${WORK}/nbos" write "${O}/w.txt"
    expect_out "2i broker removed, os_enforce: an ungranted command is refused" "spawn-done 2" "${WORK}/nbos" spawn "${NOTGRANTED}"
    expect_out "2j broker removed, os_enforce: the granted command spawns" "spawn-done 1" "${WORK}/nbos" spawn echo
elif [ "${B_NBOS}" -eq 1 ]; then
    skip "2e-2j need the kernel layer (${OSK})"
fi
if [ "${B_NBOS}" -eq 1 ] && [ "${KNET}" -eq 1 ]; then
    expect_out "2k broker removed, os_enforce: listen on an ungranted port is refused" "listen-done false" "${WORK}/nbos" listen "127.0.0.1:${P_LISTEN_NO}"
    expect_out "2l broker removed, os_enforce: listen on the granted port" "listen-done true" "${WORK}/nbos" listen "127.0.0.1:${P_LISTEN_OK}"
    # an HTTP server on an ungranted port
    if command -v python3 >/dev/null 2>&1; then
        ( cd "${WORK}" && exec python3 -m http.server "${P_SERVER}" --bind 127.0.0.1 ) >/dev/null 2>&1 &
        SRV_PID=$!
        up=0
        for _ in $(seq 1 50); do
            if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(s.connect_ex(('127.0.0.1', ${P_SERVER})))" 2>/dev/null; then up=1; break; fi
            sleep 0.1
        done
        if [ "${up}" -eq 1 ]; then
            expect_out "2m broker removed, no os_enforce: http.geturl to an ungranted port" "geturl-done 1" "${WORK}/nbplain" geturl "http://127.0.0.1:${P_SERVER}/"
            expect_out "2n broker removed, os_enforce: http.geturl to an ungranted port is refused" "geturl-done 2" "${WORK}/nbos" geturl "http://127.0.0.1:${P_SERVER}/"
        else
            fail "2m/2n the test server did not start on 127.0.0.1:${P_SERVER}"
        fi
        kill "${SRV_PID}" 2>/dev/null; wait "${SRV_PID}" 2>/dev/null; SRV_PID=""
    else
        skip "2m/2n need python3 for the test server"
    fi
elif [ "${B_NBOS}" -eq 1 ]; then
    skip "2k-2n need kernel TCP rules (${OSK})"
fi

# 3: the broker still answers first, unchanged
if [ "${B_OS}" -eq 1 ] && [ "${B_PLAIN}" -eq 1 ]; then
    run "${WORK}/plain" read "${WORK}/secret.txt"; e_plain="${ERR}"; rc_plain=${RC}
    run "${WORK}/os" read "${WORK}/secret.txt"; e_os="$(printf '%s\n' "${ERR}" | /usr/bin/grep -a -v '^CAP004')"; rc_os=${RC}
    if [ "${rc_plain}" -eq 1 ] && [ "${rc_os}" -eq 1 ] && [ "${e_plain}" = "${e_os}" ] \
       && printf '%s' "${e_os}" | /usr/bin/grep -a -q '^CAP002:'; then
        pass "3 the broker's CAP002 is the same with and without os_enforce"
    else
        fail "3 CAP002 differs (rc ${rc_plain}/${rc_os})"; printf '%s\n---\n%s\n' "${e_plain}" "${e_os}" | sed 's/^/      | /' | head -8
    fi
fi

# 4: without os_enforce nothing changes in the IR
if ( cd "${WORK}" && "${TKC}" --config="${WORK}/plain.toml" --emit-llvm probe.tk -o plain.ll && \
     "${TKC}" --config="${WORK}/os.toml" --emit-llvm probe.tk -o os.ll && \
     "${TKC}" --config="${WORK}/plain.toml" --cap-os-enforce --emit-llvm probe.tk -o flag.ll ) >/dev/null 2>&1; then
    diff "${WORK}/plain.ll" "${WORK}/os.ll" >"${WORK}/ir.diff"   # a file: pipefail and grep -q
    if ! /usr/bin/grep -a -q '__tk_cap_baked_os' "${WORK}/plain.ll" \
       && [ "$(/usr/bin/grep -a -c '^[<>]' "${WORK}/ir.diff")" = 1 ] \
       && /usr/bin/grep -a -q '^> @__tk_cap_baked_os = constant i32 1$' "${WORK}/ir.diff" \
       && cmp -s "${WORK}/os.ll" "${WORK}/flag.ll"; then
        pass "4 the IR differs by one line, @__tk_cap_baked_os, and --cap-os-enforce bakes the same"
    else
        fail "4 IR: $(head -4 "${WORK}/ir.diff" | tr '\n' ' ')"
    fi
else
    fail "4 --emit-llvm failed"
fi

# 5: ALLOW_ALL + os_enforce
printf '[capabilities]\nall = true\nos_enforce = true\n' >"${WORK}/all.toml"
if build all -- "--config=${WORK}/all.toml"; then
    run "${WORK}/all" ffiread "${WORK}/secret.txt"
    if [ "${OUT}" = "ffiread-done true" ] && ! printf '%s' "${ERR}" | /usr/bin/grep -a -q CAP004; then
        pass "5 all = true + os_enforce: nothing restricted, no CAP004"
    else
        fail "5 all = true + os_enforce: '${OUT}' $(head -1 "${WORK}/e")"
    fi
fi

# 6: CAP004 for a scope the kernel cannot carry
TABDIR="${WORK}/ta$(printf '\t')b"; mkdir -p "${TABDIR}"
printf '[capabilities]\nos_enforce = true\n' >"${WORK}/osonly.toml"
if build osonly -- "--config=${WORK}/osonly.toml"; then
    RC=0
    ( cd "${WORK}" && TK_DIAG_JSON=1 "${WORK}/osonly" ffiread "${WORK}/secret.txt" "--allow-read=${TABDIR}" ) >"${WORK}/o" 2>"${WORK}/e" || RC=$?
    t="$(/usr/bin/grep -a '^CAP004: ' "${WORK}/e" | head -1)"
    j="$(/usr/bin/grep -a '^{"diagnostic_id":"CAP004"' "${WORK}/e" | head -1)"
    if [ "${RC}" -eq 0 ] && [ "$(head -1 "${WORK}/o")" = "ffiread-done true" ] \
       && printf '%s' "${t}" | /usr/bin/grep -a -q "cannot be expressed to the kernel, so fs.read is not restricted by the OS layer; the in-process broker still enforces every grant" \
       && printf '%s' "${j}" | python3 -c 'import json,sys; d=json.loads(sys.stdin.read()); sys.exit(0 if d["severity"]=="warning" and d["stage"]=="runtime" and d["layer"]=="plan" and "fix" not in d and "\t" in d["reason"] else 1)'; then
        pass "6 an inexpressible scope: one CAP004 line + JSON (warning, no fix), fs.read lifted, the program runs"
    else
        fail "6 CAP004 (rc ${RC})"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -5
    fi
    if printf '%s' "${t}" | /usr/bin/grep -a -q "$(printf '\t')"; then fail "6b the CAP004 text line carries a raw control character"
    else pass "6b the CAP004 text line escapes the control character"; fi
fi

# 7: the manifest key and the records
printf '[capabilities]\nfs_read = true\nos_enforce = "yes"\n' >"${WORK}/bad.toml"
if ( cd "${WORK}" && "${TKC}" --config="${WORK}/bad.toml" --check probe.tk ) >"${WORK}/b.out" 2>&1; then
    fail "7a os_enforce = \"yes\" was accepted"
elif /usr/bin/grep -a -q "bad.toml:3: os_enforce wants true or false, got '\"yes\"'" "${WORK}/b.out"; then
    pass "7a os_enforce = \"yes\" is a parse error naming line 3"
else
    fail "7a parse error text: $(head -1 "${WORK}/b.out")"
fi
( cd "${WORK}" && "${TKC}" --config="${WORK}/os.toml" --show-limits probe.tk ) >"${WORK}/sl_os" 2>/dev/null
( cd "${WORK}" && "${TKC}" --config="${WORK}/plain.toml" --show-limits probe.tk ) >"${WORK}/sl_plain" 2>/dev/null
if /usr/bin/grep -a -q '^  cap-os       = 1$' "${WORK}/sl_os" && /usr/bin/grep -a -q '^  cap-os       = 0$' "${WORK}/sl_plain"; then
    pass "7b --show-limits prints cap-os 1 / 0"
else
    fail "7b --show-limits cap-os"
fi
( cd "${WORK}" && "${TKC}" --config="${WORK}/os.toml" --companion probe.tk ) >"${WORK}/cm_os" 2>/dev/null
( cd "${WORK}" && "${TKC}" --config="${WORK}/plain.toml" --companion probe.tk ) >"${WORK}/cm_plain" 2>/dev/null
if /usr/bin/grep -a -q 'OS-level enforcement: \*\*on\*\* (os_enforce)' "${WORK}/cm_os" \
   && /usr/bin/grep -a -q '^## Capabilities' "${WORK}/cm_plain" \
   && ! /usr/bin/grep -a -q 'OS-level enforcement' "${WORK}/cm_plain"; then
    pass "7c the --companion record names the OS layer only when it is on"
else
    fail "7c --companion OS-level line"
fi

# 8: TK_CAP_OS_SHOW
if [ "${B_OS}" -eq 1 ] && [ "${KFS}" -eq 1 ]; then
    RC=0
    ( cd "${WORK}" && TK_CAP_OS_SHOW=1 "${WORK}/os" read "${D}/in.txt" ) >"${WORK}/o" 2>"${WORK}/e" || RC=$?
    ECHO_C="$(cd / && perl -MCwd=abs_path -e 'print abs_path(shift)' "${ECHO}")"
    case "${OSK}" in
        darwin)
            if /usr/bin/grep -a -q "^  (subpath \"${D}\"))\$" "${WORK}/e" \
               && /usr/bin/grep -a -q "^(allow process-exec\* (literal \"${ECHO_C}\"))\$" "${WORK}/e" \
               && /usr/bin/grep -a -q "^(allow network-bind (local ip \"localhost:${P_LISTEN_OK}\"))\$" "${WORK}/e" \
               && /usr/bin/grep -a -q "^(allow network-outbound (remote ip \"localhost:${P_CONNECT_OK}\"))\$" "${WORK}/e"; then
                pass "8 TK_CAP_OS_SHOW prints the sandbox profile with the scopes, the command and the ports"
            else
                fail "8 TK_CAP_OS_SHOW profile"; sed 's/^/      | /' "${WORK}/e" | head -30
            fi ;;
        linux)
            if /usr/bin/grep -a -q "^read scope ${D}\$" "${WORK}/e" && /usr/bin/grep -a -q "^write scope ${O}\$" "${WORK}/e" \
               && /usr/bin/grep -a -q "^execute scope ${ECHO_C}\$" "${WORK}/e"; then
                pass "8 TK_CAP_OS_SHOW prints the Landlock rules with the scopes and the command"
            else
                fail "8 TK_CAP_OS_SHOW rules"; sed 's/^/      | /' "${WORK}/e" | head -30
            fi ;;
    esac
else
    skip "8 needs the kernel layer (${OSK})"
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
[ "${FAIL}" -eq 0 ]
