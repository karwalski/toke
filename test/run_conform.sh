#!/usr/bin/env bash
# run_conform.sh — tkc conformance test runner
#
# Usage:
#   bash test/run_conform.sh              # run all series
#   SERIES=L bash test/run_conform.sh     # run only L-series tests
#
# Requires: tkc binary at ./tkc (relative to the repo root, i.e. the
# directory that contains this script's parent directory).
# Requires: python3 (for YAML parsing) or the inline awk fallback.
#
# Exit code: 0 if all tests pass, 1 if any test fails or tkc is absent.

set -euo pipefail

# ── Locate repo root (directory that contains the Makefile and tkc binary) ──
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TKC="${REPO_ROOT}/tkc"
TEST_DIR="${SCRIPT_DIR}"

# ── Colour codes (disabled if not a terminal) ───────────────────────────────
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    YELLOW='\033[0;33m'
    RESET='\033[0m'
else
    GREEN='' RED='' YELLOW='' RESET=''
fi

# ── Check tkc binary exists ──────────────────────────────────────────────────
if [ ! -x "${TKC}" ]; then
    echo "${YELLOW}WARNING: tkc binary not found at ${TKC}${RESET}"
    echo "         The compiler has not been built yet."
    echo "         Run 'make' to build tkc, then re-run 'make conform'."
    echo ""
    echo "         Conformance suite is DEFINED but cannot be EXECUTED until tkc exists."
    exit 1
fi

# ── Python3 YAML extraction helper ──────────────────────────────────────────
# We use python3 to extract fields from YAML test files.  If python3 is not
# available the runner falls back to a minimal awk-based extractor that handles
# the simple key: value subset we use in test files.

yaml_get() {
    # yaml_get <file> <key>
    # Prints the value of a top-level scalar key from a YAML file.
    local file="$1" key="$2"
    if command -v python3 &>/dev/null; then
        python3 - "${file}" "${key}" <<'PYEOF'
import sys, re

path, key = sys.argv[1], sys.argv[2]
with open(path) as f:
    content = f.read()

# Handle multi-line block scalar (key: |)
block = re.search(r'^' + re.escape(key) + r':\s*\|\n((?:  .*\n?)*)', content, re.MULTILINE)
if block:
    # Strip two-space indent from each line
    lines = block.group(1).split('\n')
    print('\n'.join(l[2:] if l.startswith('  ') else l for l in lines).rstrip())
    sys.exit(0)

# Handle inline scalar (key: value or key: "value")
inline = re.search(r'^' + re.escape(key) + r':\s*(.+)$', content, re.MULTILINE)
if inline:
    val = inline.group(1).strip()
    if val.startswith('"') and val.endswith('"'):
        # Decode YAML double-quoted string escapes
        val = val[1:-1]
        val = val.replace('\\r', '\r').replace('\\n', '\n').replace('\\t', '\t')
        val = val.replace('\\\\"', '"').replace('\\\\', '\\')
    print(val, end='')
    sys.exit(0)

sys.exit(0)  # key not found → empty output
PYEOF
    else
        # Minimal awk fallback for simple key: value lines only
        awk -v k="${key}" '
            $0 ~ "^"k": " {
                sub(/^[^:]+: */, ""); sub(/^"/, ""); sub(/"$/, ""); print; exit
            }
        ' "${file}"
    fi
}

