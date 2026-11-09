#!/usr/bin/env bash
# C1161_pkg_git_fetch_local.sh — `tkc pkg fetch`: git-based package fetch,
# pinned and verified (story 76.1.3d; ADR-0004 "Supply-chain security").
#
# THE GAP. ADR-0004 specified the fetch -- clone at the version's tag, pin
# the commit and a content hash on first use, refuse any later fetch or cache
# that does not reproduce both -- and src/pkg.c held the checks
# (pkg_lock_check_pinned, pkg_verify_fetched), but nothing fetched.
#
# NO NETWORK. Every repository is a bare repository created here and reached
# through file:// under the ADR's explicit local-path override
# (--allow-file-source).  No remote is contacted.  HOME is a scratch
# directory, so the package cache ($HOME/.toke/pkg) is too.
#
# The content hash is checked against an independent computation from the
# ADR's definition (sorted regular files, "path NUL size NUL bytes").
#
# Negative control: TKC=<main's tkc> -- every case fails (no `pkg` command).
#
# Story: 76.1.3d

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }
diag_is() {   # name file code
    if grep -q '"error_code":"'"$3"'"' "$2" 2>/dev/null && ! grep -q '"fix":"[^"]' "$2"; then ok "$1"
    else bad "$1" "want $3 in: $(head -c 400 "$2")"; fi
}
command -v git >/dev/null 2>&1 || { echo "SKIP C1161: git not installed"; exit 0; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1161_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
export HOME="${WORK}/home"; mkdir -p "${HOME}"
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
export GIT_CONFIG_NOSYSTEM=1
G() { git -c user.name=t -c user.email=t@example.invalid -c init.defaultBranch=main \
          -c commit.gpgsign=false -c tag.gpgsign=false "$@"; }
R="${WORK}/repos"; mkdir -p "${R}"
CACHE="${HOME}/.toke/pkg"
AF=--allow-file-source

# mkrepo <name> -- a working repo at $R/<name>.src and a bare clone $R/<name>.git
mkrepo() { mkdir -p "${R}/$1.src"; G -C "${R}/$1.src" init -q; G init -q --bare "${R}/$1.git"; }
# release <name> <version> <pkg.toml deps block> -- commit, tag v<version>, push
release() {
    local d="${R}/$1.src"
    printf '[package]\nname = "%s"\nversion = "%s"\n%s' "$1" "$2" "$3" > "${d}/pkg.toml"
    printf 'm=pkg.%s;\nf=ver():str{ <"%s" };\n' "$1" "$2" > "${d}/$1.tk"
    G -C "${d}" add -A && G -C "${d}" commit -qm "$1 $2" && G -C "${d}" tag -a -m "v$2" "v$2"
    G -C "${d}" push -q --force --tags "${R}/$1.git" HEAD:refs/heads/main 2>/dev/null
}
# independent canonical tree hash (ADR-0004)
treehash() {
    python3 - "$1" <<'PY'
import hashlib, os, sys
root = sys.argv[1]; files = []
for dp, dn, fn in os.walk(root):
    for f in fn:
        p = os.path.join(dp, f); files.append(os.path.relpath(p, root).replace(os.sep, '/'))
h = hashlib.sha256()
for rel in sorted(files, key=lambda s: s.encode()):
    b = open(os.path.join(root, rel), 'rb').read()
    h.update(rel.encode() + b'\0' + str(len(b)).encode() + b'\0' + b)
print('sha256:' + h.hexdigest())
PY
}
lockval() { awk -v t="[$2]" -v k="$3" '$0==t{s=1;next} /^\[/{s=0} s && $1==k{gsub(/"/,"",$3); print $3}' "$1"; }

mkrepo util;  release util 1.0.0 ''
mkrepo greet; release greet 1.0.0 ''
release greet 1.1.0 "[dependencies.util]
version = \">=1.0.0\"
git = \"file://${R}/util.git\"
"
P="${WORK}/app"; mkdir -p "${P}"; cd "${P}" || exit 1
"${TKC}" pkg init app >/dev/null 2>&1

# 1. the transport rule holds without the override
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.greet]\nversion = "1.0.0"\ngit = "file://%s/greet.git"\n' "${R}" > pkg.toml
"${TKC}" pkg fetch >out 2>err; expect "1a-no-override-rc" "$?" "1"; diag_is "1b-no-override-E9012" err E9012
[ ! -d "${CACHE}/greet" ] && ok "1c-nothing-fetched" || bad "1c-nothing-fetched"

