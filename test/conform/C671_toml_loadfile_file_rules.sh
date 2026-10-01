#!/usr/bin/env bash
# C671_toml_loadfile_file_rules.sh -- toml.loadfile reads through std.file's
# open path: it refuses a symlinked final component (AMB-07) and a directory,
# and does not hang on a writerless FIFO (story 121.74).
#
# THE DEFECT.  toml.c's toml_load_file used a plain fopen().  It followed a
# symlink on the final path component -- the rule every std.file read enforces
# with O_NOFOLLOW -- and on Darwin a directory opened, its fseek/ftell/fread
# did not fail, and it parsed as an EMPTY TOML document ($ok).  Measured on
# main 437dc471: cases 2 and 3 exit 0 ($ok), case 5 hangs.
#
# THE FIX.  toml_load_file reads with file_read() (file.c), which opens
# O_NOFOLLOW | O_NONBLOCK, refuses a directory with "is a directory", and
# reads a FIFO non-blockingly (121.73).  std.toml now links std.file.
#
# WHAT IS ASSERTED (probe exits 0 on $ok, 3 on $err; every run bounded)
#   1. a regular TOML file: $ok;
#   2. a symlink to that file: $err;
#   3. a directory: $err;
#   4. a missing path: $err;
#   5. a writerless FIFO: returns promptly (an empty document is $ok).
#
# Negative control: C671_STDLIB=<a src/stdlib tree> builds the probe against
# it (via TKC_STDLIB_DIR).  With main's tree cases 2, 3 and 5 fail.
#
# Story: 121.74
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C671_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c671_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C671: toml.loadfile follows std.file's open rules (121.74)"
echo "----------------------------------------------------------"

cat > tl.tk <<'EOF'
m=tl;
i=toml:std.toml;
i=args:std.args;
f=main():i64{
  let p=args.get(1);
  let r=mt toml.loadfile(p) {$ok:v 0;$err:e 3};
  <r
};
EOF
if ! TKC_STDLIB_DIR="${S}" "${TKC}" tl.tk --out tl >tl.cl 2>&1; then
    echo "  FAIL: compile tl.tk"; sed 's/^/      | /' tl.cl | head -5; exit 1
fi

run_bounded() {   # run_bounded SECS CMD...: CMD's status, 124 if killed
    local secs="$1"; shift
    "$@" >/dev/null 2>&1 & local p=$! t=0
    while kill -0 "${p}" 2>/dev/null; do
        if [ "${t}" -ge $((secs * 10)) ]; then
            kill -9 "${p}" 2>/dev/null; wait "${p}" 2>/dev/null; return 124
        fi
        sleep 0.1; t=$((t + 1))
    done
    wait "${p}"
}

printf 'name = "x"\nn = 1\n' > good.toml
ln -s good.toml link.toml
mkdir adir
mkfifo empty.fifo
for c in "1|good.toml|0" "2|link.toml|3" "3|adir|3" "4|nope.toml|3" "5|empty.fifo|0"; do
    IFS='|' read -r n p want <<<"${c}"
    rc=0; run_bounded 20 ./tl --allow-read "${p}" || rc=$?
    if [ "${rc}" = "${want}" ]; then
        pass "${n} ${p}: exit ${rc}"
    else
        fail "${n} ${p}: exit ${rc}, want ${want} (124 = hung, killed at 20 s)"
    fi
done

echo "----------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
