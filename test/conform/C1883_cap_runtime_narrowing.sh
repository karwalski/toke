#!/usr/bin/env bash
# C1883_cap_runtime_narrowing.sh -- run-time --allow-* flags narrow a baked
# grant set instead of widening it (story 138.19, D2; design note §4).
#
# THE DEFECT (loke T-59 point 4).  tk_cap_init unioned every --allow-* token
# in argv with the baked set, and --allow-all switched to ALLOW_ALL: whoever
# launched a binary could add any class.  A binary built with loke's manifest
# (no env_write) ran env.set when started with --allow-env.
#
# THE CHANGE (owner answer 2026-11-15, D2).  A binary that bakes a grant set
# (any class key or `all` in tkc.toml [capabilities], or a compile-time
# --allow-* flag) takes run-time flags as a NARROWING: a class a run-time flag
# names is admitted only where both the baked grant and the flag admit it; a
# class no flag names keeps its baked grant.  --allow-all at run time on such
# a binary is refused with one CAP003 line (exit 1), unless the baked set is already
# everything.  `runtime = "union"` in the manifest restores the old union.  A
# binary with no baked grant set -- including one built with only `enforce` or
# --cap-enforce, which are vestigial -- takes its run-time flags as the grant,
# as before.
#
# WHAT IS ASSERTED
#   1. baked fs_read = [dir]; --allow-read=dir/sub narrows: dir/sub/x read,
#      dir/in.txt denied
#   2. baked fs_read = [dir]; --allow-read=<outside> cannot widen: the outside
#      file is denied
#   3. loke's T-59 manifest (no env_write) + --allow-env: env.set stops with
#      CAP001, and the hint names the manifest, not the flag
#   4. --allow-all on that binary is refused before main runs (exit 1, one
#      CAP003 line, no program output; its JSON record is C1884 case 12)
#   5. all = true baked: --allow-all is accepted (it widens nothing); a scoped
#      flag still narrows
#   6. runtime = "union": --allow-env adds env.write, --allow-read=<outside>
#      widens
#   7. no baked grant set: a scoped run-time flag is the grant
#   8. --cap-enforce alone bakes no grant set: --allow-read grants the class
#   9. a grant baked by a compile-time flag (tkc --allow-read=dir) narrows too
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- 3 and 4 FAIL (env.set runs; the
# program runs under --allow-all); the array-manifest cases fail to build.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1883_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C1883: run-time flags narrow a baked grant set (138.19 D2)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/probe.tk" <<'EOF'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=env:std.env;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let p=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="read"){
    let r=mt file.read(p) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{};
  if(op=="env"){
    let v=env.set(p;"1");
    io.println("env-done");
  }el{};
  <0
};
EOF
D="${WORK}/data"; mkdir -p "${D}/sub"
echo a >"${D}/in.txt"; echo b >"${D}/sub/x"; echo c >"${WORK}/outside.txt"

