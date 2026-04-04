#!/usr/bin/env bash
# run_bench.sh — Benchmark runner comparing toke vs C compiled binaries.
# Usage: ./bench/run_bench.sh [iterations]
#   iterations: number of times to run each benchmark (default: 10)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROG_DIR="$SCRIPT_DIR/programs"
BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"
TKC="$ROOT_DIR/tkc"
CC="${CC:-cc}"
ITERS="${1:-10}"

# Benchmark programs (basenames without extension)
BENCHMARKS=(
  fib_recursive
  fib_iterative
  sum_array
  nested_loops
  binary_search
  prime_sieve
  deep_recursion
  struct_ops
  large_expr
  chained_calls
  collatz
  gcd_euler
)

mkdir -p "$BUILD_DIR" "$RESULTS_DIR"

# ── Colours (disabled when not a tty) ───────────────────────────────
if [ -t 1 ]; then
  BOLD='\033[1m'  DIM='\033[2m'  RST='\033[0m'
  GREEN='\033[32m' RED='\033[31m' CYAN='\033[36m'
else
  BOLD='' DIM='' RST='' GREEN='' RED='' CYAN=''
fi

printf "${BOLD}toke vs C benchmark suite${RST}\n"
printf "  compiler : %s\n" "$($TKC --version 2>&1)"
printf "  cc       : %s\n" "$($CC --version 2>&1 | head -1)"
printf "  runs     : %d per benchmark\n" "$ITERS"
printf "  date     : %s\n\n" "$(date -u '+%Y-%m-%d %H:%M UTC')"

# ── Compile all programs ────────────────────────────────────────────
printf "${CYAN}Compiling...${RST}\n"
compile_failures=()
for name in "${BENCHMARKS[@]}"; do
  tk_src="$PROG_DIR/${name}.tk"
  c_src="$PROG_DIR/${name}.c"
  tk_bin="$BUILD_DIR/${name}_tk"
  c_bin="$BUILD_DIR/${name}_c"

  if [ ! -f "$tk_src" ]; then
    printf "  ${RED}SKIP${RST} %s — no .tk source\n" "$name"
    compile_failures+=("$name")
    continue
  fi
  if [ ! -f "$c_src" ]; then
    printf "  ${RED}SKIP${RST} %s — no .c source\n" "$name"
    compile_failures+=("$name")
    continue
  fi

  # Compile toke
  if ! "$TKC" -O2 "$tk_src" --out "$tk_bin" 2>/dev/null; then
    printf "  ${RED}FAIL${RST} %s.tk — tkc compilation failed\n" "$name"
    compile_failures+=("$name")
    continue
  fi

  # Compile C
  if ! "$CC" -O2 -o "$c_bin" "$c_src" 2>/dev/null; then
    printf "  ${RED}FAIL${RST} %s.c — cc compilation failed\n" "$name"
    compile_failures+=("$name")
    continue
  fi

  printf "  ${GREEN}OK${RST}   %s\n" "$name"
done
printf "\n"

