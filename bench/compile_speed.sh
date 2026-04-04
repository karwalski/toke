#!/usr/bin/env bash
# compile_speed.sh — Compiler speed benchmarks: tkc vs tcc/zig cc.
# Measures compilation time (not execution time) for each benchmark program.
# Separates tkc frontend (lex/parse/name-resolution/type-check) from backend
# (LLVM IR emission + object code + linking).
#
# Usage: ./bench/compile_speed.sh [iterations]
#   iterations: number of times to compile each program (default: 5)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROG_DIR="$SCRIPT_DIR/programs"
C_DIR="$SCRIPT_DIR/programs/c"
BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"
TKC="$ROOT_DIR/tkc"
CC="${CC:-cc}"
ITERS="${1:-5}"

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
  GREEN='\033[32m' RED='\033[31m' CYAN='\033[36m' YELLOW='\033[33m'
else
  BOLD='' DIM='' RST='' GREEN='' RED='' CYAN='' YELLOW=''
fi

# ── Detect available C compilers ────────────────────────────────────
HAS_TCC=0
HAS_ZIG=0
TCC_BIN=""
ZIG_BIN=""

if command -v tcc >/dev/null 2>&1; then
  HAS_TCC=1
  TCC_BIN="$(command -v tcc)"
fi
if command -v zig >/dev/null 2>&1; then
  HAS_ZIG=1
  ZIG_BIN="$(command -v zig)"
fi

printf "${BOLD}Compiler Speed Benchmarks${RST}\n"
printf "  tkc      : %s\n" "$($TKC --version 2>&1)"
printf "  cc       : %s\n" "$($CC --version 2>&1 | head -1)"
if [ "$HAS_TCC" -eq 1 ]; then
  printf "  tcc      : %s\n" "$($TCC_BIN -v 2>&1 | head -1)"
fi
if [ "$HAS_ZIG" -eq 1 ]; then
  printf "  zig cc   : %s\n" "$($ZIG_BIN version 2>&1)"
fi
printf "  runs     : %d per benchmark\n" "$ITERS"
printf "  date     : %s\n\n" "$(date -u '+%Y-%m-%d %H:%M UTC')"