# build NAME [tkc args...] -- the probe built with these compile-time grants
build() {
    local name="$1"; shift
    if ( cd "${WORK}" && "${TKC}" "$@" --out "${WORK}/${name}" probe.tk ) >"${WORK}/${name}.cl" 2>&1; then
        return 0
    fi
    fail "build ${name}: $(head -1 "${WORK}/${name}.cl")"; return 1
}
# expect LABEL admit|deny|cap001|refused BIN ARGS...
expect() {
    local label="$1" want="$2"; shift 2
    local rc=0
    ( cd "${WORK}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    local ok=0
    case "${want}" in
        admit)   [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o" && ok=1 ;;
        deny)    [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002:" "${WORK}/e" && ok=1 ;;
        cap001)  [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP001:" "${WORK}/e" && ok=1 ;;
        refused) [ "${rc}" -eq 1 ] && [ ! -s "${WORK}/o" ] && [ "$(wc -l <"${WORK}/e" | tr -d ' ')" = 1 ] \
                 && /usr/bin/grep -a -q "^CAP003: --allow-all refused" "${WORK}/e" && ok=1 ;;
    esac
    if [ "${ok}" -eq 1 ]; then
        pass "${label}: ${want} ($(head -1 "${WORK}/o" "${WORK}/e" 2>/dev/null | /usr/bin/grep -a -v '^==>' | /usr/bin/grep -a . | head -1 | sed "s#${WORK}#<work>#g" | cut -c1-90))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}

printf '[capabilities]\nfs_read = ["%s"]\n' "${D}" >"${WORK}/scoped.toml"
if build scoped "--config=${WORK}/scoped.toml"; then
    expect "1a baked [dir] + --allow-read=dir/sub reads dir/sub/x" admit "${WORK}/scoped" read "${D}/sub/x" "--allow-read=${D}/sub"
    expect "1b ... and dir/in.txt is narrowed away" deny "${WORK}/scoped" read "${D}/in.txt" "--allow-read=${D}/sub"
    expect "1c no run-time flag: the baked scope" admit "${WORK}/scoped" read "${D}/in.txt"
    expect "2a --allow-read=<outside> cannot widen" deny "${WORK}/scoped" read "${WORK}/outside.txt" "--allow-read=${WORK}/outside.txt"
    expect "2b --allow-read (whole class) cannot widen" deny "${WORK}/scoped" read "${WORK}/outside.txt" --allow-read
fi

# 3, 4: loke's T-59 manifest
printf '[capabilities]\nfs_read = true\nfs_write = true\nnet = true\nprocess_spawn = true\nenv_write = false\nenforce = true\n' >"${WORK}/loke.toml"
if build loke "--config=${WORK}/loke.toml"; then
    expect "3a loke manifest + --allow-env: env.set" cap001 "${WORK}/loke" env LOKE_CAP_PROBE --allow-env
    if /usr/bin/grep -a -q "run-time flags can narrow them, not add to them" "${WORK}/e" \
       && ! /usr/bin/grep -a -q "grant it at run time with --allow-env" "${WORK}/e"; then
        pass "3b the CAP001 hint names the manifest, not the run-time flag"
    else
        fail "3b CAP001 hint"; sed 's/^/      | /' "${WORK}/e" | head -3
    fi
    expect "4 loke manifest + --allow-all" refused "${WORK}/loke" read "${WORK}/outside.txt" --allow-all
    expect "4b loke manifest, no flags: reads (baked whole fs_read)" admit "${WORK}/loke" read "${WORK}/outside.txt"
fi

# 5: all = true baked
printf '[capabilities]\nall = true\n' >"${WORK}/all.toml"
if build all "--config=${WORK}/all.toml"; then
    expect "5a all = true + --allow-all (widens nothing)" admit "${WORK}/all" read "${WORK}/outside.txt" --allow-all
    expect "5b all = true + --allow-read=<dir> narrows fs.read" deny "${WORK}/all" read "${WORK}/outside.txt" "--allow-read=${D}"
    expect "5c ... and leaves env.write whole" admit "${WORK}/all" env TOKE_C1883 "--allow-read=${D}"
fi

# 6: runtime = "union"
printf '[capabilities]\nfs_read = ["%s"]\nruntime = "union"\n' "${D}" >"${WORK}/union.toml"
if build union "--config=${WORK}/union.toml"; then
    expect "6a runtime = union + --allow-env adds env.write" admit "${WORK}/union" env TOKE_C1883 --allow-env
    expect "6b runtime = union + --allow-read=<outside> widens" admit "${WORK}/union" read "${WORK}/outside.txt" "--allow-read=${WORK}/outside.txt"
    expect "6c runtime = union, no flag: the baked scope still binds" deny "${WORK}/union" read "${WORK}/outside.txt"
fi

# 7: no baked grant set
if build plain; then
    expect "7a no baked set + --allow-read=<dir>: inside" admit "${WORK}/plain" read "${D}/in.txt" "--allow-read=${D}"
    expect "7b no baked set + --allow-read=<dir>: outside" deny "${WORK}/plain" read "${WORK}/outside.txt" "--allow-read=${D}"
fi
# 8: --cap-enforce alone
if build enf --cap-enforce; then
    expect "8 --cap-enforce + --allow-read (whole class)" admit "${WORK}/enf" read "${WORK}/outside.txt" --allow-read
fi
# 9: a compile-time scoped flag bakes a narrowing set
if build cli "--allow-read=${D}"; then
    expect "9a tkc --allow-read=<dir>: the baked scope" admit "${WORK}/cli" read "${D}/in.txt"
    expect "9b ... + --allow-env cannot add env.write" cap001 "${WORK}/cli" env TOKE_C1883 --allow-env
fi
# show-limits
if ( cd "${WORK}" && "${TKC}" --config="${WORK}/union.toml" --show-limits probe.tk ) 2>/dev/null | /usr/bin/grep -a -q "cap-runtime  = union" \
   && ( cd "${WORK}" && "${TKC}" --config="${WORK}/scoped.toml" --show-limits probe.tk ) 2>/dev/null | /usr/bin/grep -a -q "cap-runtime  = narrow"; then
    pass "10 --show-limits reports cap-runtime narrow / union"
else
    fail "10 --show-limits cap-runtime"
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
