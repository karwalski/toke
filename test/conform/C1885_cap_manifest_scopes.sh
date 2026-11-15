#!/usr/bin/env bash
# C1885_cap_manifest_scopes.sh -- tkc.toml [capabilities] scope lists, the
# grant record, and the D4 warning release (story 138.19, D1 and D4; design
# note §2, §5).
#
# THE CHANGE.
#   D1: a class key takes a one-line array of quoted scopes,
#       fs_read = ["./content", "/etc/ssl/certs"]; a malformed array, an
#       unknown net prefix or an empty string is a parse error naming the line.
#       --show-limits and the --companion record list the scopes.
#   D4: a QUOTED STRING keeps its old meaning in this release -- the whole
#       class -- and the compiler warns, W9012, naming the release (3.4.0) in
#       which it becomes a one-element scope list.  No `fix`: the manifest is
#       not the source file a fix would edit.
#   And `all = true`, which the spec has always listed, parses: the unknown-key
#   sentinel was the same all-ones value as TK_CAP_ALL, so `all` was refused
#   as "unknown capability 'all'" (v3.2.0, measured).
#
# WHAT IS ASSERTED
#   1. arrays parse, --show-limits prints one cap-scope line per scope, and a
#      trailing # comment is allowed after a value
#   2. each malformed manifest is refused with its line number: unterminated
#      array, unknown net prefix (udp:), empty string, trailing comma, nested
#      array, listen without a port, a CIDR listen, `all` with a list,
#      runtime = "bogus"
#   3. all = true parses (cap-grants = 0xffffffff)
#   4. the --companion record lists each class with its scopes
#   5. loke's T-59 point 1 in the array form: fs_read = ["/nonexistent-dir-only"]
#      denies reading /etc/hosts (CAP002); net = ["connect:10.255.255.1:9"]
#      denies net.portavailable (a listen)
#   6. W9012 (D4): fs_read = "<dir>" warns in --diag-json (code W9012,
#      severity warning, no fix, the message names 3.4.0) and in text, and the
#      binary still reads OUTSIDE <dir> -- the meaning has not changed yet
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- 1, 3, 4, 5 FAIL (arrays and `all`
# are refused), 6 FAILS (no warning).
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1885_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C1885: tkc.toml scope lists, the grant record, W9012 (138.19 D1, D4)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/probe.tk" <<'EOF'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=net:std.net;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  if(op=="fs_read"){
    let r=mt file.read("/etc/hosts") {$ok:s 1;$err:e 2};
    io.println("fs_read-done \(r)");
  }el{};
  if(op=="net"){
    io.println("net-done \(net.portavailable(1))");
  }el{};
  <0
};
EOF
D="${WORK}/content"; mkdir -p "${D}"

# 1 arrays, show-limits, a trailing comment
cat >"${WORK}/ok.toml" <<EOF
[capabilities]
fs_read       = ["./content", '/etc/ssl/certs']   # read-only inputs
fs_write      = []
net           = ["connect:api.example.com:443", "listen:127.0.0.1:48130"]
process_spawn = ["/usr/bin/git", "git"]
env_write     = ["TZ", "TOKE_*"]
EOF
out="$(cd "${WORK}" && "${TKC}" --config="${WORK}/ok.toml" --show-limits probe.tk 2>&1)"
n="$(printf '%s\n' "${out}" | /usr/bin/grep -a -c '^  cap-scope    = ')"
if [ "${n}" = 8 ] && printf '%s\n' "${out}" | /usr/bin/grep -a -q '^  cap-scope    = fs.read /etc/ssl/certs$' \
   && printf '%s\n' "${out}" | /usr/bin/grep -a -q '^  cap-scope    = net listen:127.0.0.1:48130$' \
   && printf '%s\n' "${out}" | /usr/bin/grep -a -q '^  cap-runtime  = narrow$'; then
    pass "1 a scope-list manifest parses; --show-limits lists its 8 scopes"
else
    fail "1 scope lists (${n} cap-scope lines)"; printf '%s\n' "${out}" | sed 's/^/      | /' | tail -12
fi

# 2 malformed manifests, each refused at its line
bad() {   # LABEL VALUE-LINE REGEX
    printf '[capabilities]\n# line 2\n%s\n' "$2" >"${WORK}/bad.toml"
    local rc=0 msg
    msg="$(cd "${WORK}" && "${TKC}" --config="${WORK}/bad.toml" --check probe.tk 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s\n' "${msg}" | /usr/bin/grep -a -q "bad.toml:3: .*$3"; then
        pass "2 $1: refused at line 3 ($(printf '%s\n' "${msg}" | head -1 | sed 's#.*bad.toml:3: ##' | cut -c1-70))"
    else
        fail "2 $1: rc=${rc}"; printf '%s\n' "${msg}" | sed 's/^/      | /' | head -3
    fi
}
bad "unterminated array"     'fs_read = ["./a"'                 'one line'
bad "unknown net prefix"     'net = ["udp:1.2.3.4:5"]'          'connect:HOST'
bad "empty string"           'fs_read = [""]'                   'empty scope'
bad "trailing comma"         'fs_read = ["./a",]'               'trailing comma'
bad "nested array"           'fs_read = [["./a"]]'              'quoted strings only'
bad "listen without a port"  'net = ["listen:127.0.0.1"]'       'explicit address and port'
bad "a CIDR listen"          'net = ["listen:127.0.0.0/8:80"]'  'one IP literal'
bad "all with a list"        'all = ["x"]'                      'true or false'
bad "runtime = bogus"        'runtime = "bogus"'                'narrow'