# ── Helper: median of values ────────────────────────────────────────
median() {
  local -a sorted=($(printf '%s\n' "$@" | sort -n))
  local n=${#sorted[@]}
  echo "${sorted[$((n / 2))]}"
}

# ── Helper: time a command in milliseconds ──────────────────────────
# Returns wall-clock time in ms (integer).
time_ms() {
  local start end elapsed_ms
  # Use perl for sub-millisecond precision (available on macOS by default)
  start=$(perl -MTime::HiRes=time -e 'printf "%.6f", time()')
  "$@" >/dev/null 2>&1
  end=$(perl -MTime::HiRes=time -e 'printf "%.6f", time()')
  elapsed_ms=$(perl -e "printf '%.0f', ($end - $start) * 1000")
  echo "$elapsed_ms"
}

# ── Measure compilation times ───────────────────────────────────────
printf "${CYAN}Measuring compilation times (%d iterations each)...${RST}\n\n" "$ITERS"

# Result arrays
declare -a names_ok
declare -a tkc_frontend_med tkc_full_med tkc_backend_med
declare -a cc_med tcc_med zig_med

for name in "${BENCHMARKS[@]}"; do
  tk_src="$PROG_DIR/${name}.tk"
  c_src="$C_DIR/${name}.c"

  if [ ! -f "$tk_src" ]; then
    printf "  ${RED}SKIP${RST} %s — no .tk source\n" "$name"
    continue
  fi
  if [ ! -f "$c_src" ]; then
    printf "  ${RED}SKIP${RST} %s — no .c source in c/\n" "$name"
    continue
  fi

  printf "  %-20s " "$name"

  # -- tkc frontend only (--check): lex + parse + name resolution + type check
  fe_samples=()
  for ((r = 0; r < ITERS; r++)); do
    ms=$(time_ms "$TKC" --check "$tk_src")
    fe_samples+=("$ms")
  done
  fe_med=$(median "${fe_samples[@]}")

  # -- tkc full compile (frontend + LLVM IR + object + link)
  full_samples=()
  for ((r = 0; r < ITERS; r++)); do
    ms=$(time_ms "$TKC" -O2 "$tk_src" --out "$BUILD_DIR/${name}_speed_tk")
    full_samples+=("$ms")
  done
  full_med=$(median "${full_samples[@]}")

  # -- tkc backend time = full - frontend
  be_med=$((full_med - fe_med))
  if [ "$be_med" -lt 0 ]; then be_med=0; fi

  # -- cc (system compiler)
  cc_samples=()
  for ((r = 0; r < ITERS; r++)); do
    ms=$(time_ms "$CC" -O2 -o "$BUILD_DIR/${name}_speed_cc" "$c_src")
    cc_samples+=("$ms")
  done
  cc_m=$(median "${cc_samples[@]}")

  # -- tcc (if available)
  tcc_m="-"
  if [ "$HAS_TCC" -eq 1 ]; then
    tcc_samples=()
    for ((r = 0; r < ITERS; r++)); do
      ms=$(time_ms "$TCC_BIN" -o "$BUILD_DIR/${name}_speed_tcc" "$c_src")
      tcc_samples+=("$ms")
    done
    tcc_m=$(median "${tcc_samples[@]}")
  fi

  # -- zig cc (if available)
  zig_m="-"
  if [ "$HAS_ZIG" -eq 1 ]; then
    zig_samples=()
    for ((r = 0; r < ITERS; r++)); do
      ms=$(time_ms "$ZIG_BIN" cc -O2 -o "$BUILD_DIR/${name}_speed_zig" "$c_src")
      zig_samples+=("$ms")
    done
    zig_m=$(median "${zig_samples[@]}")
  fi

  printf "${GREEN}OK${RST}  fe:%3d ms  be:%3d ms  full:%3d ms  cc:%3d ms" \
    "$fe_med" "$be_med" "$full_med" "$cc_m"
  if [ "$HAS_TCC" -eq 1 ]; then printf "  tcc:%s ms" "$tcc_m"; fi
  if [ "$HAS_ZIG" -eq 1 ]; then printf "  zig:%s ms" "$zig_m"; fi
  printf "\n"

  names_ok+=("$name")
  tkc_frontend_med+=("$fe_med")
  tkc_backend_med+=("$be_med")
  tkc_full_med+=("$full_med")
  cc_med+=("$cc_m")
  tcc_med+=("$tcc_m")
  zig_med+=("$zig_m")
done

printf "\n"

# ── Generate results table ──────────────────────────────────────────
RESULT_FILE="$RESULTS_DIR/compile_speed_$(date '+%Y%m%d_%H%M%S').md"

# Build header with optional columns
hdr_extra=""
sep_extra=""
if [ "$HAS_TCC" -eq 1 ]; then hdr_extra+=" tcc (ms) |"; sep_extra+="----------|"; fi
if [ "$HAS_ZIG" -eq 1 ]; then hdr_extra+=" zig cc (ms) |"; sep_extra+="-------------|"; fi

{
  echo "# Compiler Speed Benchmark Results"
  echo ""
  echo "- **Date**: $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "- **tkc**: $($TKC --version 2>&1)"
  echo "- **cc**: $($CC --version 2>&1 | head -1)"
  if [ "$HAS_TCC" -eq 1 ]; then echo "- **tcc**: $($TCC_BIN -v 2>&1 | head -1)"; fi
  if [ "$HAS_ZIG" -eq 1 ]; then echo "- **zig**: $($ZIG_BIN version 2>&1)"; fi
  echo "- **Iterations**: $ITERS (median reported)"
  echo "- **Optimisation**: -O2 for tkc, cc, zig cc; default for tcc"
  echo "- **Platform**: $(uname -srm)"
  echo ""
  echo "## Methodology"
  echo ""
  echo "- **tkc frontend**: \`tkc --check\` (lex + parse + name resolution + type check, no codegen)"
  echo "- **tkc full**: \`tkc -O2\` (frontend + LLVM IR emission + object code + linking)"
  echo "- **tkc backend**: full - frontend (LLVM IR + opt + codegen + link)"
  echo "- **cc/tcc/zig cc**: full compile of equivalent C source"
  echo ""
  echo "## Compilation Time (median, ms)"
  echo ""
  printf "| Benchmark | tkc frontend (ms) | tkc backend (ms) | tkc full (ms) | cc (ms) |"
  printf " %s" "$hdr_extra"
  echo ""
  printf "|-----------|-------------------|------------------|---------------|---------|"
  printf " %s" "$sep_extra"
  echo ""

  for ((i = 0; i < ${#names_ok[@]}; i++)); do
    printf "| %s | %s | %s | %s | %s |" \
      "${names_ok[$i]}" \
      "${tkc_frontend_med[$i]}" \
      "${tkc_backend_med[$i]}" \
      "${tkc_full_med[$i]}" \
      "${cc_med[$i]}"
    if [ "$HAS_TCC" -eq 1 ]; then printf " %s |" "${tcc_med[$i]}"; fi
    if [ "$HAS_ZIG" -eq 1 ]; then printf " %s |" "${zig_med[$i]}"; fi
    echo ""
  done

  echo ""
  echo "## Summary"
  echo ""

  # Compute averages
  total_fe=0 total_be=0 total_full=0 total_cc=0
  for ((i = 0; i < ${#names_ok[@]}; i++)); do
    total_fe=$((total_fe + tkc_frontend_med[i]))
    total_be=$((total_be + tkc_backend_med[i]))
    total_full=$((total_full + tkc_full_med[i]))
    total_cc=$((total_cc + cc_med[i]))
  done
  n=${#names_ok[@]}
  if [ "$n" -gt 0 ]; then
    avg_fe=$((total_fe / n))
    avg_be=$((total_be / n))
    avg_full=$((total_full / n))
    avg_cc=$((total_cc / n))
    echo "- **Average tkc frontend**: ${avg_fe} ms"
    echo "- **Average tkc backend (LLVM)**: ${avg_be} ms"
    echo "- **Average tkc full**: ${avg_full} ms"
    echo "- **Average cc -O2**: ${avg_cc} ms"
    if [ "$avg_full" -gt 0 ]; then
      fe_pct=$(awk "BEGIN {printf \"%.0f\", $avg_fe * 100 / $avg_full}")
      be_pct=$(awk "BEGIN {printf \"%.0f\", $avg_be * 100 / $avg_full}")
      echo "- **Frontend/backend split**: ${fe_pct}% frontend, ${be_pct}% backend"
    fi
    if [ "$avg_cc" -gt 0 ]; then
      ratio=$(awk "BEGIN {printf \"%.2f\", $avg_full / $avg_cc}")
      echo "- **tkc/cc ratio**: ${ratio}x"
    fi
  fi

} > "$RESULT_FILE"

printf "${GREEN}Results written to:${RST} %s\n" "$RESULT_FILE"

# Print table to stdout
printf "\n"
cat "$RESULT_FILE"
