#!/usr/bin/env bash
# Build the multi-module mortgage example into a single binary.
#
# toke has no single-command multi-module link yet (the `-o` flag in older
# docs is unimplemented, and `--out` with multiple files emits one binary per
# file — see toke story 114.40). So we emit each module to LLVM IR and link
# them together with the stdlib C sources reported by `--emit-deps`.
set -euo pipefail
cd "$(dirname "$0")"

TKC="${TKC:-tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-/Users/matthew.watt/tk/toke/src/stdlib}"
OUT="${1:-mortgage}"

# 1. Interfaces (so cross-module types resolve during IR emission)
"$TKC" --emit-interface --out model model.tk >/dev/null 2>&1 || true
"$TKC" --emit-interface --out calc  calc.tk  >/dev/null 2>&1 || true

# 2. LLVM IR per module
for m in model calc main; do
  "$TKC" --emit-llvm --out "$m.ll" "$m.tk" >/dev/null 2>&1 || true
done

# 3. Union of stdlib C sources across all modules
CDEPS=$( { "$TKC" --emit-deps main.tk; "$TKC" --emit-deps calc.tk; "$TKC" --emit-deps model.tk; } 2>/dev/null \
        | grep '\.c$' | sort -u | tr '\n' ' ')

# 4. Link (macOS frameworks needed by the crypto glue)
EXTRA=""
case "$(uname)" in Darwin) EXTRA="-framework Security -framework CoreFoundation";; esac
# shellcheck disable=SC2086
clang -O1 -x ir main.ll calc.ll model.ll -x c $CDEPS -o "$OUT" -lm $EXTRA

echo "Built $OUT"
