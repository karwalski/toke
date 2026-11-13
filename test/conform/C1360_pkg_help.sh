#!/usr/bin/env bash
# C1360_pkg_help.sh — `tkc pkg --help`, `tkc pkg help [<command>]` and
# `tkc pkg <command> --help` print usage on stdout and exit 0 (132.142).
#
# THE DEFECT.  `tkc pkg --help` and `tkc pkg help` were rejected as an unknown
# command (E9031, exit 3), and `tkc pkg add --help` as an unknown flag.  A
# command's usage existed only as the E9031 text printed for a wrong argument
# count, which docs/reference/cli.md had to scrape.
#
# NOW the commands are one table (PKG_CMDS in src/pkg_cli.c); the help and the
# E9031 usage diagnostic both print from it.
#
# CASES.
#   1. `tkc pkg --help` and `tkc pkg help`: exit 0, stdout byte-identical to
#      fixtures/help/tkc_pkg_help.txt (the C718 convention: an intentional help
#      change updates the fixture), stderr empty.
#   2. For each command, `tkc pkg <cmd> --help` and `tkc pkg help <cmd>`: exit 0,
#      stdout identical to fixtures/help/tkc_pkg_<cmd>_help.txt, stderr empty,
#      and nothing written in the working directory (`init --help` must not
#      create pkg.toml).
#   3. Each command's help Usage line is the E9031 usage diagnostic it gives for
#      a wrong argument count (the single table).
#   4. Real misuse keeps E9031 and exit 3, with nothing on stdout:
#      `tkc pkg`, `tkc pkg bogus`, `tkc pkg bogus --help`, `tkc pkg help bogus`,
#      `tkc pkg add --bogus --help`, `tkc pkg --help extra`.
#
# Negative control: TKC=<main's tkc, 6692ac91> fails 1 and 2 (E9031, exit 3).
#
# Story: 132.142

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
FIX="${SCRIPT_DIR}/fixtures/help"
PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1360_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
export HOME="${WORK}/home"; mkdir -p "${HOME}"
P="${WORK}/proj"; mkdir -p "${P}"; cd "${P}" || exit 1

# help_is NAME FIXTURE ARGS...: exit 0, stdout == fixture, stderr empty,
# nothing written in the project directory.
help_is() {
    local name="$1" fix="$2"; shift 2
    "${TKC}" "$@" >"${WORK}/out" 2>"${WORK}/err"; local rc=$?
    [ "${rc}" -eq 0 ] && ok "${name} exits 0" || bad "${name} exits 0" "rc=${rc}: $(head -c 300 "${WORK}/err")"
    if cmp -s "${WORK}/out" "${fix}"; then ok "${name} stdout matches $(basename "${fix}")"
    else bad "${name} stdout matches $(basename "${fix}")" "$(diff "${fix}" "${WORK}/out" | head -8)"; fi
    [ ! -s "${WORK}/err" ] && ok "${name} stderr empty" || bad "${name} stderr empty" "$(head -c 300 "${WORK}/err")"
    [ -z "$(ls -A "${P}")" ] && ok "${name} writes nothing" || bad "${name} writes nothing" "$(ls -A "${P}")"
}

# misuse NAME ARGS...: E9031, exit 3, stdout empty
misuse() {
    local name="$1"; shift
    "${TKC}" "$@" >"${WORK}/out" 2>"${WORK}/err"; local rc=$?
    if [ "${rc}" -eq 3 ] && grep -q '"error_code":"E9031"' "${WORK}/err" && [ ! -s "${WORK}/out" ]; then
        ok "${name} is E9031, exit 3"
    else
        bad "${name} is E9031, exit 3" "rc=${rc} out=$(head -c 200 "${WORK}/out") err=$(head -c 200 "${WORK}/err")"
    fi
}

# ── 1. top-level help ─────────────────────────────────────────────────
help_is "pkg --help" "${FIX}/tkc_pkg_help.txt" pkg --help
help_is "pkg help"   "${FIX}/tkc_pkg_help.txt" pkg help

# ── 2/3. each command ─────────────────────────────────────────────────
for c in init add remove resolve fetch list; do
    help_is "pkg ${c} --help" "${FIX}/tkc_pkg_${c}_help.txt" pkg "${c}" --help
    help_is "pkg help ${c}"   "${FIX}/tkc_pkg_${c}_help.txt" pkg help "${c}"
    usage="$(head -1 "${FIX}/tkc_pkg_${c}_help.txt" | sed 's/^Usage: //')"
    "${TKC}" pkg "${c}" w x y z >/dev/null 2>"${WORK}/err"
    msg="$(sed -n 's/.*"message":"\([^"]*\)".*/\1/p' "${WORK}/err" | head -1)"
    if [ "${msg}" = "usage: ${usage}" ]; then ok "pkg ${c}: help Usage line is the E9031 usage"
    else bad "pkg ${c}: help Usage line is the E9031 usage" "help [${usage}] diag [${msg}]"; fi
done

# ── 4. misuse is still E9031 ──────────────────────────────────────────
misuse "pkg (no command)"      pkg
misuse "pkg bogus"             pkg bogus
misuse "pkg bogus --help"      pkg bogus --help
misuse "pkg help bogus"        pkg help bogus
misuse "pkg add --bogus --help" pkg add --bogus --help
misuse "pkg --help extra"      pkg --help extra
[ -z "$(ls -A "${P}")" ] && ok "misuse writes nothing" || bad "misuse writes nothing" "$(ls -A "${P}")"

echo "C1360: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