# ── Helper: median of sorted values ────────────────────────────────
median() {
  local -a sorted=($(printf '%s\n' "$@" | sort -n))
  local n=${#sorted[@]}
  echo "${sorted[$((n / 2))]}"
}

# ── Helper: run one binary, capture wall-time and peak RSS ──────────
# Returns: wall_ms peak_rss_kb
run_one() {
  local bin="$1"
  # macOS /usr/bin/time -lp gives wall time and max RSS in bytes
  local tmp
  tmp=$(mktemp)
  /usr/bin/time -lp "$bin" >/dev/null 2>"$tmp"
  local wall_s peak_bytes
  wall_s=$(awk '/^real/ {print $2}' "$tmp")
  # macOS: "maximum resident set size" line gives bytes
  peak_bytes=$(awk '/maximum resident set size/ {print $1}' "$tmp" | head -1)
  rm -f "$tmp"
  # Convert wall seconds to milliseconds (integer)
  local wall_ms
  wall_ms=$(awk "BEGIN {printf \"%.0f\", $wall_s * 1000}")
  # Convert bytes to KB
  local peak_kb
  peak_kb=$(( ${peak_bytes:-0} / 1024 ))
  echo "$wall_ms $peak_kb"
}

# ── Run benchmarks ──────────────────────────────────────────────────
printf "${CYAN}Running benchmarks (%d iterations each)...${RST}\n\n" "$ITERS"

# Collect results: arrays indexed by benchmark
declare -a tk_times_med c_times_med tk_rss_med c_rss_med tk_sizes c_sizes

idx=0
for name in "${BENCHMARKS[@]}"; do
  # Skip if compilation failed
  if [[ " ${compile_failures[*]:-} " == *" $name "* ]]; then
    continue
  fi

  tk_bin="$BUILD_DIR/${name}_tk"
  c_bin="$BUILD_DIR/${name}_c"

  printf "  %-20s " "$name"

  # Gather samples
  tk_wall_samples=()
  tk_rss_samples=()
  c_wall_samples=()
  c_rss_samples=()

  for ((r = 0; r < ITERS; r++)); do
    read -r w rss <<< "$(run_one "$tk_bin")"
    tk_wall_samples+=("$w")
    tk_rss_samples+=("$rss")
  done
  for ((r = 0; r < ITERS; r++)); do
    read -r w rss <<< "$(run_one "$c_bin")"
    c_wall_samples+=("$w")
    c_rss_samples+=("$rss")
  done

  # Compute medians
  tk_med=$(median "${tk_wall_samples[@]}")
  c_med=$(median "${c_wall_samples[@]}")
  tk_rss=$(median "${tk_rss_samples[@]}")
  c_rss=$(median "${c_rss_samples[@]}")

  # Binary sizes
  tk_sz=$(stat -f%z "$tk_bin" 2>/dev/null || stat --printf=%s "$tk_bin" 2>/dev/null)
  c_sz=$(stat -f%z "$c_bin" 2>/dev/null || stat --printf=%s "$c_bin" 2>/dev/null)

  # Ratio (toke / C), lower is better for toke
  if [ "$c_med" -gt 0 ]; then
    ratio=$(awk "BEGIN {printf \"%.2f\", $tk_med / $c_med}")
  else
    ratio="N/A"
  fi

  printf "toke: %5d ms  C: %5d ms  ratio: %sx\n" "$tk_med" "$c_med" "$ratio"

  # Store for table
  tk_times_med+=("$tk_med")
  c_times_med+=("$c_med")
  tk_rss_med+=("$tk_rss")
  c_rss_med+=("$c_rss")
  tk_sizes+=("$tk_sz")
  c_sizes+=("$c_sz")
  idx=$((idx + 1))
done

printf "\n"

# ── Generate markdown results table ────────────────────────────────
RESULT_FILE="$RESULTS_DIR/results_$(date '+%Y%m%d_%H%M%S').md"
{
  echo "# Benchmark Results"
  echo ""
  echo "- **Date**: $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "- **Compiler**: $($TKC --version 2>&1)"
  echo "- **CC**: $($CC --version 2>&1 | head -1)"
  echo "- **Iterations**: $ITERS per benchmark"
  echo "- **Optimisation**: -O2 for both toke and C"
  echo "- **Platform**: $(uname -srm)"
  echo ""
  echo "## Wall Time (median, ms)"
  echo ""
  echo "| Benchmark | toke (ms) | C (ms) | Ratio (toke/C) |"
  echo "|-----------|-----------|--------|----------------|"

  active_idx=0
  for name in "${BENCHMARKS[@]}"; do
    if [[ " ${compile_failures[*]:-} " == *" $name "* ]]; then
      echo "| $name | SKIP | SKIP | -- |"
      continue
    fi
    tk_t="${tk_times_med[$active_idx]}"
    c_t="${c_times_med[$active_idx]}"
    if [ "$c_t" -gt 0 ]; then
      ratio=$(awk "BEGIN {printf \"%.2f\", $tk_t / $c_t}")
    else
      ratio="N/A"
    fi
    echo "| $name | $tk_t | $c_t | ${ratio}x |"
    active_idx=$((active_idx + 1))
  done

  echo ""
  echo "## Binary Size (bytes)"
  echo ""
  echo "| Benchmark | toke | C | Ratio (toke/C) |"
  echo "|-----------|------|---|----------------|"

  active_idx=0
  for name in "${BENCHMARKS[@]}"; do
    if [[ " ${compile_failures[*]:-} " == *" $name "* ]]; then
      echo "| $name | SKIP | SKIP | -- |"
      continue
    fi
    tk_s="${tk_sizes[$active_idx]}"
    c_s="${c_sizes[$active_idx]}"
    if [ "$c_s" -gt 0 ]; then
      ratio=$(awk "BEGIN {printf \"%.2f\", $tk_s / $c_s}")
    else
      ratio="N/A"
    fi
    echo "| $name | $tk_s | $c_s | ${ratio}x |"
    active_idx=$((active_idx + 1))
  done

  echo ""
  echo "## Peak RSS (median, KB)"
  echo ""
  echo "| Benchmark | toke (KB) | C (KB) | Ratio (toke/C) |"
  echo "|-----------|-----------|--------|----------------|"

  active_idx=0
  for name in "${BENCHMARKS[@]}"; do
    if [[ " ${compile_failures[*]:-} " == *" $name "* ]]; then
      echo "| $name | SKIP | SKIP | -- |"
      continue
    fi
    tk_r="${tk_rss_med[$active_idx]}"
    c_r="${c_rss_med[$active_idx]}"
    if [ "$c_r" -gt 0 ]; then
      ratio=$(awk "BEGIN {printf \"%.2f\", $tk_r / $c_r}")
    else
      ratio="N/A"
    fi
    echo "| $name | $tk_r | $c_r | ${ratio}x |"
    active_idx=$((active_idx + 1))
  done

} > "$RESULT_FILE"

printf "${GREEN}Results written to:${RST} %s\n" "$RESULT_FILE"

# Also print the table to stdout
printf "\n"
cat "$RESULT_FILE"
