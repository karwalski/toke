#!/usr/bin/env bash
# optimization_ladder.sh — Benchmark the effect of LLVM optimization levels
# on compiled toke programs.
#
# Compiles each bench/programs/*.tk file to LLVM IR via tkc, then compiles the
# IR with clang at -O0, -O1, -O2, -O3, and -O2 -flto=thin (ThinLTO).
# Reports wall-clock execution time and marginal gains between each level.
#
# Usage: ./bench/optimization_ladder.sh [iterations]
#   iterations: number of times to run each binary (default: 10)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROG_DIR="$SCRIPT_DIR/programs"
BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"
TKC="$ROOT_DIR/tkc"
CLANG="${CLANG:-clang}"
ITERS="${1:-10}"

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

OPT_LEVELS=("-O0" "-O1" "-O2" "-O3" "-O2 -flto=thin")
OPT_LABELS=("-O0" "-O1" "-O2" "-O3" "-flto")

mkdir -p "$BUILD_DIR" "$RESULTS_DIR"

# ── Colours (disabled when not a tty) ───────────────────────────────
if [ -t 1 ]; then
  BOLD='\033[1m'  DIM='\033[2m'  RST='\033[0m'
  GREEN='\033[32m' RED='\033[31m' CYAN='\033[36m'
else
  BOLD='' DIM='' RST='' GREEN='' RED='' CYAN=''
fi

printf "${BOLD}Optimization Ladder Benchmark${RST}\n"
printf "  tkc      : %s\n" "$($TKC --version 2>&1)"
printf "  clang    : %s\n" "$($CLANG --version 2>&1 | head -1)"
printf "  runs     : %d per binary\n" "$ITERS"
printf "  levels   : %s\n" "${OPT_LABELS[*]}"
printf "  date     : %s\n\n" "$(date -u '+%Y-%m-%d %H:%M UTC')"

