#!/usr/bin/env bash
# C2056_cap001_fix_names_manifest_key.sh -- CAP001's JSON `fix` names the
# tkc.toml key that src/config.c accepts (story 138.53).
#
# THE BUG.  On a binary with no baked grant set, CAP001's JSON line read
# `"fix":"grant --allow-write or tkc.toml [capabilities] fs.write=true"`.  The
# manifest key is `fs_write`; `fs.write = true` is refused by tkc as "unknown
# capability 'fs.write'", so half of the fix was wrong every time it was
# emitted (AGENTS §6: a fix only where it is right in every case).
#
# THE CHANGE.  The manifest half names the key, spelled as the narrowing
# binary's fix already spells it: `tkc.toml [capabilities] <key> = true`.  The
# text lines are unchanged.
#
# WHAT IS ASSERTED, for each class (fs.read, fs.write, env.write, net,
# process.spawn) on a binary with nothing baked:
#   a. the JSON fix is exactly
#      "grant <flag> or tkc.toml [capabilities] <key> = true"
#   b. the manifest half applied (that line in a tkc.toml, rebuilt): tkc
#      accepts it and the call is admitted
#   c. the run-time half applied (<flag>): the call is admitted
# and once: the old spelling (fs.write = true) is still refused by tkc, so the
# old fix could not have worked.
#
# NEGATIVE CONTROL: TKC=<a tkc before 138.53> -- every (a) FAILS (the fix
# names the class, fs.write=true); (b), (c) and the refusal pass.
#
# Story: 138.53
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2056_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C2056: CAP001's fix names the tkc.toml key (138.53)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/probe.tk" <<'TK'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=env:std.env;
i=ws:std.ws;
i=process:std.process;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let p=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="read"){
    let r=mt file.read(p) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{};
  if(op=="write"){
    let r=mt file.write(p;"x") {$ok:v 1;$err:e 2};
    io.println("write-done \(r)");
  }el{};
  if(op=="env"){
    let v=env.set(p;"1");
    io.println("env-done");
  }el{};
  if(op=="connect"){
    let r=mt ws.connect(p) {$ok:c 1;$err:e 2};
    io.println("connect-done \(r)");
  }el{};
  if(op=="spawn"){
    let h=process.spawn(@(p));
    io.println("spawn-done");
  }el{};
  <0
};
TK
echo a >"${WORK}/in.txt"

build() {   # NAME [tkc args]
    local name="$1"; shift
    ( cd "${WORK}" && "${TKC}" "$@" --out "${WORK}/${name}" probe.tk ) >"${WORK}/${name}.cl" 2>&1
}
jfix() {
    python3 -I - "${WORK}/e" <<'PY'
import json, sys
rec = None
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.strip()
    if line.startswith("{"):
        try: rec = json.loads(line)
        except ValueError: print("<badjson>"); sys.exit(0)
if rec is None: print("<nojson>")
else: print(rec.get("diagnostic_id", "?") + "|" + rec.get("fix", "<absent>"))
PY
}
admitted() { local rc=0; ( cd "${WORK}" && "$@" ) >"${WORK}/o2" 2>"${WORK}/e2" || rc=$?
             [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o2"; }

build plain || { fail "build plain: $(head -1 "${WORK}/plain.cl")"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }

# class flag key op arg
check() {
    local cls="$1" flag="$2" key="$3" op="$4" arg="$5"
    ( cd "${WORK}" && TK_DIAG_JSON=1 "${WORK}/plain" "${op}" "${arg}" ) >"${WORK}/o" 2>"${WORK}/e"
    local want="CAP001|grant ${flag} or tkc.toml [capabilities] ${key} = true" got
    got="$(jfix)"
    if [ "${got}" = "${want}" ]; then pass "${cls} a: fix = ${got#CAP001|}"
    else fail "${cls} a: got '${got}', wanted '${want}'"; fi
    printf '[capabilities]\n%s = true\n' "${key}" >"${WORK}/${key}.toml"
    if build "m_${key}" "--config=${WORK}/${key}.toml" && admitted "${WORK}/m_${key}" "${op}" "${arg}"; then
        pass "${cls} b: '${key} = true' in tkc.toml, rebuilt: admitted"
    else fail "${cls} b: manifest half: $(head -1 "${WORK}/m_${key}.cl" 2>/dev/null) $(head -1 "${WORK}/e2" 2>/dev/null)"; fi
    if admitted "${WORK}/plain" "${op}" "${arg}" "${flag}"; then
        pass "${cls} c: ${flag} at run time: admitted"
    else fail "${cls} c: run-time half: $(head -1 "${WORK}/e2")"; fi
}
check fs.read       --allow-read  fs_read       read    "${WORK}/in.txt"
check fs.write      --allow-write fs_write      write   "${WORK}/out.txt"
check env.write     --allow-env   env_write     env     TOKE_C2056
# the connect itself is refused (closed port); only the grant is under test
check net           --allow-net   net           connect ws://127.0.0.1:48250/
check process.spawn --allow-run   process_spawn spawn   true

printf '[capabilities]\nfs.write = true\n' >"${WORK}/dotted.toml"
if ! build dotted "--config=${WORK}/dotted.toml" && /usr/bin/grep -q "unknown capability 'fs.write'" "${WORK}/dotted.cl"; then
    pass "the class spelling (fs.write = true) is refused by tkc: the old fix could not work"
else fail "fs.write = true was not refused: $(head -1 "${WORK}/dotted.cl")"; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
