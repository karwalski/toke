#!/usr/bin/env bash
# Build the multi-module mortgage example into a single binary.
# Since toke 2.x, `tkc <files> -o <bin>` links multiple modules into one binary
# (files listed in dependency order: imported modules before their importers).
#
# TKC defaults to this checkout's compiler (run `make` at the repo root
# first), then to `tkc` on PATH. TKC_STDLIB_DIR needs no default: tkc has
# its stdlib path compiled in, and honours the variable only when it is set.
set -euo pipefail
cd "$(dirname "$0")"
REPO_TKC="../../tkc"
if [ -z "${TKC:-}" ]; then
    if [ -x "${REPO_TKC}" ]; then TKC="${REPO_TKC}"; else TKC=tkc; fi
fi
"$TKC" model.tk calc.tk main.tk -o "${1:-mortgage}"
echo "Built ${1:-mortgage}"
