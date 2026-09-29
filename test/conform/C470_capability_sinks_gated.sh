#!/usr/bin/env bash
# C470_capability_sinks_gated.sh -- every stdlib entry that reaches an fs /
# net / env-write / process-spawn primitive passes a capability check of that
# class on the way (ADR-0010; story 121.22).
#
# THE DEFECT.  124.4c/124.4c2 gated the sinks once, by hand, and nothing
# re-checked them.  Two of the entries they missed, measured on main with a
# binary compiled --cap-enforce and granted nothing:
#   * log.openaccess("/tmp/x/access.log";0;0;0) returned 0 and created the
#     file and its .lock sidecar (fs.write without a grant);
#   * toml.loadfile(path) read the file (fs.read without a grant).
# Both are now gated in their wrappers (log_glue.c, toml_glue.c).  The gate
# scripts/check_capability_sinks.py found them, and found the rest of the
# backlog it carries in scripts/check_capability_sinks_skiplist.txt.
#
# WHAT IS ASSERTED
#   1. the gate is OK on this tree (every sink path gated, exempt, or a
#      known-ungated line; no stale line)
#   2. NEGATIVE CONTROL, in a copy of the tree: delete file.write's
#      TK_REQUIRE -> the gate FAILS naming tk_file_write_w.  The injection is
#      asserted to have landed before the gate runs.
#   3. NEGATIVE CONTROL: delete toml.loadfile's new TK_REQUIRE -> the gate
#      FAILS naming tk_toml_loadfile_w (the fix is what makes it pass)
#   4. runtime, --cap-enforce and no grant: log.openaccess on a file path
#      exits 1 with CAP001 'fs.write' and creates nothing; with the runtime
#      flag --allow-write the same binary opens it (exit 0, file exists)
#   5. runtime: log.openaccess("/dev/stderr") needs no grant (the process's
#      own stream -- ooke's default `[log] access = "-"`)
#   6. runtime, --cap-enforce and no grant: toml.loadfile exits 1 with
#      CAP001 'fs.read'; with --allow-read it loads (exit 0)
#
# Negative control for 4 and 6: C470_STDLIB=<main's src/stdlib> is not
# needed -- on main both binaries exit 0 with no grant (recorded in the
# 121.22 report).
#
# Story: 121.22
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
GATE="${REPO_ROOT}/scripts/check_capability_sinks.py"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c470_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C470: every fs/net/env/process sink is capability-gated (121.22)"
echo "------------------------------------------------------------------------"

# ── 1: the gate on this tree ──────────────────────────────────────────────
if python3 "${GATE}" >"${WORK}/g1.out" 2>&1; then
    pass "1 gate OK: $(tail -1 "${WORK}/g1.out")"
else
    fail "1 gate failed on this tree"; sed 's/^/      | /' "${WORK}/g1.out" | head -20
fi

