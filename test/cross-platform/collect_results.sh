#!/bin/sh
# collect_results.sh — Cross-platform test result collector for toke/tkc.
#
# Detects the current platform, builds tkc, runs conformance and e2e tests,
# and emits a JSON result document to stdout matching result-schema.json.
#
# Supported: Linux, macOS (Darwin), FreeBSD.
# Usage:  sh test/cross-platform/collect_results.sh
#
# The script must be run from the toke repo root (the directory containing
# the Makefile).

set -e

# ── Helpers ──────────────────────────────────────────────────────────────────

json_escape() {
    # Escape a string for safe embedding in a JSON value.
    # Handles backslash, double-quote, and newlines.
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/	/\\t/g' | tr '\n' ' '
}

# ── Repo root ────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# ── Platform detection ───────────────────────────────────────────────────────

OS="$(uname -s)"
MACHINE="$(uname -m)"

# Normalise architecture name
case "${MACHINE}" in
    x86_64|amd64)  ARCH="x86_64"  ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *)             ARCH="${MACHINE}" ;;
esac

# Detect OS version string
case "${OS}" in
    Linux)
        if [ -f /etc/os-release ]; then
            . /etc/os-release
            PLATFORM="${NAME} ${VERSION_ID:-unknown}"
        elif command -v lsb_release >/dev/null 2>&1; then
            PLATFORM="$(lsb_release -ds 2>/dev/null || echo "Linux unknown")"
        else
            PLATFORM="Linux $(uname -r)"
        fi
        ;;
    Darwin)
        SW_VERS="$(sw_vers -productVersion 2>/dev/null || echo "unknown")"
        PLATFORM="macOS ${SW_VERS}"
        ;;
    FreeBSD)
        PLATFORM="FreeBSD $(freebsd-version 2>/dev/null || uname -r)"
        ;;
    *)
        PLATFORM="${OS} $(uname -r)"
        ;;
esac

# ── Compiler detection ──────────────────────────────────────────────────────

COMPILER_VERSION=""
LLVM_VERSION=""
if command -v cc >/dev/null 2>&1; then
    COMPILER_VERSION="$(cc --version 2>/dev/null | head -1)"
    # Try to extract LLVM/clang version number
    LLVM_VERSION="$(echo "${COMPILER_VERSION}" | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p')"
fi

# ── libc detection ───────────────────────────────────────────────────────────

LIBC=""
case "${OS}" in
    Linux)
        if command -v ldd >/dev/null 2>&1; then
            LIBC="$(ldd --version 2>&1 | head -1 | sed 's/^ldd //' || echo "unknown")"
        else
            LIBC="unknown"
        fi
        ;;
    Darwin)
        LIBC="libSystem (macOS ${SW_VERS})"
        ;;
    FreeBSD)
        LIBC="FreeBSD libc"
        ;;
    *)
        LIBC="unknown"
        ;;
esac

# ── Timestamp ────────────────────────────────────────────────────────────────

TIMESTAMP="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

# ── Build toke ───────────────────────────────────────────────────────────────

NOTES=""
BUILD_SUCCESS="true"
BUILD_ERRORS=""
BUILD_WARNINGS=0
BUILD_TIME=0
BINARY_SIZE=0

BUILD_LOG="$(mktemp /tmp/toke_build_XXXXXX.log)"
trap 'rm -f "${BUILD_LOG}"' EXIT

BUILD_START="$(date +%s)"

if make clean >/dev/null 2>&1 && make 2>"${BUILD_LOG}"; then
    BUILD_SUCCESS="true"
else
    BUILD_SUCCESS="false"
    # Collect error lines
    BUILD_ERRORS="$(grep -i 'error:' "${BUILD_LOG}" | head -20 || true)"
fi

BUILD_END="$(date +%s)"
BUILD_TIME=$((BUILD_END - BUILD_START))

# Count warnings from build log
if [ -f "${BUILD_LOG}" ]; then
    BUILD_WARNINGS="$(grep -c 'warning:' "${BUILD_LOG}" 2>/dev/null || echo 0)"
fi

# Binary size
if [ -f "${REPO_ROOT}/tkc" ]; then
    # Portable: use wc -c instead of stat (syntax varies across platforms)
    BINARY_SIZE="$(wc -c < "${REPO_ROOT}/tkc" | tr -d ' ')"
fi

# ── Format build errors as JSON array ────────────────────────────────────────

BUILD_ERRORS_JSON="[]"
if [ -n "${BUILD_ERRORS}" ]; then
    BUILD_ERRORS_JSON="["
    first=true
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        escaped="$(json_escape "${line}")"
        if ${first}; then
            first=false
        else
            BUILD_ERRORS_JSON="${BUILD_ERRORS_JSON},"
        fi
        BUILD_ERRORS_JSON="${BUILD_ERRORS_JSON}\"${escaped}\""
    done <<EOF
${BUILD_ERRORS}
EOF
    BUILD_ERRORS_JSON="${BUILD_ERRORS_JSON}]"
fi

# ── Run conformance tests ───────────────────────────────────────────────────

CONFORM_PASS=0
CONFORM_FAIL=0
CONFORM_SKIP=0

