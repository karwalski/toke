#!/usr/bin/env bash
# C511_file_readers_take_fs_read.sh -- toon.load, yaml.load, md.renderfile,
# crypto.sha256file and crypto.sha256verify read the file they are given
# only under an fs.read grant (story 121.42 item 5; ADR-0010).
#
# THE DEFECT.  Each of these opens a program-chosen path -- toon_glue.c and
# yaml_glue.c through file.c's file_read(), md.c and crypto.c with fopen() --
# and none of them passed TK_REQUIRE(TK_CAP_FS_READ): a binary compiled
# --cap-enforce and granted nothing read the file anyway.  Measured on main
# e1cef7da: all five probes below exited 0 with no grant.  The sink gate
# (scripts/check_capability_sinks.py, 121.22) carried the seven paths as
# known-ungated skiplist lines; they are deleted with the fix.
#
# WHAT IS ASSERTED
#   1. the sink gate is OK on this tree -- none of the five entries is on the
#      skiplist any more, so a missing gate would be a new finding;
#   2. NEGATIVE CONTROL, in a copy of the tree: delete yaml.load's
#      TK_REQUIRE -> the gate FAILS naming tk_yaml_load_w (the injection is
#      asserted to have landed first);
#   3. runtime, --cap-enforce and no grant: each probe exits 1 with CAP001
#      'fs.read'; with the run-time flag --allow-read the same binary exits 0.
#
# Negative control for 3: C511_STDLIB=<a src/stdlib tree> compiles the probes
# against it (via TKC_STDLIB_DIR).  With main's tree every "no grant" case
# fails (exit 0).
#
# Story: 121.42 (5)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
GATE="${REPO_ROOT}/scripts/check_capability_sinks.py"
S="${C511_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c511_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C511: toon.load / yaml.load / md.renderfile / crypto.sha256file|verify take fs.read (121.42)"
echo "-----------------------------------------------------------------------------------------"

# ── 1: the gate on this tree ──────────────────────────────────────────────
if python3 "${GATE}" >"${WORK}/g1.out" 2>&1; then
    pass "1 gate OK: $(tail -1 "${WORK}/g1.out" | cut -c1-100)"
else
    fail "1 gate failed on this tree"; sed 's/^/      | /' "${WORK}/g1.out" | head -20
fi
if /usr/bin/grep -a -q -E '^ungated \| (tk_toon_load_w|tk_yaml_load_w|tk_md_renderfile_w|tk_crypto_sha256file_w|tk_crypto_sha256verify_w) ' \
        "${REPO_ROOT}/scripts/check_capability_sinks_skiplist.txt"; then
    fail "1 a gated entry is still on the skiplist as ungated"
else
    pass "1 none of the five entries is on the skiplist"
fi

# ── 2: negative control in a copy ─────────────────────────────────────────
rm -rf "${WORK}/tree"; mkdir -p "${WORK}/tree/src" "${WORK}/tree/scripts"
cp -R "${REPO_ROOT}/src/stdlib" "${WORK}/tree/src/stdlib"
cp "${REPO_ROOT}/src/stdlib_tables.c" "${REPO_ROOT}/src/llvm.c" "${WORK}/tree/src/"
cp -R "${REPO_ROOT}/stdlib" "${WORK}/tree/stdlib"
f="${WORK}/tree/src/stdlib/yaml_glue.c"
awk '
    index($0, "tk_yaml_load_w(") && /^int64_t/ { infn = 1 }
    infn && !done && /TK_REQUIRE(_[A-Z]+)?\(/ { done = 1; infn = 0; next }
    { print }' "${f}" >"${f}.new" && mv "${f}.new" "${f}"
if cmp -s "${f}" "${REPO_ROOT}/src/stdlib/yaml_glue.c"; then
    fail "2 injection did not land (yaml_glue.c unchanged) -- control proves nothing"
elif python3 "${GATE}" --root "${WORK}/tree" >"${WORK}/g2.out" 2>&1; then
    fail "2 gate stayed OK with tk_yaml_load_w's TK_REQUIRE removed"
elif /usr/bin/grep -a -q '^UNGATED: tk_yaml_load_w ' "${WORK}/g2.out"; then
    pass "2 tk_yaml_load_w's TK_REQUIRE removed -> gate FAILS: $(/usr/bin/grep -a -m1 '^UNGATED: tk_yaml_load_w ' "${WORK}/g2.out" | cut -c1-90)"
else
    fail "2 gate failed but did not name tk_yaml_load_w"; sed 's/^/      | /' "${WORK}/g2.out" | head -8
fi

# ── 3: the runtime ────────────────────────────────────────────────────────
cd "${WORK}"
P="${WORK}/probe"; mkdir -p "${P}"
printf 'data[1]{a}:\n1\n' >"${P}/x.toon"
printf 'a: 1\n' >"${P}/x.yaml"
printf '# hi\n' >"${P}/x.md"
mk() {  # $1 name  $2 import  $3 body
    printf 'm=%s;\n%s\nf=main():i64{\n  %s\n};\n' "$1" "$2" "$3" >"$1.tk"
}
mk toonload 'i=toon:std.toon;'     "let r=toon.load(\"${P}/x.toon\");<0"
mk yamlload 'i=yaml:std.yaml;'     "let r=yaml.load(\"${P}/x.yaml\");<0"
mk mdfile   'i=md:std.md;'         "let r=mt md.renderfile(\"${P}/x.md\") {\$ok:v 0;\$err:e 3};<r"
mk shafile  'i=crypto:std.crypto;' "let h=crypto.sha256file(\"${P}/x.md\");<0"
mk shaver   'i=crypto:std.crypto;' "let ok=crypto.sha256verify(\"${P}/x.md\";\"00\");<0"
for p in toonload yamlload mdfile shafile shaver; do
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --cap-enforce "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        fail "3 compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; continue
    fi
    rc=0; ./"${p}" >"${p}.out" 2>"${p}.err" || rc=$?
    if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "CAP001: capability 'fs.read'" "${p}.err"; then
        pass "3 ${p}, no grant: exit 1, CAP001 fs.read"
    else
        fail "3 ${p}, no grant: exit ${rc} (the file was read with no fs.read grant)"
    fi
    rc=0; ./"${p}" --allow-read >"${p}.out2" 2>"${p}.err2" || rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "3 ${p} --allow-read: exit 0"
    else
        fail "3 ${p} --allow-read: exit ${rc}"; sed 's/^/      | /' "${p}.err2" | head -3
    fi
done

echo "-----------------------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