# ── Helper: median of values ────────────────────────────────────────
median() {
  local -a sorted=($(printf '%s\n' "$@" | sort -n))
  local n=${#sorted[@]}
  echo "${sorted[$((n / 2))]}"
}

# ── Helper: time a binary in milliseconds ───────────────────────────
time_ms() {
  local start end
  start=$(perl -MTime::HiRes=time -e 'printf "%.6f", time()')
  "$@" >/dev/null 2>&1
  end=$(perl -MTime::HiRes=time -e 'printf "%.6f", time()')
  perl -e "printf '%.0f', ($end - $start) * 1000"
}

# ── Emit LLVM IR and compile at each opt level ──────────────────────
printf "${CYAN}Compiling...${RST}\n"
compile_failures=()

for name in "${BENCHMARKS[@]}"; do
  tk_src="$PROG_DIR/${name}.tk"
  ll_file="$BUILD_DIR/${name}.ll"

  if [ ! -f "$tk_src" ]; then
    printf "  ${RED}SKIP${RST} %s — no .tk source\n" "$name"
    compile_failures+=("$name")
    continue
  fi

  # Emit LLVM IR (unoptimised — tkc at -O0 so clang controls optimisation)
  if ! "$TKC" -O0 --emit-llvm "$tk_src" --out "$ll_file" 2>/dev/null; then
    printf "  ${RED}FAIL${RST} %s — tkc --emit-llvm failed\n" "$name"
    compile_failures+=("$name")
    continue
  fi

  # Compile at each optimisation level
  level_ok=1
  for i in "${!OPT_LEVELS[@]}"; do
    label="${OPT_LABELS[$i]}"
    flags="${OPT_LEVELS[$i]}"
    bin="$BUILD_DIR/${name}_${label//[- ]/_}"
    # shellcheck disable=SC2086
    if ! $CLANG $flags -o "$bin" "$ll_file" 2>/dev/null; then
      printf "  ${RED}FAIL${RST} %s — clang %s failed\n" "$name" "$label"
      level_ok=0
      break
    fi
  done

  if [ "$level_ok" -eq 0 ]; then
    compile_failures+=("$name")
    continue
  fi

  printf "  ${GREEN}OK${RST}   %s\n" "$name"
done
printf "\n"

# ── Run benchmarks ──────────────────────────────────────────────────
printf "${CYAN}Running benchmarks (%d iterations each)...${RST}\n\n" "$ITERS"

# results[bench_idx][level_idx] stored as flat arrays
# For each benchmark we store NUM_LEVELS median values
NUM_LEVELS=${#OPT_LEVELS[@]}
declare -a result_names
declare -a result_times  # flat: result_times[bench*NUM_LEVELS + level]

bench_idx=0
for name in "${BENCHMARKS[@]}"; do
  if [[ " ${compile_failures[*]:-} " == *" $name "* ]]; then
    continue
  fi

  printf "  %-20s " "$name"

  for i in "${!OPT_LEVELS[@]}"; do
    label="${OPT_LABELS[$i]}"
    bin="$BUILD_DIR/${name}_${label//[- ]/_}"

    samples=()
    for ((r = 0; r < ITERS; r++)); do
      ms=$(time_ms "$bin")
      samples+=("$ms")
    done
    med=$(median "${samples[@]}")
    result_times+=("$med")
    printf "%s:%3d ms  " "$label" "$med"
  done

  result_names+=("$name")
  bench_idx=$((bench_idx + 1))
  printf "\n"
done

printf "\n"

# ── Helper: compute marginal gain string ────────────────────────────
# gain_pct(prev_ms, cur_ms) -> string like "-12.3%"
gain_pct() {
  local prev="$1" cur="$2"
  if [ "$prev" -eq 0 ]; then
    echo "N/A"
  else
    perl -e "printf '%.1f%%', (($cur - $prev) / $prev) * 100"
  fi
}

# ── Generate markdown results ───────────────────────────────────────
RESULT_FILE="$RESULTS_DIR/optimization_ladder_$(date '+%Y%m%d_%H%M%S').md"

{
  echo "# Optimization Ladder Results"
  echo ""
  echo "- **Date**: $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "- **tkc**: $($TKC --version 2>&1)"
  echo "- **clang**: $($CLANG --version 2>&1 | head -1)"
  echo "- **Iterations**: $ITERS (median reported)"
  echo "- **Platform**: $(uname -srm)"
  echo ""
  echo "## Methodology"
  echo ""
  echo "Each .tk file is compiled to LLVM IR via \`tkc -O0 --emit-llvm\`, then"
  echo "the IR is compiled with \`clang\` at -O0, -O1, -O2, -O3, and -O2 -flto=thin."
  echo "This isolates the LLVM backend optimisation impact from tkc's own passes."
  echo ""

  # ── Absolute times table ────────────────────────────────────────
  echo "## Wall-Clock Time (median, ms)"
  echo ""
  printf "| Benchmark |"
  for label in "${OPT_LABELS[@]}"; do printf " %s (ms) |" "$label"; done
  echo ""
  printf "|-----------|"
  for _ in "${OPT_LABELS[@]}"; do printf "----------|"; done
  echo ""

  for ((b = 0; b < ${#result_names[@]}; b++)); do
    printf "| %s |" "${result_names[$b]}"
    for ((l = 0; l < NUM_LEVELS; l++)); do
      idx=$((b * NUM_LEVELS + l))
      printf " %s |" "${result_times[$idx]}"
    done
    echo ""
  done

  echo ""

  # ── Marginal gains table ────────────────────────────────────────
  echo "## Marginal Gains (vs previous level)"
  echo ""
  echo "Negative values indicate the program got faster."
  echo ""
  printf "| Benchmark |"
  for ((l = 1; l < NUM_LEVELS; l++)); do
    printf " %s vs %s |" "${OPT_LABELS[$l]}" "${OPT_LABELS[$((l - 1))]}"
  done
  echo ""
  printf "|-----------|"
  for ((l = 1; l < NUM_LEVELS; l++)); do printf "----------------|"; done
  echo ""

  for ((b = 0; b < ${#result_names[@]}; b++)); do
    printf "| %s |" "${result_names[$b]}"
    for ((l = 1; l < NUM_LEVELS; l++)); do
      prev_idx=$((b * NUM_LEVELS + l - 1))
      cur_idx=$((b * NUM_LEVELS + l))
      prev_t="${result_times[$prev_idx]}"
      cur_t="${result_times[$cur_idx]}"
      g=$(gain_pct "$prev_t" "$cur_t")
      printf " %s |" "$g"
    done
    echo ""
  done

  echo ""

  # ── Summary: average times per level ────────────────────────────
  echo "## Summary"
  echo ""
  n=${#result_names[@]}
  if [ "$n" -gt 0 ]; then
    for ((l = 0; l < NUM_LEVELS; l++)); do
      total=0
      for ((b = 0; b < n; b++)); do
        idx=$((b * NUM_LEVELS + l))
        total=$((total + result_times[idx]))
      done
      avg=$((total / n))
      echo "- **Average ${OPT_LABELS[$l]}**: ${avg} ms"
    done

    echo ""

    # Overall gains: each level vs -O0
    echo "### Speedup vs -O0 (average across all benchmarks)"
    echo ""
    total_o0=0
    for ((b = 0; b < n; b++)); do
      idx=$((b * NUM_LEVELS))
      total_o0=$((total_o0 + result_times[idx]))
    done
    avg_o0=$((total_o0 / n))
    for ((l = 1; l < NUM_LEVELS; l++)); do
      total=0
      for ((b = 0; b < n; b++)); do
        idx=$((b * NUM_LEVELS + l))
        total=$((total + result_times[idx]))
      done
      avg=$((total / n))
      if [ "$avg" -gt 0 ]; then
        speedup=$(perl -e "printf '%.2f', $avg_o0 / $avg")
        echo "- **${OPT_LABELS[$l]}**: ${speedup}x faster than -O0"
      fi
    done
  fi

} > "$RESULT_FILE"

printf "${GREEN}Results written to:${RST} %s\n" "$RESULT_FILE"

# Print table to stdout
printf "\n"
cat "$RESULT_FILE"
