#!/usr/bin/env bash
# C1102 -- the toke.gbnf mask-cost micro-benchmark runs and reports counts (131.52).
#
# scripts/gbnf_bench.py measures grammar-constrained decoding cost as
# operation counts (Earley items per character, character-mask size, and with
# a vocabulary the trie-walk mask), for toke.gbnf and for a JSON grammar over
# the same programs.  Nothing is gated on time; this smoke test only pins that
# the benchmark runs character-level offline, that both grammars accept their
# own benchmark inputs (it raises otherwise), and that the counts are sane.
#
# Story: 131.52
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
cd "${REPO_ROOT}" || exit 1
OUT="$(mktemp)"; trap 'rm -f "${OUT}"' EXIT
python3 scripts/gbnf_bench.py --programs 3 --no-vocab --json > "${OUT}" 2>&1 || { cat "${OUT}"; echo "FAIL: C1102 benchmark did not run"; exit 1; }
python3 - "${OUT}" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for k in ("toke", "json"):
    r = d[k]
    assert r["programs"] == 3, (k, r["programs"])
    assert r["chars"] > 0 and r["earley_items_per_char"]["median"] > 0, k
    assert 0 < r["char_mask_size"]["median"] <= 98, k
    assert "token_positions" not in r, "--no-vocab must not walk a vocabulary"
assert d["unit"].startswith("operation counts")
print("toke items/char median", d["toke"]["earley_items_per_char"]["median"],
      "| json items/char median", d["json"]["earley_items_per_char"]["median"])
PY
rc=$?
[ $rc -eq 0 ] && echo "PASS: C1102 gbnf mask benchmark smoke" || { echo "FAIL: C1102"; exit 1; }