# 3 all = true
out="$(cd "${WORK}" && printf '[capabilities]\nall = true\n' >all.toml && "${TKC}" --config="${WORK}/all.toml" --show-limits probe.tk 2>&1)"
if printf '%s\n' "${out}" | /usr/bin/grep -a -q '^  cap-grants   = 0xffffffff$'; then
    pass "3 all = true parses (cap-grants = 0xffffffff)"
else
    fail "3 all = true"; printf '%s\n' "${out}" | sed 's/^/      | /' | head -3
fi

# 4 the companion record
if ( cd "${WORK}" && "${TKC}" --config="${WORK}/ok.toml" --companion probe.tk ) >"${WORK}/comp.md" 2>"${WORK}/comp.err" \
   && /usr/bin/grep -a -q '^- `fs.read`: `./content`, `/etc/ssl/certs`$' "${WORK}/comp.md" \
   && /usr/bin/grep -a -q '^- `env.write`: `TZ`, `TOKE_\*`$' "${WORK}/comp.md" \
   && /usr/bin/grep -a -q 'Mode: \*\*enforce (deny-by-default)\*\*. Run-time --allow-\* flags: \*\*narrow only\*\*' "${WORK}/comp.md"; then
    pass "4 --companion lists each class with its scopes, and the run-time mode"
else
    fail "4 companion record"; /usr/bin/grep -a -A8 '## Capabilities' "${WORK}/comp.md" | sed 's/^/      | /' | head -10
fi

# 5 T-59 point 1, array form
printf '[capabilities]\nfs_read = ["/nonexistent-dir-only"]\n' >"${WORK}/sfs.toml"
printf '[capabilities]\nnet = ["connect:10.255.255.1:9"]\n' >"${WORK}/snet.toml"
for m in sfs snet; do
    ( cd "${WORK}" && "${TKC}" --config="${WORK}/${m}.toml" --out "${WORK}/${m}" probe.tk ) >"${WORK}/${m}.cl" 2>&1 || fail "5 build ${m}"
done
rc=0; ( cd "${WORK}" && ./sfs fs_read ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability 'fs.read'" "${WORK}/e" && [ ! -s "${WORK}/o" ]; then
    pass "5a fs_read = [\"/nonexistent-dir-only\"]: /etc/hosts denied ($(head -1 "${WORK}/e" | cut -c1-80))"
else fail "5a rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -3; fi
rc=0; ( cd "${WORK}" && ./snet net ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability 'net'" "${WORK}/e" && [ ! -s "${WORK}/o" ]; then
    pass "5b net = [\"connect:10.255.255.1:9\"]: a listen denied ($(head -1 "${WORK}/e" | cut -c1-80))"
else fail "5b rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -3; fi

# 6 W9012, the D4 warning release
printf '[capabilities]\nfs_read = "%s"\n' "${D}" >"${WORK}/q.toml"
( cd "${WORK}" && "${TKC}" --diag-json --config="${WORK}/q.toml" --out "${WORK}/q" probe.tk ) >"${WORK}/q.out" 2>"${WORK}/q.err"
if python3 -I - "${WORK}/q.err" <<'PY'
import json, sys
recs = [json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
w = [r for r in recs if r.get("error_code") == "W9012"]
ok = (len(w) == 1 and w[0]["severity"] == "warning" and "fix" not in w[0]
      and "3.4.0" in w[0]["message"] and w[0]["pos"]["line"] == 2
      and w[0]["file"].endswith("q.toml"))
sys.exit(0 if ok else 1)
PY
then pass "6a quoted string: one W9012 (warning, line 2 of q.toml, names 3.4.0, no fix)"
else fail "6a W9012 JSON"; sed 's/^/      | /' "${WORK}/q.err" | head -3; fi
rc=0; ( cd "${WORK}" && ./q fs_read ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
if [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q "fs_read-done 1" "${WORK}/o"; then
    pass "6b the quoted string still grants the whole class (reads /etc/hosts)"
else fail "6b rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -3; fi
if ( cd "${WORK}" && "${TKC}" --diag-text --config="${WORK}/q.toml" --check probe.tk ) 2>&1 | /usr/bin/grep -a -q '^warning\[W9012\]'; then
    pass "6c --diag-text prints warning[W9012]"
else fail "6c text warning"; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