# 2. first fetch: trust on first use records source, commit and hash
"${TKC}" pkg fetch ${AF} >out 2>err; expect "2a-fetch-rc" "$?" "0"
grep -q 'fetching greet 1.0.0 ... ok' out && ok "2b-fetch-output" || bad "2b-fetch-output" "$(cat out err)"
WANT_C="$(git -C "${R}/greet.git" rev-parse 'v1.0.0^{commit}')"
expect "2c-commit-pinned" "$(lockval pkg.lock commit greet)" "${WANT_C}"
expect "2d-hash-pinned" "$(lockval pkg.lock hash greet)" "$(treehash "${CACHE}/greet/1.0.0")"
expect "2e-source-pinned" "$(lockval pkg.lock source greet)" "file://${R}/greet.git"
[ ! -e "${CACHE}/greet/1.0.0/.git" ] && [ -f "${CACHE}/greet/1.0.0/greet.tk" ] \
    && ok "2f-working-tree-only" || bad "2f-working-tree-only" "$(ls -a "${CACHE}/greet/1.0.0")"
expect "2g-commit-sidecar" "$(cat "${CACHE}/greet/1.0.0.commit")" "${WANT_C}"
ls "${CACHE}/greet" | grep -q '^\.tmp' && bad "2h-no-temp-left" || ok "2h-no-temp-left"

# 3. second fetch: verified from the cache, lock byte-identical
cp pkg.lock lock1
"${TKC}" pkg fetch ${AF} >out 2>err; expect "3a-refetch-rc" "$?" "0"
grep -q 'greet 1.0.0 ... ok (cached)' out && ok "3b-cached" || bad "3b-cached" "$(cat out)"
cmp -s pkg.lock lock1 && ok "3c-lock-stable" || bad "3c-lock-stable"

# 4. transitive: greet 1.1.0 requires util; util is fetched and pinned too
"${TKC}" pkg add greet ">=1.1.0" --git "file://${R}/greet.git" ${AF} >out 2>err
expect "4a-add-rc" "$?" "0"
"${TKC}" pkg fetch ${AF} >out 2>err; expect "4b-fetch-rc" "$?" "0"
expect "4c-util-locked" "$(lockval pkg.lock resolved util)" "1.0.0"
expect "4d-util-pinned" "$(lockval pkg.lock hash util)" "$(treehash "${CACHE}/util/1.0.0")"
"${TKC}" pkg list ${AF} >out 2>err
expect "4e-list" "$(cat out)" 'direct:
  greet          1.1.0
transitive:
  util           1.0.0  (via greet)'

# 5. a moved tag: the same version now names another commit -- refused
cp pkg.lock lock2
echo 'f=extra():i64{ <1 };' >> "${R}/greet.src/greet.tk"
G -C "${R}/greet.src" commit -qam moved && G -C "${R}/greet.src" tag -f -a -m moved v1.1.0 >/dev/null
G -C "${R}/greet.src" push -q --force "${R}/greet.git" refs/tags/v1.1.0 2>/dev/null
rm -rf "${CACHE}/greet/1.1.0" "${CACHE}/greet/1.1.0.commit"
"${TKC}" pkg fetch ${AF} >out 2>err; expect "5a-moved-tag-rc" "$?" "1"; diag_is "5b-moved-tag-E9030" err E9030
grep -q 'commit mismatch' err && ok "5c-names-commit" || bad "5c-names-commit" "$(cat err)"
cmp -s pkg.lock lock2 && ok "5d-lock-untouched" || bad "5d-lock-untouched"
[ ! -d "${CACHE}/greet/1.1.0" ] && ok "5e-not-installed" || bad "5e-not-installed"
# put the tag back where the lock pins it
G -C "${R}/greet.src" tag -f -a -m back v1.1.0 "$(lockval pkg.lock commit greet)" >/dev/null
G -C "${R}/greet.src" push -q --force "${R}/greet.git" refs/tags/v1.1.0 2>/dev/null
"${TKC}" pkg fetch ${AF} >out 2>err; expect "5f-restored-rc" "$?" "0"

# 6. a tampered cache is refused (the cache is not trusted)
echo '(* changed *)' >> "${CACHE}/util/1.0.0/util.tk"
"${TKC}" pkg fetch ${AF} >out 2>err; expect "6a-tamper-rc" "$?" "1"; diag_is "6b-tamper-E9030" err E9030
grep -q 'content hash mismatch' err && ok "6c-names-hash" || bad "6c-names-hash" "$(cat err)"
rm -rf "${CACHE}/util"
"${TKC}" pkg fetch ${AF} >out 2>err; expect "6d-refetch-after-delete-rc" "$?" "0"

# 7. no tag for the version: E9014
P7="${WORK}/app7"; mkdir -p "${P7}"; cd "${P7}" || exit 1
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.greet]\nversion = "9.0.0"\ngit = "file://%s/greet.git"\n' "${R}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "7a-notag-rc" "$?" "1"; diag_is "7b-notag-E9014" err E9014
[ ! -e pkg.lock ] && ok "7c-no-lock-on-error" || bad "7c-no-lock-on-error"

