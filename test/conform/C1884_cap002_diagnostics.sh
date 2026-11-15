#!/usr/bin/env bash
# C1884_cap002_diagnostics.sh -- CAP002 "class granted, scope not" and its
# machine-readable record (story 138.19, D3; design note §3.5).
#
# THE CHANGE (owner answer 2026-11-15, D3).  A call whose class is granted but
# not for this scope stops with the new code CAP002; a call whose class is not
# granted at all keeps CAP001, with the same text and JSON as before.  Under
# TK_DIAG_JSON the CAP002 line carries "capability" and "scope" (the canonical
# path, connect:/listen: HOST:PORT, command or key), and a `fix` naming the
# exact scope that would admit the call, in the one place that would admit it:
#   - a run-time flag (--allow-read=<scope>) when run-time flags union with the
#     grant (no baked set, or runtime = "union"), or when only the run-time
#     flags exclude it;
#   - the manifest (fs_read = ["<scope>"]) when only the baked grant excludes it;
#   - no fix when both exclude it, or the request has no canonical scope (a
#     dangling symlink).  AGENTS §6: a fix only where it is right every time.
# A runtime record cannot come from `tkc --check`, so the D-series YAML runner
# cannot drive it; this script asserts each fix EXACTLY and then APPLIES it and
# shows the call admitted -- the fix is deterministically sufficient.
#
# WHAT IS ASSERTED
#   1. CAP002's JSON line parses, with diagnostic_id, severity, stage,
#      capability and scope (the canonical path)
#   2. case A (no baked set): fix "--allow-read=<path>"; applied -> admitted
#   3. case B (baked [dir]): fix "fs_read = [\"<path>\"]"; rebuilt -> admitted
#   4. case C (baked whole class, run-time flag narrows): fix
#      "--allow-read=<path>"; applied -> admitted
#   5. case D (baked [dir] AND a narrower run-time flag): no fix
#   6. a dangling symlink: scope is the path as given, no fix
#   7. net: fix "--allow-net=connect:127.0.0.1:48120"; applied -> admitted
#   8. CAP001 with no baked set: text and JSON byte-for-byte as before 138.19
#   9. CAP001 on a narrowing binary: fix "tkc.toml [capabilities] env_write =
#      true"; rebuilt -> admitted; absent when a run-time flag scopes the class
#  10. (round 2) a path holding a newline and a forged CAP001 record cannot
#      inject a diagnostic line: the text line escapes it (\n), stderr holds
#      exactly one JSON line, CAP002, whose scope is the path byte for byte
#  11. (round 2) a scope a run-time flag cannot carry (it contains ',') has no
#      fix, and the hint says why -- not "both the baked grant and the
#      run-time flags exclude it" on a binary with nothing baked
#  12. (round 2) CAP003: a malformed run-time scope and a refused --allow-all
#      print one text line and, under TK_DIAG_JSON, one JSON record with
#      diagnostic_id CAP003, severity, stage, flag and reason, and no fix
#  13. (round 2) a scope a tkc.toml string cannot carry (a '"') has no
#      manifest fix, and the hint says why
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- no CAP002 exists there (1-7, 9-13
# FAIL); 8 passes on both, which is the point of it.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1884_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C1884: CAP002 and its exact fix (138.19 D3)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/probe.tk" <<'EOF'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=env:std.env;
i=ws:std.ws;
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
  if(op=="connect"){
    let r=mt ws.connect(p) {$ok:c 1;$err:e 2};
    io.println("connect-done \(r)");
  }el{};
  <0
};
EOF
D="${WORK}/data"; mkdir -p "${D}/sub"
echo a >"${D}/in.txt"; echo c >"${WORK}/out.txt"
ln -s "${WORK}/nowhere" "${D}/dangling"
OUT="${WORK}/out.txt"

