#!/bin/sh
# perfmeasure.sh — compile one toke program and report wall, peak RSS and
# allocation count for a given PAT_N.  Batch F (127.48/127.50/127.91/127.108).
#
# Usage: test/perf/perfmeasure.sh <file.tk> <PAT_N> [reps]
#
# Wall is the MINIMUM of `reps` runs (default 3): the minimum is the run least
# disturbed by other load, and these programs are deterministic.  Peak RSS is
# ru_maxrss from /usr/bin/time -l on that same set (max taken).  Allocation
# counts come from bench/patterns/liballoccount.dylib, the interposer already
# in the tree; `make -C bench/patterns` builds it.  macOS only — the RSS unit
# on Darwin is bytes, on Linux it is kibibytes, and the interposer uses dyld.
#
# Every number this prints is from this host; a ratio without the host and the
# baseline beside it is not a result.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TK=$1; N=$2; REPS=${3:-3}
BIN=$(mktemp -t perfmeasure)
"$ROOT/toke" -O2 --allow-all "$TK" --out "$BIN" >/dev/null || { echo "COMPILE FAILED: $TK" >&2; exit 1; }
best=99999; rss=0; out=""
i=0
while [ "$i" -lt "$REPS" ]; do
  i=$((i+1))
  t=$(PAT_N="$N" /usr/bin/time -l "$BIN" 2>"$BIN.err" | tail -1)
  out=$t
  w=$(awk '/real/{print $1}' "$BIN.err")
  r=$(awk '/maximum resident/{print $1}' "$BIN.err")
  best=$(awk -v a="$best" -v b="$w" 'BEGIN{print (b<a)?b:a}')
  rss=$(awk -v a="$rss" -v b="$r" 'BEGIN{print (b>a)?b:a}')
done
DY="$ROOT/bench/patterns/liballoccount.dylib"
ac="n/a"
if [ -f "$DY" ]; then
  ac=$(PAT_N="$N" DYLD_INSERT_LIBRARIES="$DY" "$BIN" 2>&1 >/dev/null | awk '/^ALLOC/{print $2}')
fi
printf '%-44s N=%-10s wall=%ss rss=%sB %s stdout=%s\n' "$TK" "$N" "$best" "$rss" "$ac" "$out"
rm -f "$BIN" "$BIN.err"
