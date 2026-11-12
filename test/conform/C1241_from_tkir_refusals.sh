#!/usr/bin/env bash
# C1241_from_tkir_refusals.sh -- every .tkir that `--from-tkir` cannot build
# is refused with a structured E9021 diagnostic, never a crash or a guess
# (story 76.1.6c; AGENTS.md §6).
#
# A .tkir is input from disk, so the loader treats it as untrusted: each
# section is bounds-checked, every node kind, operator and span is validated
# against the recorded source length, child counts are bounded by the bytes
# left, references must name an earlier, already-closed node (no cycles), and
# the node count must match the tree exactly.  A file written before format
# 1.2 has no AST section -- the 1.1 code section cannot be lowered faithfully
# (a LOAD names no variable, floats are truncated, match is not encoded) --
# and is refused with a message that says to re-emit it.
#
# Each case below derives a damaged file from a good one and asserts: exit 1,
# a JSON diagnostic with error_code E9021, the expected message fragment, and
# no signal.  The last case checks that the source-text modes are a usage
# error (exit 3) with --from-tkir rather than reading or rewriting the .tkir.
#
# Negative control (run 2026-11-10): disable the `d->open[idx]` test in
# dec_node -- the cycle case builds a self-containing tree and tkc dies with
# SIGSEGV (rc 139); disable the `kind > NODE_SPAWN_EXPR` test -- the bad-kind
# case gets only the vaguer "the root is not a program" (and a bad kind on any
# other node would reach the checker unvalidated).  2 FAIL either way.
#
# Story: 76.1.6c

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1241_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1241: --from-tkir refuses what it cannot build, with E9021"
echo "------------------------------------------------------------"

cat > "${WORK}/ok.tk" <<'EOF'
m=ok;
f=main():i64{let a=40;<a+2};
EOF
if ! "${TKC}" "${WORK}/ok.tk" --emit-tkir --out "${WORK}/ok.tkir" >"${WORK}/emit.log" 2>&1; then
    fail "cannot emit the base .tkir: $(head -c 300 "${WORK}/emit.log")"
    echo ""; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi
if "${TKC}" --from-tkir "${WORK}/ok.tkir" --out "${WORK}/ok" >"${WORK}/ok.log" 2>&1 &&
   { "${WORK}/ok" </dev/null; [ $? -eq 42 ]; }; then
    pass "the undamaged .tkir builds and exits 42"
else
    fail "the undamaged .tkir: $(head -c 300 "${WORK}/ok.log")"
fi

# Derive the damaged files.  Layout: "TKIR" vmaj vmin flags u16 nsec, then
# (tag u8, len u32, body)*; AST body = profile u8, count u32, entries.
python3 - "${WORK}" <<'PY'
import struct, sys, os
w = sys.argv[1]
b = open(os.path.join(w, "ok.tkir"), "rb").read()
head, nsec = b[:7], struct.unpack_from("<H", b, 7)[0]
secs, p = [], 9
for _ in range(nsec):
    tag, ln = b[p], struct.unpack_from("<I", b, p + 1)[0]
    secs.append((tag, b[p + 5:p + 5 + ln])); p += 5 + ln
def build(ss, minor=None):
    h = bytearray(head)
    if minor is not None: h[5] = minor
    out = bytes(h) + struct.pack("<H", len(ss))
    for t, body in ss: out += bytes([t]) + struct.pack("<I", len(body)) + body
    return out
def with_ast(body):
    return build([(t, body if t == 0x09 else s) for t, s in secs])
ast = dict(secs)[0x09]
def put(name, data): open(os.path.join(w, name), "wb").write(data)
put("v11.tkir", build([(t, s) for t, s in secs if t <= 0x06], minor=1))
a = bytearray(ast); a[6] = 0xFF;                        put("kind.tkir", with_ast(bytes(a)))
a = bytearray(ast); struct.pack_into("<I", a, 6 + 3 + 12, 0x7FFFFFF0); put("span.tkir", with_ast(bytes(a)))
a = bytearray(ast); struct.pack_into("<I", a, 1, struct.unpack_from("<I", a, 1)[0] + 1); put("count.tkir", with_ast(bytes(a)))
a = bytearray(ast); struct.pack_into("<I", a, 6 + 3 + 32, 0x00FFFFFF); put("kids.tkir", with_ast(bytes(a)))
# PROGRAM node whose only child is a reference to itself.
node = bytes([0x01, 0]) + struct.pack("<H", 0) + b"\0" * 32 + struct.pack("<I", 1)
put("cycle.tkir", with_ast(bytes([0]) + struct.pack("<I", 1) + node + bytes([0x02]) + struct.pack("<I", 0)))
put("trunc.tkir", b[:len(b) // 2])
put("garbage.tkir", b"this is not a tkir file at all\n")
PY

check() {   # name file fragment
    local name="$1" f="$2" frag="$3"
    "${TKC}" --from-tkir "${f}" --out "${WORK}/x" >"${WORK}/o.log" 2>&1
    local rc=$?
    if [ "${rc}" -eq 1 ] &&
       /usr/bin/grep -q '"error_code":"E9021"' "${WORK}/o.log" &&
       /usr/bin/grep -q -- "${frag}" "${WORK}/o.log" && [ ! -e "${WORK}/x" ]; then
        pass "${name}: E9021 (${frag})"
    else
        fail "${name}: rc=${rc} $(head -c 400 "${WORK}/o.log")"
    fi
    rm -f "${WORK}/x"
}

check "missing file"        "${WORK}/nope.tkir"    "cannot open"
check "not a .tkir"         "${WORK}/garbage.tkir" "invalid magic"
check "truncated"           "${WORK}/trunc.tkir"   "failed to read .tkir file"
check "format 1.1 (no AST)" "${WORK}/v11.tkir"     "no AST section (format 1.1"
check "unknown node kind"   "${WORK}/kind.tkir"    "unknown node kind"
check "span outside source" "${WORK}/span.tkir"    "outside the source text"
check "node count mismatch" "${WORK}/count.tkir"   "node count does not fit"
check "child count too big" "${WORK}/kids.tkir"    "child count exceeds the section"
check "reference cycle"     "${WORK}/cycle.tkir"   "a cycle"

"${TKC}" --from-tkir "${WORK}/ok.tkir" --fmt >"${WORK}/u.log" 2>&1
rc=$?
if [ "${rc}" -eq 3 ] && /usr/bin/grep -q "work on source text" "${WORK}/u.log"; then
    pass "--from-tkir --fmt is a usage error (exit 3)"
else
    fail "--from-tkir --fmt: rc=${rc} $(head -c 300 "${WORK}/u.log")"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