build() {   # NAME [tkc args]
    local name="$1"; shift
    ( cd "${WORK}" && "${TKC}" "$@" --out "${WORK}/${name}" probe.tk ) >"${WORK}/${name}.cl" 2>&1 \
        || { fail "build ${name}: $(head -1 "${WORK}/${name}.cl")"; return 1; }
}
# jfield FIELD -- the field of the LAST JSON line on stderr ("<absent>" if none)
jfield() {
    python3 -I - "$1" "${WORK}/e" <<'PY'
import json, sys
field, path = sys.argv[1], sys.argv[2]
rec = None
for line in open(path, encoding="utf-8", errors="replace"):
    line = line.strip()
    if line.startswith("{"):
        try: rec = json.loads(line)
        except ValueError: print("<badjson>"); sys.exit(0)
if rec is None: print("<nojson>")
else: print(rec.get(field, "<absent>"))
PY
}
runj() { local rc=0; ( cd "${WORK}" && TK_DIAG_JSON=1 "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?; return "${rc}"; }
admitted() { local rc=0; ( cd "${WORK}" && "$@" ) >"${WORK}/o2" 2>"${WORK}/e2" || rc=$?
             [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o2"; }
# checkfix LABEL WANTFIX -- WANTFIX is the exact fix, or <absent>
checkfix() {
    local got; got="$(jfield fix)"
    if [ "$(jfield diagnostic_id)" = CAP002 ] && [ "${got}" = "$2" ]; then
        pass "$1: fix = ${got//${WORK}/<work>}"
    else
        fail "$1: diagnostic $(jfield diagnostic_id), fix '${got}' (wanted '$2')"; sed 's/^/      | /' "${WORK}/e" | head -4
    fi
}

build plain || exit 1
# 1 + 2: case A
runj "${WORK}/plain" read "${OUT}" "--allow-read=${D}"
if [ "$(jfield diagnostic_id)" = CAP002 ] && [ "$(jfield severity)" = error ] && [ "$(jfield stage)" = runtime ] \
   && [ "$(jfield capability)" = fs.read ] && [ "$(jfield scope)" = "${OUT}" ]; then
    pass "1 CAP002 JSON: diagnostic_id, severity, stage, capability, scope = the canonical path"
else
    fail "1 CAP002 JSON fields"; sed 's/^/      | /' "${WORK}/e" | head -4
fi
checkfix "2 case A (no baked set)" "--allow-read=${OUT}"
if admitted "${WORK}/plain" read "${OUT}" "--allow-read=${D}" "--allow-read=${OUT}"; then
    pass "2 the fix applied (one more run-time flag): admitted"
else fail "2 the fix applied: still denied"; fi

# 3: case B
printf '[capabilities]\nfs_read = ["%s"]\n' "${D}" >"${WORK}/b.toml"
if build caseb "--config=${WORK}/b.toml"; then
    runj "${WORK}/caseb" read "${OUT}"
    checkfix "3 case B (baked [dir])" "fs_read = [\"${OUT}\"]"
    printf '[capabilities]\nfs_read = ["%s"]\n' "${OUT}" >"${WORK}/b2.toml"
    if build caseb2 "--config=${WORK}/b2.toml" && admitted "${WORK}/caseb2" read "${OUT}"; then
        pass "3 the fix applied (the manifest line, rebuilt): admitted"
    else fail "3 the fix applied: still denied"; fi
fi

# 4: case C
printf '[capabilities]\nfs_read = true\n' >"${WORK}/c.toml"
if build casec "--config=${WORK}/c.toml"; then
    runj "${WORK}/casec" read "${OUT}" "--allow-read=${D}"
    checkfix "4 case C (baked whole, run-time narrows)" "--allow-read=${OUT}"
    if admitted "${WORK}/casec" read "${OUT}" "--allow-read=${D}" "--allow-read=${OUT}"; then
        pass "4 the fix applied: admitted"
    else fail "4 the fix applied: still denied"; fi
fi

# 5: case D
if [ -x "${WORK}/caseb" ]; then
    runj "${WORK}/caseb" read "${OUT}" "--allow-read=${D}/sub"
    checkfix "5 case D (both exclude it)" "<absent>"
fi

# 6: dangling symlink
runj "${WORK}/plain" read "${D}/dangling" "--allow-read=${D}"
if [ "$(jfield scope)" = "${D}/dangling" ]; then pass "6 scope is the path as given"; else fail "6 scope '$(jfield scope)'"; fi
checkfix "6 dangling symlink" "<absent>"

# 7: net
runj "${WORK}/plain" connect ws://127.0.0.1:48120/ --allow-net=connect:127.0.0.1:48121
checkfix "7 net connect" "--allow-net=connect:127.0.0.1:48120"
if admitted "${WORK}/plain" connect ws://127.0.0.1:48120/ --allow-net=connect:127.0.0.1:48121 --allow-net=connect:127.0.0.1:48120; then
    pass "7 the fix applied: admitted (the connect itself is refused: closed port)"
else fail "7 the fix applied: still denied"; fi

# 8: CAP001 unchanged
rc=0; ( cd "${WORK}" && TK_DIAG_JSON=1 "${WORK}/plain" read "${OUT}" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
want8="CAP001: capability 'fs.read' required but not granted
  grant it at run time with --allow-read, or declare it in tkc.toml [capabilities]
{\"diagnostic_id\":\"CAP001\",\"severity\":\"error\",\"stage\":\"runtime\",\"capability\":\"fs.read\",\"fix\":\"grant --allow-read or tkc.toml [capabilities] fs.read=true\"}"
if [ "${rc}" -eq 1 ] && [ "$(cat "${WORK}/e")" = "${want8}" ]; then
    pass "8 CAP001 (no baked set): text and JSON byte-for-byte as before"
else
    fail "8 CAP001 changed (rc=${rc})"; sed 's/^/      | /' "${WORK}/e" | head -4
fi

# 9: CAP001 on a narrowing binary
if build narrow "--config=${WORK}/c.toml"; then
    runj "${WORK}/narrow" env TOKE_C1884 --allow-env
    if [ "$(jfield diagnostic_id)" = CAP001 ] && [ "$(jfield fix)" = "tkc.toml [capabilities] env_write = true" ]; then
        pass "9a CAP001 (narrowing binary): fix = tkc.toml [capabilities] env_write = true"
    else fail "9a CAP001 narrowing fix '$(jfield fix)'"; sed 's/^/      | /' "${WORK}/e" | head -4; fi
    printf '[capabilities]\nfs_read = true\nenv_write = true\n' >"${WORK}/c2.toml"
    if build narrow2 "--config=${WORK}/c2.toml" && admitted "${WORK}/narrow2" env TOKE_C1884 --allow-env; then
        pass "9a the fix applied (rebuilt): admitted"
    else fail "9a the fix applied: still denied"; fi
    runj "${WORK}/narrow" env TOKE_C1884 --allow-env=TZ
    if [ "$(jfield diagnostic_id)" = CAP001 ] && [ "$(jfield fix)" = "<absent>" ]; then
        pass "9b CAP001 with a scoping run-time flag for the class: no fix"
    else fail "9b fix '$(jfield fix)'"; fi
fi

# 10: a newline in the program's path cannot forge a diagnostic line
FORGED='{"diagnostic_id":"CAP001","severity":"error","stage":"runtime","capability":"fs.read","fix":"--allow-all"}'
NLPATH="${WORK}/x"$'\n'"${FORGED}"
runj "${WORK}/plain" read "${NLPATH}" "--allow-read=${D}"
nj="$(/usr/bin/grep -a -c '^{' "${WORK}/e")"
if [ "${nj}" = 1 ] && [ "$(jfield diagnostic_id)" = CAP002 ] && [ "$(jfield scope)" = "${NLPATH}" ] \
   && /usr/bin/grep -a -q -F "not for '${WORK}/x\n{\"diagnostic_id\"" "${WORK}/e"; then
    pass "10 newline in a path: escaped on the text line; one JSON line (CAP002, scope byte for byte)"
else
    fail "10 newline injection: ${nj} JSON line(s)"; sed 's/^/      | /' "${WORK}/e" | head -5
fi

# 11: a comma in the scope, no baked set
COMMA="${WORK}/a,b.txt"; echo x >"${COMMA}"
runj "${WORK}/plain" read "${COMMA}" "--allow-read=${D}/sub"
checkfix "11 a scope with ',' (no baked set)" "<absent>"
if /usr/bin/grep -a -q "the flag splits its value on ','" "${WORK}/e" && ! /usr/bin/grep -a -q "baked" "${WORK}/e"; then
    pass "11 the hint says the flag cannot carry it (and names nothing baked)"
else fail "11 hint: $(sed -n 2p "${WORK}/e")"; fi

# 12: CAP003
cap003() {   # LABEL BIN ARGS...
    local label="$1"; shift
    local rc=0
    runj "$@" || rc=$?
    if [ "${rc}" -eq 1 ] && [ ! -s "${WORK}/o" ] && [ "$(/usr/bin/grep -a -c . "${WORK}/e")" = 2 ] \
       && /usr/bin/grep -a -q "^CAP003: " "${WORK}/e" && [ "$(jfield diagnostic_id)" = CAP003 ] \
       && [ "$(jfield severity)" = error ] && [ "$(jfield stage)" = runtime ] \
       && [ "$(jfield fix)" = "<absent>" ] && [ "$(jfield reason)" != "<absent>" ]; then
        pass "12 ${label}: CAP003 text + JSON (flag $(jfield flag)), no fix"
    else
        fail "12 ${label}: rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}
cap003 "malformed --allow-net=api.example.com" "${WORK}/plain" read "${OUT}" --allow-net=api.example.com
if [ "$(jfield flag)" = "--allow-net=api.example.com" ]; then pass "12 flag is the argument as given"; else fail "12 flag '$(jfield flag)'"; fi
[ -x "${WORK}/caseb" ] && cap003 "--allow-all on a narrowing binary" "${WORK}/caseb" read "${OUT}" --allow-all

# 13: a '"' in the scope, only the baked grant excludes it
QUOTE="${WORK}/q\"x.txt"; echo x >"${QUOTE}"
if [ -x "${WORK}/caseb" ]; then
    runj "${WORK}/caseb" read "${QUOTE}"
    checkfix "13 a scope with '\"' (baked [dir])" "<absent>"
    if /usr/bin/grep -a -q "cannot be written in tkc.toml" "${WORK}/e"; then
        pass "13 the hint says a tkc.toml string cannot carry it"
    else fail "13 hint: $(sed -n 2p "${WORK}/e")"; fi
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
