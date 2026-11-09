#!/usr/bin/env bash
# C1160_pkg_cli_manifest_commands.sh — `tkc pkg init/add/remove/resolve/list`
# (story 76.1.3b, design ADR-0004 "CLI Commands").
#
# THE GAP. The 76.1.3a resolver, manifest parser and lock writer existed in
# src/pkg.c but were linked into no command: `tkc pkg init` was read as a
# source file named "pkg" ("tkc: cannot open pkg"), and nothing wrote a
# pkg.toml or a pkg.lock.  This drives every manifest command through the
# real binary and checks that every error path is one structured diagnostic
# with its code (AGENTS §6) and no `fix`.
#
# No network: no command here fetches (resolve does not), and no source is
# contacted.
#
# Negative control: TKC=<main's tkc> -- every case fails (no `pkg` command).
#
# Story: 76.1.3b

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }
# one JSON diagnostic with this code, and no fix
diag_is() {   # name file code
    local n; n=$(grep -c '"error_code":"'"$3"'"' "$2" 2>/dev/null)
    if [ "${n:-0}" -ge 1 ] && ! grep -q '"fix":"[^"]' "$2"; then ok "$1"
    else bad "$1" "want $3 in: $(head -c 400 "$2")"; fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1160_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
export HOME="${WORK}/home"; mkdir -p "${HOME}"
P="${WORK}/myapp"; mkdir -p "${P}"; cd "${P}" || exit 1

# 1. init: default name is the directory's; the ADR's template
"${TKC}" pkg init >out 2>err; expect "1a-init-rc" "$?" "0"
expect "1b-init-template" "$(cat pkg.toml)" '[package]
name = "myapp"
version = "0.1.0"

[dependencies]'
# 2. init again: a warning, the file untouched
echo '# keep me' >> pkg.toml; cp pkg.toml before
"${TKC}" pkg init >out 2>err; expect "2a-reinit-rc" "$?" "0"
diag_is "2b-reinit-W9011" err W9011
cmp -s pkg.toml before && ok "2c-reinit-unchanged" || bad "2c-reinit-unchanged"
# 3. a directory name that is no package name, and an explicit bad name
mkdir -p "${WORK}/My-App"; ( cd "${WORK}/My-App" && "${TKC}" pkg init >out 2>err; echo "rc=$?" >rc )
expect "3a-badname-rc" "$(cat "${WORK}/My-App/rc")" "rc=3"
diag_is "3b-badname-E9031" "${WORK}/My-App/err" E9031
[ ! -e "${WORK}/My-App/pkg.toml" ] && ok "3c-badname-nothing-written" || bad "3c-badname-nothing-written"
( cd "${WORK}/My-App" && "${TKC}" pkg init std >out 2>err; echo "rc=$?" >rc )
expect "3d-reserved-name-rc" "$(cat "${WORK}/My-App/rc")" "rc=3"

# 4. add, simple form: inserted in [dependencies], comments kept, lock written
"${TKC}" pkg add jsonparser "1.2.0" >out 2>err; expect "4a-add-rc" "$?" "0"
grep -qx 'jsonparser = "1.2.0"' pkg.toml && ok "4b-add-line" || bad "4b-add-line" "$(cat pkg.toml)"
grep -qx '# keep me' pkg.toml && ok "4c-comment-kept" || bad "4c-comment-kept"
grep -qx 'jsonparser = "1.2.0"' pkg.lock && grep -qx 'lock-version = "2"' pkg.lock \
    && ok "4d-lock-written" || bad "4d-lock-written" "$(cat pkg.lock 2>&1)"
grep -q 'resolved 1 package' out && ok "4e-resolve-output" || bad "4e-resolve-output" "$(cat out)"
# 5. add again updates in place (one line, new constraint)
"${TKC}" pkg add jsonparser ">=1.3.0 <2.0.0" >out 2>err; expect "5a-update-rc" "$?" "0"
expect "5b-update-one-line" "$(grep -c '^jsonparser' pkg.toml)" "1"
grep -qx 'jsonparser = ">=1.3.0 <2.0.0"' pkg.toml && ok "5c-update-value" || bad "5c-update-value"
grep -qx 'jsonparser = "1.3.0"' pkg.lock && ok "5d-mvs-min" || bad "5d-mvs-min" "$(cat pkg.lock)"
# 6. add with --git: the extended form
"${TKC}" pkg add web ">=0.3.0" --git https://example.com/toke/web.git >out 2>err
expect "6a-add-git-rc" "$?" "0"
grep -qx '\[dependencies.web\]' pkg.toml && grep -qx 'git = "https://example.com/toke/web.git"' pkg.toml \
    && ok "6b-extended-form" || bad "6b-extended-form" "$(cat pkg.toml)"
grep -qx 'web = "https://example.com/toke/web.git"' pkg.lock && ok "6c-lock-source" || bad "6c-lock-source" "$(cat pkg.lock)"
# 7. invalid arguments: E9031, rc 3, nothing changed
cp pkg.toml before; cp pkg.lock beforelock
"${TKC}" pkg add web '^1.0.0' >out 2>err; expect "7a-caret-rc" "$?" "3"; diag_is "7b-caret-E9031" err E9031
"${TKC}" pkg add Web 1.0.0 >out 2>err;   expect "7c-upper-rc" "$?" "3"; diag_is "7d-upper-E9031" err E9031
"${TKC}" pkg add web '1.0.0" ; x' >out 2>err; expect "7e-inject-rc" "$?" "3"
"${TKC}" pkg add evil 1.0.0 --git 'ext::sh -c touch% /tmp/pwn' >out 2>err; expect "7f-ext-rc" "$?" "3"
diag_is "7g-ext-E9031" err E9031
"${TKC}" pkg add evil 1.0.0 --git http://example.com/e.git >out 2>err; expect "7h-http-rc" "$?" "3"
"${TKC}" pkg add evil 1.0.0 --git "file://${WORK}/x.git" >out 2>err; expect "7i-file-no-override-rc" "$?" "3"
grep -q 'allow-file-source' err && ok "7j-file-hint" || bad "7j-file-hint" "$(cat err)"
"${TKC}" pkg add myapp 1.0.0 >out 2>err; expect "7k-self-rc" "$?" "3"
"${TKC}" pkg frobnicate >out 2>err;        expect "7l-unknown-cmd-rc" "$?" "3"; diag_is "7m-unknown-cmd-E9031" err E9031
"${TKC}" pkg add web >out 2>err;           expect "7n-missing-arg-rc" "$?" "3"
"${TKC}" pkg list --bogus >out 2>err;      expect "7o-unknown-flag-rc" "$?" "3"
"${TKC}" pkg >out 2>err;                   expect "7p-no-cmd-rc" "$?" "3"
cmp -s pkg.toml before && cmp -s pkg.lock beforelock && ok "7q-unchanged" || bad "7q-unchanged"

# 8. list: direct deps from the lock
"${TKC}" pkg list >out 2>err; expect "8a-list-rc" "$?" "0"
expect "8b-list" "$(cat out)" 'direct:
  jsonparser     1.3.0
  web            0.3.0'
# 9. remove: both forms; the lock follows; unknown name is E9031
"${TKC}" pkg remove web >out 2>err; expect "9a-remove-ext-rc" "$?" "0"
grep -q 'web' pkg.toml && bad "9b-ext-table-gone" "$(cat pkg.toml)" || ok "9b-ext-table-gone"
grep -q '^web' pkg.lock && bad "9c-lock-follows" || ok "9c-lock-follows"
"${TKC}" pkg remove web >out 2>err; expect "9d-remove-absent-rc" "$?" "3"; diag_is "9e-remove-absent-E9031" err E9031
"${TKC}" pkg remove jsonparser >out 2>err; expect "9f-remove-simple-rc" "$?" "0"
grep -q 'jsonparser' pkg.toml && bad "9g-simple-gone" || ok "9g-simple-gone"
grep -qx '# keep me' pkg.toml && ok "9h-comment-survives" || bad "9h-comment-survives"

# 10. resolution failures, before anything is written
cat > pkg.toml <<'EOF'
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
a = "1.0.0"
a2 = ">=1.0.0"
EOF
"${TKC}" pkg resolve >out 2>err; expect "10a-resolve-rc" "$?" "0"
# exact 1.0.0 vs >=1.2.0 for one package: E9025, naming it
cat > pkg.toml <<'EOF'
[package]
name = "myapp"
version = "0.1.0"

[dependencies]
a = "1.0.0"

[dependencies.b]
version = ">=1.0.0"
git = "https://example.com/b.git"
EOF
"${TKC}" pkg resolve >out 2>err; expect "10b-ok-rc" "$?" "0"
cp pkg.lock beforelock
printf '[package]\nname = "myapp"\nversion = "0.1.0"\n[dependencies]\nmyapp = "1.0.0"\n' > pkg.toml
"${TKC}" pkg resolve >out 2>err; expect "10c-self-dep-rc" "$?" "1"; diag_is "10d-self-dep-E9033" err E9033
cmp -s pkg.lock beforelock && ok "10e-lock-untouched-on-error" || bad "10e-lock-untouched-on-error"
printf '[package]\nname = "myapp"\nversion = "0.1.0"\n[dependencies]\nx = "1.0.0"\n[scripts]\npost = "sh evil.sh"\n' > pkg.toml
"${TKC}" pkg resolve >out 2>err; expect "10f-malformed-rc" "$?" "1"; diag_is "10g-malformed-E9012" err E9012
grep -q '"line":6' err && ok "10h-malformed-line" || bad "10h-malformed-line" "$(cat err)"
printf 'garbage\n' > pkg.lock
printf '[package]\nname = "myapp"\nversion = "0.1.0"\n' > pkg.toml
"${TKC}" pkg resolve >out 2>err; expect "10i-bad-lock-rc" "$?" "1"; diag_is "10j-bad-lock-E9028" err E9028
"${TKC}" pkg list >out 2>err; expect "10k-list-bad-lock-rc" "$?" "1"
rm -f pkg.lock; "${TKC}" pkg list >out 2>err; diag_is "10l-list-no-lock-E9028" err E9028
rm -f pkg.toml; "${TKC}" pkg resolve >out 2>err; expect "10m-no-manifest-rc" "$?" "1"; diag_is "10n-no-manifest-E9032" err E9032

# 11. --diag-text gives the same code in text form
"${TKC}" pkg resolve --diag-text >out 2>err
grep -q 'E9032' err && ! grep -q '"error_code"' err && ok "11-diag-text" || bad "11-diag-text" "$(cat err)"

echo "C1160: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