# ── 2, 3: negative controls in a copy ─────────────────────────────────────
mkcopy() {
    rm -rf "${WORK}/tree"; mkdir -p "${WORK}/tree/src"
    cp -R "${REPO_ROOT}/src/stdlib" "${WORK}/tree/src/stdlib"
    cp "${REPO_ROOT}/src/stdlib_tables.c" "${REPO_ROOT}/src/llvm.c" "${WORK}/tree/src/"
    cp -R "${REPO_ROOT}/stdlib" "${WORK}/tree/stdlib"
}
control() {  # $1 label  $2 file  $3 function  $4 expected entry
    local label="$1" file="$2" fn="$3" want="$4"
    mkcopy
    local f="${WORK}/tree/src/stdlib/${file}"
    # delete the first TK_REQUIRE line inside function $fn
    awk -v fn="${fn}" '
        index($0, fn "(") && /^[a-z]/ { infn = 1 }
        infn && !done && /TK_REQUIRE\(/ { done = 1; infn = 0; next }
        { print }' "${f}" >"${f}.new" && mv "${f}.new" "${f}"
    if cmp -s "${f}" "${REPO_ROOT}/src/stdlib/${file}"; then
        fail "${label} injection did not land (${file}:${fn} unchanged) -- control proves nothing"
        return
    fi
    if python3 "${GATE}" --root "${WORK}/tree" >"${WORK}/g.out" 2>&1; then
        fail "${label} gate stayed OK with ${fn}'s TK_REQUIRE removed"
    elif /usr/bin/grep -a -q "^UNGATED: ${want} " "${WORK}/g.out"; then
        pass "${label} ${fn}'s TK_REQUIRE removed -> gate FAILS: $(/usr/bin/grep -a -m1 "^UNGATED: ${want} " "${WORK}/g.out" | cut -c1-90)"
    else
        fail "${label} gate failed but did not name ${want}"; sed 's/^/      | /' "${WORK}/g.out" | head -8
    fi
}
control 2 file_glue.c tk_file_write_w tk_file_write_w
control 3 toml_glue.c tk_toml_loadfile_w tk_toml_loadfile_w

# ── 4-6: the runtime ──────────────────────────────────────────────────────
cd "${WORK}"
PROBE="${WORK}/probe"; mkdir -p "${PROBE}"
printf 'a = 1\n' >"${PROBE}/x.toml"
cat >tlog.tk <<EOF
m=tlog;
i=log:std.log;
f=main():i64{
  let r=log.openaccess("${PROBE}/logs/access.log";0;0;0);
  <r
};
EOF
cat >tstderr.tk <<'EOF'
m=tstderr;
i=log:std.log;
f=main():i64{
  let r=log.openaccess("/dev/stderr";0;0;0);
  <r
};
EOF
cat >ttoml.tk <<EOF
m=ttoml;
i=toml:std.toml;
f=main():i64{
  let r=mt toml.loadfile("${PROBE}/x.toml") {\$ok:v 0;\$err:e 3};
  <r
};
EOF
built=1
for p in tlog tstderr ttoml; do
    if ! "${TKC}" --cap-enforce "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        fail "compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; built=0
    fi
done
if [ "${built}" -eq 1 ]; then
    rc=0; ./tlog >tlog.out 2>tlog.err || rc=$?
    if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "CAP001: capability 'fs.write'" tlog.err \
       && [ ! -e "${PROBE}/logs/access.log" ] && [ ! -e "${PROBE}/logs" ]; then
        pass "4a log.openaccess, no grant: exit 1, CAP001 fs.write, nothing created"
    else
        fail "4a log.openaccess, no grant: rc=${rc}, created=$( [ -e "${PROBE}/logs" ] && echo yes || echo no)"
        sed 's/^/      | /' tlog.err | head -3
    fi
    rc=0; ./tlog --allow-write >tlog2.out 2>tlog2.err || rc=$?
    if [ "${rc}" -eq 0 ] && [ -f "${PROBE}/logs/access.log" ]; then
        pass "4b log.openaccess --allow-write: exit 0, file created"
    else
        fail "4b log.openaccess --allow-write: rc=${rc}"; sed 's/^/      | /' tlog2.err | head -3
    fi
    rc=0; ./tstderr >ts.out 2>ts.err || rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "5 log.openaccess(\"/dev/stderr\"), no grant: exit 0 (own stream)"
    else
        fail "5 log.openaccess(\"/dev/stderr\"), no grant: rc=${rc}"; sed 's/^/      | /' ts.err | head -3
    fi
    rc=0; ./ttoml >tt.out 2>tt.err || rc=$?
    if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "CAP001: capability 'fs.read'" tt.err; then
        pass "6a toml.loadfile, no grant: exit 1, CAP001 fs.read"
    else
        fail "6a toml.loadfile, no grant: rc=${rc}"; sed 's/^/      | /' tt.err | head -3
    fi
    rc=0; ./ttoml --allow-read >tt2.out 2>tt2.err || rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "6b toml.loadfile --allow-read: exit 0"
    else
        fail "6b toml.loadfile --allow-read: rc=${rc}"; sed 's/^/      | /' tt2.err | head -3
    fi
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