if [ "${BUILD_SUCCESS}" = "true" ] && [ -f test/run_conform.sh ]; then
    CONFORM_OUT="$(bash test/run_conform.sh 2>&1 || true)"
    # Parse the summary line: "Results: N passed, N failed, N skipped"
    CONFORM_PASS="$(echo "${CONFORM_OUT}" | sed -n 's/.*Results: *\([0-9][0-9]*\) passed.*/\1/p' | tail -1)"
    CONFORM_FAIL="$(echo "${CONFORM_OUT}" | sed -n 's/.*Results:.*\([0-9][0-9]*\) failed.*/\1/p' | tail -1)"
    CONFORM_SKIP="$(echo "${CONFORM_OUT}" | sed -n 's/.*Results:.*\([0-9][0-9]*\) skipped.*/\1/p' | tail -1)"
    # Also try the simpler format used by some versions
    if [ -z "${CONFORM_PASS}" ]; then
        CONFORM_PASS="$(echo "${CONFORM_OUT}" | sed -n 's/.*\([0-9][0-9]*\) passed.*/\1/p' | tail -1)"
    fi
    if [ -z "${CONFORM_FAIL}" ]; then
        CONFORM_FAIL="$(echo "${CONFORM_OUT}" | sed -n 's/.*\([0-9][0-9]*\) failed.*/\1/p' | tail -1)"
    fi
    CONFORM_PASS="${CONFORM_PASS:-0}"
    CONFORM_FAIL="${CONFORM_FAIL:-0}"
    CONFORM_SKIP="${CONFORM_SKIP:-0}"
else
    NOTES="${NOTES}conformance tests skipped (build failed or runner missing);"
fi

# ── Run e2e tests ────────────────────────────────────────────────────────────

E2E_PASS=0
E2E_FAIL=0
E2E_SKIP=0

if [ "${BUILD_SUCCESS}" = "true" ] && [ -f test/e2e/run_e2e.sh ]; then
    E2E_OUT="$(bash test/e2e/run_e2e.sh 2>&1 || true)"
    # Parse the summary line: "N passed, N failed"
    E2E_PASS="$(echo "${E2E_OUT}" | sed -n 's/.*\([0-9][0-9]*\) passed.*/\1/p' | tail -1)"
    E2E_FAIL="$(echo "${E2E_OUT}" | sed -n 's/.*\([0-9][0-9]*\) failed.*/\1/p' | tail -1)"
    E2E_SKIP="$(echo "${E2E_OUT}" | grep -c '^SKIP ' 2>/dev/null || echo 0)"
    E2E_PASS="${E2E_PASS:-0}"
    E2E_FAIL="${E2E_FAIL:-0}"
    E2E_SKIP="${E2E_SKIP:-0}"
else
    NOTES="${NOTES}e2e tests skipped (build failed or runner missing);"
fi

# ── Stdlib tests (placeholder — no unified runner yet) ───────────────────────

STDLIB_PASS=0
STDLIB_FAIL=0
STDLIB_SKIP=0
NOTES="${NOTES}stdlib test collection not yet automated;"

# ── ooke build (optional — only if ooke repo is sibling) ────────────────────

OOKE_JSON=""
OOKE_DIR="${REPO_ROOT}/../ooke"
if [ -d "${OOKE_DIR}" ] && [ -f "${OOKE_DIR}/Makefile" ]; then
    OOKE_SUCCESS="true"
    OOKE_SIZE=0
    OOKE_START="$(date +%s)"
    if (cd "${OOKE_DIR}" && make clean >/dev/null 2>&1 && make >/dev/null 2>&1); then
        OOKE_SUCCESS="true"
    else
        OOKE_SUCCESS="false"
    fi
    OOKE_END="$(date +%s)"
    OOKE_TIME=$((OOKE_END - OOKE_START))
    if [ -f "${OOKE_DIR}/ooke" ]; then
        OOKE_SIZE="$(wc -c < "${OOKE_DIR}/ooke" | tr -d ' ')"
    fi
    OOKE_JSON="\"ooke_build\": {\"success\": ${OOKE_SUCCESS}, \"time_secs\": ${OOKE_TIME}, \"binary_size_bytes\": ${OOKE_SIZE}},"
fi

# ── Format notes as JSON array ───────────────────────────────────────────────

NOTES_JSON="[]"
if [ -n "${NOTES}" ]; then
    NOTES_JSON="["
    first=true
    # Split on semicolons
    OLD_IFS="${IFS}"
    IFS=";"
    for note in ${NOTES}; do
        note="$(echo "${note}" | sed 's/^ *//;s/ *$//')"
        [ -z "${note}" ] && continue
        escaped="$(json_escape "${note}")"
        if ${first}; then
            first=false
        else
            NOTES_JSON="${NOTES_JSON},"
        fi
        NOTES_JSON="${NOTES_JSON}\"${escaped}\""
    done
    IFS="${OLD_IFS}"
    NOTES_JSON="${NOTES_JSON}]"
fi

# ── Emit JSON ────────────────────────────────────────────────────────────────

cat <<ENDJSON
{
  "platform": "$(json_escape "${PLATFORM}")",
  "arch": "${ARCH}",
  "timestamp": "${TIMESTAMP}",
  "compiler": "$(json_escape "${COMPILER_VERSION}")",
  "llvm_version": "${LLVM_VERSION}",
  "libc": "$(json_escape "${LIBC}")",
  "toke_build": {
    "success": ${BUILD_SUCCESS},
    "time_secs": ${BUILD_TIME},
    "binary_size_bytes": ${BINARY_SIZE},
    "warnings": ${BUILD_WARNINGS},
    "errors": ${BUILD_ERRORS_JSON}
  },
  "tests": {
    "conformance": {"pass": ${CONFORM_PASS}, "fail": ${CONFORM_FAIL}, "skip": ${CONFORM_SKIP}},
    "e2e": {"pass": ${E2E_PASS}, "fail": ${E2E_FAIL}, "skip": ${E2E_SKIP}},
    "stdlib": {"pass": ${STDLIB_PASS}, "fail": ${STDLIB_FAIL}, "skip": ${STDLIB_SKIP}}
  },
  ${OOKE_JSON}
  "notes": ${NOTES_JSON}
}
ENDJSON
