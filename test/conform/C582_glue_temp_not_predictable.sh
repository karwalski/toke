#!/usr/bin/env bash
# C582_glue_temp_not_predictable.sh — the auto-glue temp file is not written
# through a predictable, symlink-following name (story 121.28, audit-120
# compiler-toolchain COM-03).
#
# THE DEFECT.  glue_gen_write_temp wrote the generated wrappers to
# /tmp/tk_glue_auto_<pid>.c with fopen("w").  A symlink planted at that name
# was followed: its target was truncated and overwritten with the generated
# C (arbitrary file overwrite with the compiler user's rights), and the file
# could be swapped before clang compiled it into the binary.  It now comes
# from mkstemps (O_EXCL, unpredictable, mode 0600).
#
# WHAT THIS DRIVES.  The pid is made known in advance: a bash process plants
# the symlink at /tmp/tk_glue_auto_$$.c and then `exec`s tkc, which keeps the
# pid.  The victim file must be unchanged and the build must still succeed
# and run.
#
# Negative control: restore the old two lines in src/glue_gen.c
#   snprintf(out_path, ..., "/tmp/tk_glue_auto_%d.c", (int)getpid());
#   FILE *f = fopen(out_path, "w");
# — the victim is overwritten with "/*\n * tk_glue_auto.c — ..." and the
# victim case fails.
#
# Story: 121.28

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c582_XXXXXX")"
PIDFILE="${WORK}/pid"
cleanup() {
    if [ -s "${PIDFILE}" ]; then
        local l="/tmp/tk_glue_auto_$(cat "${PIDFILE}").c"
        [ -L "${l}" ] && rm -f "${l}"
    fi
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "C582: auto-glue temp file is not predictable"
echo "--------------------------------------------"

cat > "${WORK}/hello.tk" <<'EOF'
m=c582;
i=io:std.io;
f=main():i64{
  io.println("hello");
  <0
};
EOF
printf 'VICTIM\n' > "${WORK}/victim.txt"

rc=0
bash -c 'echo $$ > "$1"; ln -s "$2" "/tmp/tk_glue_auto_$$.c" || exit 99;
         exec "$3" --out "$4" "$5"' _ \
    "${PIDFILE}" "${WORK}/victim.txt" "${TKC}" "${WORK}/hello" "${WORK}/hello.tk" \
    >"${WORK}/build.log" 2>&1 || rc=$?

if [ "${rc}" = 99 ]; then
    fail "could not plant the symlink (pid name already taken)"
elif [ "$(cat "${WORK}/victim.txt")" = "VICTIM" ]; then
    pass "planted symlink target left unchanged"
else
    fail "planted symlink target overwritten: $(head -c 60 "${WORK}/victim.txt" | tr '\n' ' ')"
fi

if [ "${rc}" = 0 ] && [ "$("${WORK}/hello" 2>/dev/null)" = "hello" ]; then
    pass "program still builds and runs"
else
    fail "build rc ${rc}: $(head -c 200 "${WORK}/build.log")"
fi

echo ""
echo "C582: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