# 8. a repository that is not there: E9013; and git never runs through a shell
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.nope]\nversion = "1.0.0"\ngit = "file://%s/x$(touch${IFS}%s/pwned)"\n' "${R}" "${WORK}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "8a-missing-repo-rc" "$?" "1"; diag_is "8b-missing-repo-E9013" err E9013
[ ! -e "${WORK}/pwned" ] && ok "8c-no-shell" || bad "8c-no-shell"
# ext:: stays refused even under the override
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.nope]\nversion = "1.0.0"\ngit = "ext::sh -c touch%%20%s/pwned2"\n' "${WORK}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "8d-ext-rc" "$?" "1"
[ ! -e "${WORK}/pwned2" ] && ok "8e-ext-not-run" || bad "8e-ext-not-run"

# 9. a package whose pkg.toml names another package or version: E9030
mkrepo liar; release liar 1.0.0 ''
sed -i.bak 's/^version = "1.0.0"/version = "2.0.0"/' "${R}/liar.src/pkg.toml" && rm -f "${R}/liar.src/pkg.toml.bak"
G -C "${R}/liar.src" commit -qam lie && G -C "${R}/liar.src" tag -a -m v1.0.1 v1.0.1 && G -C "${R}/liar.src" push -q --tags "${R}/liar.git" 2>/dev/null
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.liar]\nversion = "1.0.1"\ngit = "file://%s/liar.git"\n' "${R}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "9a-identity-rc" "$?" "1"; diag_is "9b-identity-E9030" err E9030
[ ! -d "${CACHE}/liar/1.0.1" ] && ok "9c-not-installed" || bad "9c-not-installed"

# 10. a cycle between packages: E9033
mkrepo cyca; mkrepo cycb
release cyca 1.0.0 "[dependencies.cycb]
version = \"1.0.0\"
git = \"file://${R}/cycb.git\"
"
release cycb 1.0.0 "[dependencies.cyca]
version = \"1.0.0\"
git = \"file://${R}/cyca.git\"
"
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.cyca]\nversion = "1.0.0"\ngit = "file://%s/cyca.git"\n' "${R}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "10a-cycle-rc" "$?" "1"; diag_is "10b-cycle-E9033" err E9033
grep -q 'cyca -> cycb -> cyca' err && ok "10c-cycle-path" || bad "10c-cycle-path" "$(cat err)"

# 11. no source and no index: E9011
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies]\nghost = "1.0.0"\n' > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "11a-nosource-rc" "$?" "1"; diag_is "11b-nosource-E9011" err E9011

# 12. two sources for one package: E9034
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.greet]\nversion = "1.1.0"\ngit = "file://%s/greet.git"\n[dependencies.util]\nversion = "1.0.0"\ngit = "file://%s/liar.git"\n' "${R}" "${R}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "12a-two-sources-rc" "$?" "1"; diag_is "12b-two-sources-E9034" err E9034

# 13. a symlink makes a package unhashable: refused
mkrepo link; release link 1.0.0 ''
sed -i.bak 's/1.0.0/1.0.1/' "${R}/link.src/pkg.toml"; rm -f "${R}/link.src/pkg.toml.bak"
ln -s /etc/passwd "${R}/link.src/pw" && G -C "${R}/link.src" add -A && G -C "${R}/link.src" commit -qm l \
  && G -C "${R}/link.src" tag -a -m v1.0.1 v1.0.1 && G -C "${R}/link.src" push -q --tags "${R}/link.git" 2>/dev/null
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.link]\nversion = "1.0.1"\ngit = "file://%s/link.git"\n' "${R}" > pkg.toml
"${TKC}" pkg fetch ${AF} >out 2>err; expect "13a-symlink-rc" "$?" "1"; diag_is "13b-symlink-E9030" err E9030
grep -q 'cannot be hashed' err && ok "13c-symlink-named" || bad "13c-symlink-named" "$(cat err)"

# 14. a partly pinned lock is refused, not silently re-pinned: E9029
cd "${P}" || exit 1
awk '!/^util = "sha256:/' pkg.lock > l2 && mv l2 pkg.lock
"${TKC}" pkg fetch ${AF} >out 2>err; expect "14a-partial-rc" "$?" "1"; diag_is "14b-partial-E9029" err E9029
cp lock2 pkg.lock

# 15. ambient GIT_DIR / GIT_INDEX_FILE cannot redirect git
rm -rf "${CACHE}/util"
GIT_DIR="${WORK}/nowhere" GIT_INDEX_FILE="${WORK}/idx" "${TKC}" pkg fetch ${AF} >out 2>err
expect "15a-env-scrubbed-rc" "$?" "0"
grep -q 'fetching util 1.0.0 ... ok$' out && ok "15b-env-scrubbed-fetched" || bad "15b-env-scrubbed-fetched" "$(cat out err)"

echo "C1161: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
