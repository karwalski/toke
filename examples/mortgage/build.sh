#!/usr/bin/env bash
# Build the multi-module mortgage example into a single binary.
# Since toke 2.x, `tkc <files> -o <bin>` links multiple modules into one binary
# (files listed in dependency order: imported modules before their importers).
set -euo pipefail
cd "$(dirname "$0")"
TKC="${TKC:-tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-/Users/matthew.watt/tk/toke/src/stdlib}"
"$TKC" model.tk calc.tk main.tk -o "${1:-mortgage}"
echo "Built ${1:-mortgage}"
