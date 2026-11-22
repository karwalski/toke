#!/usr/bin/env bash
# C1882_cap_scope_cmd_env.sh -- process.spawn scoped to commands and env.write
# scoped to keys (story 138.19, D1; design note §1, §3.3).
#
# THE DEFECT (loke T-59 point 3).  `process_spawn` covered every executable
# and `env_write` every variable: `--allow-run=/usr/bin/git` let a program
# run anything, and nothing could grant `TZ` without granting `PATH` or
# `LD_PRELOAD`.
#
# THE CHANGE.  A spawn grant is a command: an absolute path, compared with
# the canonical path argv[0] resolves to, or a bare name, resolved on the
# PATH at start-up and pinned there.  process.exec / process.spawndetached
# split a command string and are checked on its first token.  Arguments are
# not scoped.  An env.write grant is a key, or a prefix with one trailing *.
#
# WHAT IS ASSERTED (the "commands" are copies of /usr/bin/true in the work
# directory; nothing else is run)
#   1. an absolute grant admits that path and denies another binary of the
#      same name (b1/tool vs b2/tool)
#   2. a bare-name grant (`tool`, PATH=b1 at start-up) admits `tool` and
#      denies b2/tool; changing PATH at run time (env.set) does not widen it
#   3. process.exec("<cmd> arg") is checked on its first token
#   4. env.set: `TZ` and `TOKE_*` admit TZ and TOKE_HOME, deny TOKEN and PATH
#   5. the whole classes (--allow-run, --allow-env) admit everything, as before
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- every denial FAILS (exit 0).
#
# Story: 138.19
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
TRUE_BIN="$(command -v true 2>/dev/null)"
case "${TRUE_BIN}" in /*) ;; *) TRUE_BIN=/usr/bin/true ;; esac
if [ ! -x "${TRUE_BIN}" ]; then echo "CONFORM-SKIP: no true(1) binary to copy"; exit 0; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1882_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C1882: process.spawn scoped to commands, env.write to keys (138.19 D1)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/cmdprobe.tk" <<'EOF'
m=cmdprobe;
i=io:std.io;
i=args:std.args;
i=process:std.process;
i=env:std.env;
f=main():i64{
  let op=mt args.get(1) {$ok:s s;$err:e ""};
  let a=mt args.get(2) {$ok:s s;$err:e ""};
  if(op=="spawn"){
    let p=mt process.spawn(@(a)) {$ok:h 1;$err:e 2};
    io.println("spawn-done \(p)");
  }el{};
  if(op=="exec"){
    let o=process.exec(a);
    io.println("exec-done");
  }el{};
  if(op=="setpath"){
    let v=env.set("PATH";"/nonexistent-dir");
    let p=mt process.spawn(@(a)) {$ok:h 1;$err:e 2};
    io.println("setpath-done \(p)");
  }el{};
  if(op=="env"){
    let v=env.set(a;"1");
    io.println("env-done");
  }el{};
  <0
};
EOF
if ! "${TKC}" --out "${WORK}/cmdprobe" "${WORK}/cmdprobe.tk" >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: probe did not build"; sed 's/^/      | /' "${WORK}/build.log" | head -5; exit 1
fi
P="${WORK}/cmdprobe"
mkdir -p "${WORK}/b1" "${WORK}/b2"
cp "${TRUE_BIN}" "${WORK}/b1/tool"; cp "${TRUE_BIN}" "${WORK}/b2/tool"
B1="${WORK}/b1/tool"; B2="${WORK}/b2/tool"
PATH1="${WORK}/b1:/usr/bin:/bin"

run() {   # LABEL admit|deny CLASS ARGS...
    local label="$1" want="$2" cls="$3"; shift 3
    local rc=0
    ( cd "${WORK}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${want}" = admit ] && [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: admitted ($(head -1 "${WORK}/o"))"
    elif [ "${want}" = deny ] && [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability '${cls}'" "${WORK}/e" \
         && ! /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: denied ($(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g" | cut -c1-100))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}

# 1 absolute grant
run "1a spawn b1/tool under --allow-run=b1/tool" admit process.spawn "${P}" spawn "${B1}" "--allow-run=${B1}"
run "1b spawn b2/tool (same name, another binary)" deny process.spawn "${P}" spawn "${B2}" "--allow-run=${B1}"
# 2 bare-name grant, pinned at start-up
run "2a spawn tool under --allow-run=tool (PATH=b1)" admit process.spawn env PATH="${PATH1}" "${P}" spawn tool --allow-run=tool
run "2b spawn b2/tool under --allow-run=tool (PATH=b1)" deny process.spawn env PATH="${PATH1}" "${P}" spawn "${B2}" --allow-run=tool
run "2c env.set PATH then spawn b2/tool: still denied" deny process.spawn env PATH="${PATH1}" "${P}" setpath "${B2}" --allow-run=tool --allow-env=PATH
run "2d env.set PATH then spawn tool: the pinned b1/tool" admit process.spawn env PATH="${PATH1}" "${P}" setpath tool --allow-run=tool --allow-env=PATH
# 3 command strings: the first token
run "3a exec \"b1/tool arg\" under --allow-run=b1/tool" admit process.spawn "${P}" exec "${B1} arg" "--allow-run=${B1}"
run "3b exec \"b2/tool arg\" under --allow-run=b1/tool" deny process.spawn "${P}" exec "${B2} arg" "--allow-run=${B1}"
# 4 env keys
run "4a env.set TZ under TZ,TOKE_*" admit env.write "${P}" env TZ '--allow-env=TZ,TOKE_*'
run "4b env.set TOKE_HOME under TZ,TOKE_*" admit env.write "${P}" env TOKE_HOME '--allow-env=TZ,TOKE_*'
run "4c env.set TOKEN under TZ,TOKE_*" deny env.write "${P}" env TOKEN '--allow-env=TZ,TOKE_*'
run "4d env.set PATH under TZ,TOKE_*" deny env.write "${P}" env PATH '--allow-env=TZ,TOKE_*'
# 5 whole classes
run "5a --allow-run (whole class) spawns b2/tool" admit process.spawn "${P}" spawn "${B2}" --allow-run
run "5b --allow-env (whole class) sets PATH" admit env.write "${P}" env PATH --allow-env

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