yaml_get_list() {
    # yaml_get_list <file> <key>
    # Returns newline-separated list items from a YAML sequence value.
    # Handles both inline ["E1001","E1002"] and block-sequence formats.
    local file="$1" key="$2"
    if command -v python3 &>/dev/null; then
        python3 - "${file}" "${key}" <<'PYEOF'
import sys, re

path, key = sys.argv[1], sys.argv[2]
with open(path) as f:
    content = f.read()

# Inline list: key: ["a", "b"]
inline = re.search(r'^' + re.escape(key) + r':\s*\[([^\]]*)\]', content, re.MULTILINE)
if inline:
    items = inline.group(1)
    for item in items.split(','):
        item = item.strip().strip('"').strip("'")
        if item:
            print(item)
    sys.exit(0)

# Block list:
# key:
#   - "a"
block = re.search(r'^' + re.escape(key) + r':\s*\n((?:\s+-\s+.*\n?)*)', content, re.MULTILINE)
if block:
    for line in block.group(1).split('\n'):
        m = re.match(r'\s+-\s+"?([^"]+)"?\s*$', line)
        if m:
            print(m.group(1))
    sys.exit(0)

sys.exit(0)
PYEOF
    else
        awk -v k="${key}" '
            $0 ~ "^"k": \\[" {
                gsub(/.*\[/, ""); gsub(/\].*/, ""); gsub(/"/, ""); gsub(/ /, "")
                n = split($0, a, ",")
                for (i=1;i<=n;i++) if (a[i]!="") print a[i]
                exit
            }
        ' "${file}"
    fi
}

yaml_bool() {
    # yaml_bool <file> <key> → exits 0 if key is present and equals "true", else 1
    local val
    val="$(yaml_get "$1" "$2")"
    [ "${val}" = "true" ]
}

# ── Test execution ───────────────────────────────────────────────────────────

PASS=0
FAIL=0
SKIP=0
FAILURES=()

run_test() {
    local yaml_file="$1"
    local test_id series description input_src
    local expected_exit expected_fix expected_fix_absent
    local diag_json_output actual_exit

    test_id="$(yaml_get "${yaml_file}" "id")"
    series="$(yaml_get "${yaml_file}" "series")"
    description="$(yaml_get "${yaml_file}" "description")"

    # Series filter
    if [ -n "${SERIES:-}" ] && [ "${series}" != "${SERIES}" ]; then
        return
    fi

    input_src="$(yaml_get "${yaml_file}" "input")"
    expected_exit="$(yaml_get "${yaml_file}" "expected_exit_code")"
    expected_fix="$(yaml_get "${yaml_file}" "expected_fix" 2>/dev/null || true)"
    expected_fix_absent="$(yaml_get "${yaml_file}" "expected_fix_absent" 2>/dev/null || echo "false")"

    # Write input to a temp file
    local tmpfile
    tmpfile="$(mktemp /tmp/tkc_conform_XXXXXX.tk)"
    printf '%s\n' "${input_src}" > "${tmpfile}"

    # Run tkc --check --diag-json against the input
    diag_json_output=""
    actual_exit=0
    diag_json_output="$("${TKC}" --check --diag-json "${tmpfile}" 2>&1)" || actual_exit=$?

    rm -f "${tmpfile}"

    local pass=true
    local fail_reasons=()

    # ── Check exit code ──────────────────────────────────────────────────────
    if [ "${actual_exit}" != "${expected_exit}" ]; then
        pass=false
        fail_reasons+=("exit code: expected ${expected_exit}, got ${actual_exit}")
    fi

    # ── Check expected error codes ───────────────────────────────────────────
    local expected_codes=()
    while IFS= read -r code; do
        [ -n "${code}" ] && expected_codes+=("${code}")
    done < <(yaml_get_list "${yaml_file}" "expected_error_codes")

    for code in "${expected_codes[@]+"${expected_codes[@]}"}"; do
        if ! echo "${diag_json_output}" | grep -q "\"${code}\""; then
            pass=false
            fail_reasons+=("expected error code ${code} not found in diagnostic output")
        fi
    done

    # ── Check fix field ──────────────────────────────────────────────────────
    if [ "${expected_fix_absent}" = "true" ]; then
        # The fix field must be absent (null, missing, or empty string) in ALL emitted diagnostics
        if echo "${diag_json_output}" | python3 -c "
import sys, json
output = sys.stdin.read()
for line in output.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        rec = json.loads(line)
    except json.JSONDecodeError:
        continue
    fix = rec.get('fix')
    if fix is not None and fix != '':
        print('FIX_PRESENT: ' + repr(fix))
        sys.exit(1)
sys.exit(0)
" 2>/dev/null; then
            : # fix absent as expected
        else
            pass=false
            fail_reasons+=("fix field expected absent but was present in diagnostic output")
        fi
    elif [ -n "${expected_fix}" ]; then
        if ! echo "${diag_json_output}" | grep -q "\"fix\".*\"${expected_fix}\""; then
            pass=false
            fail_reasons+=("fix field expected \"${expected_fix}\" not found in diagnostic output")
        fi
    fi

    # ── Check expected_diag_field (optional structured field check) ──────────
    local diag_field diag_value
    diag_field="$(yaml_get "${yaml_file}" "expected_diag_field.field" 2>/dev/null || true)"
    diag_value="$(yaml_get "${yaml_file}" "expected_diag_field.value" 2>/dev/null || true)"
    if [ -n "${diag_field}" ] && [ -n "${diag_value}" ]; then
        # pos.line and pos.col are nested — handled by checking json path
        if ! echo "${diag_json_output}" | python3 -c "
import sys, json

field = '${diag_field}'
expected = '${diag_value}'
output = sys.stdin.read()
found = False
for line in output.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        rec = json.loads(line)
    except json.JSONDecodeError:
        continue
    # Handle nested keys like pos.line
    parts = field.split('.')
    val = rec
    for p in parts:
        if isinstance(val, dict):
            val = val.get(p)
        else:
            val = None
            break
    if val is not None and str(val) == expected:
        found = True
        break
if not found:
    sys.exit(1)
sys.exit(0)
" 2>/dev/null; then
            pass=false
            fail_reasons+=("diag field '${diag_field}' expected '${diag_value}' not found")
        fi
    fi

    # ── Check expected_diag_has_pos ──────────────────────────────────────────
    if yaml_bool "${yaml_file}" "expected_diag_has_pos" 2>/dev/null; then
        if ! echo "${diag_json_output}" | python3 -c "
import sys, json
output = sys.stdin.read()
for line in output.splitlines():
    line = line.strip()
    if not line:
        continue
    try:
        rec = json.loads(line)
    except json.JSONDecodeError:
        continue
    pos = rec.get('pos', {})
    if 'line' in pos and 'col' in pos:
        sys.exit(0)
sys.exit(1)
" 2>/dev/null; then
            pass=false
            fail_reasons+=("diagnostic missing pos.line and pos.col fields")
        fi
    fi

    # ── Report ───────────────────────────────────────────────────────────────
    if $pass; then
        echo -e "${GREEN}PASS${RESET} ${test_id}: ${description}"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${RESET} ${test_id}: ${description}"
        for reason in "${fail_reasons[@]}"; do
            echo "      ${reason}"
        done
        FAIL=$((FAIL + 1))
        FAILURES+=("${test_id}")
    fi
}

# ── Discover and run all test files ─────────────────────────────────────────

echo "tkc conformance suite"
echo "Binary: ${TKC}"
if [ -n "${SERIES:-}" ]; then
    echo "Series filter: ${SERIES}"
fi
echo "------------------------------------------------------------"

# Sort by id for deterministic output
while IFS= read -r -d '' yaml_file; do
    run_test "${yaml_file}"
done < <(find "${TEST_DIR}" -name '*.yaml' -print0 | sort -z)

echo "------------------------------------------------------------"
echo -e "Results: ${GREEN}${PASS} passed${RESET}, ${RED}${FAIL} failed${RESET}, ${SKIP} skipped"

if [ ${FAIL} -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for id in "${FAILURES[@]}"; do
        echo "  ${id}"
    done
    exit 1
fi

exit 0
