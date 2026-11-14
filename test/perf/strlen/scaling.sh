#!/bin/sh
# scaling.sh — gate 2 of story 127.170 (docs/runtime-abi.md §3.1): string
# scanning must be linear.
#
# Runs each program at N and 4N and compares INSTRUCTIONS RETIRED, which load
# does not move (wall time on this machine does, two- to eightfold).  Linear
# work gives a ratio near 4; an O(N) length scan per call gives ~16.  Fails if
# any ratio exceeds LIMIT (default 6).
#
#   scan_from  the 127.91 comma scanner: str.indexfrom in a loop
#   len_only   lp(k<t.len) — .len in the loop condition
#   len_loop   the same with t.charat(k) in the body: the lexer loop
#
# Counter: macOS `/usr/bin/time -l` ("instructions retired"); Linux
# `perf stat -e instructions:u`.  With neither, it says SKIP and exits 77.
# TOKE=<compiler> measures another compiler (default: this tree's ./toke);
# PROGS="..." runs a subset.  TK_STR_VERIFY is unset for the measured runs:
# the verify switch adds a strlen to every trusted read by design, which is
# quadratic here.
set -u
DIR=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$DIR/../../.." && pwd)
TK=${TOKE:-$ROOT/toke}; N=${N:-4000}; LIMIT=${LIMIT:-6}

# 127.439: a macOS VM (GitHub's macos-15 runners) has no performance
# counters, and /usr/bin/time -l then prints no "instructions retired" line
# at all.  That is a host without a counter, the SKIP case below, not a
# failure of the code under test, so the counter is probed before it is used.
darwin_counter() {
  /usr/bin/time -l /usr/bin/true 2>&1 >/dev/null | awk '/instructions retired/ && $1 > 0 {f=1} END{exit !f}'
}
if [ "$(uname -s)" = Darwin ] && [ -x /usr/bin/time ] && darwin_counter; then
  insns() { env -u TK_STR_VERIFY PAT_N=$2 /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '/instructions retired/{print $1}'; }
elif command -v perf >/dev/null 2>&1 && perf stat -x, -e instructions:u true >/dev/null 2>&1; then
  insns() { env -u TK_STR_VERIFY PAT_N=$2 perf stat -x, -e instructions:u "$1" 2>&1 >/dev/null | awk -F, '/instructions/{print $1}'; }
else
  echo "SKIP: no instruction counter (/usr/bin/time -l on macOS reports none on this host, perf on Linux)"; exit 77
fi

W=$(mktemp -d "${TMPDIR:-/tmp}/tk_strscale.XXXXXX"); trap 'rm -rf "$W"' EXIT
fail=0
for p in ${PROGS:-scan_from len_only len_loop}; do
  "$TK" -O2 --allow-all "$DIR/$p.tk" --out "$W/$p" >/dev/null 2>&1 || { echo "FAIL $p: compile"; fail=1; continue; }
  a=$(insns "$W/$p" "$N"); b=$(insns "$W/$p" $((N*4)))
  case "$a$b" in *[!0-9]*|'') echo "FAIL $p: no instruction count (got '$a' / '$b')"; fail=1; continue;; esac
  r=$(awk -v a="$a" -v b="$b" 'BEGIN{printf "%.2f", b/a}')
  if awk -v r="$r" -v l="$LIMIT" 'BEGIN{exit !(r>l)}'; then
    echo "FAIL $p: insns N=$N $a, 4N $b, ratio $r > $LIMIT (quadratic)"; fail=1
  else
    echo "PASS $p: insns N=$N $a, 4N $b, ratio $r"
  fi
done
exit $fail
