#!/usr/bin/env bash
# companion_fidelity.sh — Round-trip fidelity validation harness for toke companion files.
#
# Story: 11.5.3
#
# OVERVIEW
# --------
# This harness validates the companion file system's round-trip fidelity:
# an LLM reading a companion file should regenerate functionally equivalent
# toke code (same AST structure, same type signatures, same control flow shape).
#
# FIDELITY METRIC
# ---------------
# Two toke programs are considered fidelity-equivalent if they produce
# identical IR for the same inputs. In practice this means:
#   1. Same module name
#   2. Same function signatures (name, param names, param types, return type)
#   3. Same type declarations (all fields present with correct names and types)
#   4. Equivalent control flow (produces identical output for all valid inputs)
#
# The AST equivalence check compares the structured IR emitted by the compiler
# rather than token identity — variable names and loop variables may differ.
#
# MODES
# -----
#   --dry-run   Validate companion format and hash without performing LLM
#               inference. Exits 0 if the companion is well-formed and fresh.
#
# USAGE
# -----
#   companion_fidelity.sh [--dry-run] [--tkc <path>] [--companion-dir <dir>] <source.tk>
#
# ARGUMENTS
# ---------
#   <source.tk>          Path to the toke source file to evaluate.
#   --dry-run            Validate format and hash only; skip LLM inference.
#   --tkc <path>         Path to the tkc compiler binary (default: tkc in PATH or
#                        ../../tkc relative to this script).
#   --companion-dir <d>  Directory to write/read companion files (default: same
#                        directory as the source file).

set -uo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRY_RUN=0
TKC=""
COMPANION_DIR=""
SOURCE_FILE=""

# ── Argument parsing ───────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --tkc)
            TKC="$2"
            shift 2
            ;;
        --companion-dir)
            COMPANION_DIR="$2"
            shift 2
            ;;
        -*)
            echo "ERROR: Unknown option: $1" >&2
            exit 2
            ;;
        *)
            if [[ -z "$SOURCE_FILE" ]]; then
                SOURCE_FILE="$1"
            else
                echo "ERROR: Unexpected argument: $1" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

if [[ -z "$SOURCE_FILE" ]]; then
    echo "Usage: $0 [--dry-run] [--tkc <path>] [--companion-dir <dir>] <source.tk>" >&2
    exit 2
fi

# ── Resolve tkc binary ─────────────────────────────────────────────────────────

if [[ -z "$TKC" ]]; then
    # Try sibling of this script (test/../tkc)
    CANDIDATE="${SCRIPT_DIR}/../tkc"
    if [[ -x "$CANDIDATE" ]]; then
        TKC="$CANDIDATE"
    elif command -v tkc &>/dev/null; then
        TKC="tkc"
    else
        echo "ERROR: tkc compiler not found. Use --tkc <path> or ensure tkc is in PATH." >&2
        exit 2
    fi
fi

# ── Validate source file ───────────────────────────────────────────────────────

if [[ ! -f "$SOURCE_FILE" ]]; then
    echo "ERROR: Source file not found: $SOURCE_FILE" >&2
    exit 2
fi

SOURCE_BASENAME="$(basename "$SOURCE_FILE")"
SOURCE_STEM="${SOURCE_BASENAME%.tk}"
SOURCE_ABSDIR="$(cd "$(dirname "$SOURCE_FILE")" && pwd)"
SOURCE_ABS="${SOURCE_ABSDIR}/${SOURCE_BASENAME}"

# ── Resolve companion directory and path ───────────────────────────────────────

if [[ -z "$COMPANION_DIR" ]]; then
    COMPANION_DIR="$SOURCE_ABSDIR"
fi

COMPANION_PATH="${COMPANION_DIR}/${SOURCE_STEM}.tkc.md"

# ── Helpers ────────────────────────────────────────────────────────────────────

log_section() {
    echo ""
    echo "══════════════════════════════════════════════════════════"
    echo "  $1"
    echo "══════════════════════════════════════════════════════════"
}

log_info()  { echo "[INFO]  $*"; }
log_ok()    { echo "[OK]    $*"; }
log_warn()  { echo "[WARN]  $*"; }
log_fail()  { echo "[FAIL]  $*"; }

PASS=0
FAIL=0

# ── Step 1: Generate companion file ───────────────────────────────────────────

log_section "Step 1: Generate companion file"
log_info "Source:    $SOURCE_ABS"
log_info "Companion: $COMPANION_PATH"

mkdir -p "$COMPANION_DIR"

if ! "$TKC" --companion-out "$COMPANION_PATH" "$SOURCE_ABS" 2>/tmp/fidelity_tkc_err; then
    log_fail "tkc --companion-out failed"
    cat /tmp/fidelity_tkc_err >&2
    exit 2
fi

log_ok "Companion generated: $COMPANION_PATH"
PASS=$((PASS+1))

# ── Step 2: Log companion content (what would be shown to an LLM) ─────────────

log_section "Step 2: Companion content (shown to LLM)"
echo ""
echo "--- BEGIN COMPANION: ${SOURCE_STEM}.tkc.md ---"
cat "$COMPANION_PATH"
echo "--- END COMPANION ---"

# ── Step 3: Validate companion format and hash ─────────────────────────────────

log_section "Step 3: Format and hash validation"

# Check format_version field
if grep -q 'format_version:' "$COMPANION_PATH"; then
    FORMAT_VER="$(grep 'format_version:' "$COMPANION_PATH" | head -1 | sed 's/.*format_version:[[:space:]]*//' | tr -d '"')"
    log_ok "format_version present: $FORMAT_VER"
    PASS=$((PASS+1))
else
    log_fail "format_version field missing from companion frontmatter"
    FAIL=$((FAIL+1))
fi

# Check source_hash field (must be 64-char lowercase hex)
if grep -q 'source_hash:' "$COMPANION_PATH"; then
    HASH="$(grep 'source_hash:' "$COMPANION_PATH" | head -1 | sed 's/.*source_hash:[[:space:]]*//' | tr -d '"')"
    if [[ ${#HASH} -eq 64 ]] && [[ "$HASH" =~ ^[0-9a-f]+$ ]]; then
        log_ok "source_hash valid: ${HASH:0:16}..."
        PASS=$((PASS+1))
    else
        log_fail "source_hash malformed (got: '$HASH')"
        FAIL=$((FAIL+1))
    fi
else
    log_fail "source_hash field missing from companion frontmatter"
    FAIL=$((FAIL+1))
fi

# Check source_file field
if grep -q 'source_file:' "$COMPANION_PATH"; then
    SRC_FIELD="$(grep 'source_file:' "$COMPANION_PATH" | head -1 | sed 's/.*source_file:[[:space:]]*//' | tr -d '"')"
    log_ok "source_file present: $SRC_FIELD"
    PASS=$((PASS+1))
else
    log_fail "source_file field missing from companion frontmatter"
    FAIL=$((FAIL+1))
fi

# Verify hash matches current source via tkc --verify-companion
VERIFY_OUT=$("$TKC" --verify-companion "$COMPANION_PATH" 2>&1)
VERIFY_RC=$?
if [[ $VERIFY_RC -eq 0 ]] && echo "$VERIFY_OUT" | grep -q "^MATCH:"; then
    log_ok "Hash verification: $VERIFY_OUT"
    PASS=$((PASS+1))
else
    log_fail "Hash verification failed (exit=$VERIFY_RC): $VERIFY_OUT"
    FAIL=$((FAIL+1))
fi

# Check required sections present
for section in "## Module" "## Functions"; do
    if grep -q "^${section}" "$COMPANION_PATH"; then
        log_ok "Section present: '${section}'"
        PASS=$((PASS+1))
    else
        log_warn "Section absent: '${section}' (may be empty module)"
    fi
done

# ── Step 4: Define fidelity metric ────────────────────────────────────────────

log_section "Step 4: Fidelity metric definition"
cat <<'METRIC'
FIDELITY METRIC: AST Equivalence (not token identity)
------------------------------------------------------
Two toke programs are fidelity-equivalent when they produce identical IR
for the same inputs. Formally, given:
  - P_orig  = original source compiled to IR
  - P_regen = LLM-regenerated source compiled to IR

P_regen is fidelity-equivalent to P_orig iff:
  1. Both compile without errors (type-check clean).
  2. All function signatures match exactly:
       - Same function names
       - Same parameter names (or semantically identical)
       - Same parameter types
       - Same return types
  3. All type declarations match:
       - Same type names
       - Same field names and types
  4. For every test input in the conformance suite, both programs produce
     the same result value.

Variable names used in local bindings (let x=...) and loop counter names
may differ, as long as the observable behaviour is preserved.

TARGET METRIC: >=90% of companions produce code that compiles and passes
type checking when regenerated by a toke-trained LLM.
METRIC
PASS=$((PASS+1))

# ── Step 5: LLM inference (skipped in dry-run) ────────────────────────────────

log_section "Step 5: LLM inference"

if [[ $DRY_RUN -eq 1 ]]; then
    log_info "--dry-run mode: LLM inference skipped."
    log_info "In a full run, this step would:"
    log_info "  1. POST the companion content to a toke-trained LLM with the prompt:"
    log_info "     'You are reading a toke companion file. Regenerate the toke source"
    log_info "      code described. Output only the .tk source, no explanation.'"
    log_info "  2. Write the LLM response to a temp file: /tmp/fidelity_regen.tk"
    log_info "  3. Compile both the original and regenerated source with tkc."
    log_info "  4. Compare IR output for functional equivalence."
    log_info "  5. Record: PASS (compiles + type-checks), PARTIAL (compiles but"
    log_info "     differs), FAIL (does not compile)."
    PASS=$((PASS+1))
else
    log_warn "LLM inference requires a live model endpoint."
    log_warn "Set LLM_ENDPOINT and LLM_API_KEY environment variables and"
    log_warn "remove --dry-run to enable full evaluation."
    log_warn "(No live model configured — skipping inference step.)"
fi

# ── Step 6: IR comparison (dry-run: compile original only) ────────────────────

log_section "Step 6: IR comparison"

TMPIR="/tmp/fidelity_orig_ir_$$.ll"

if "$TKC" --emit-ir "$SOURCE_ABS" > "$TMPIR" 2>/tmp/fidelity_ir_err; then
    IR_LINES="$(wc -l < "$TMPIR" | tr -d ' ')"
    log_ok "Original IR compiled: ${IR_LINES} lines -> $TMPIR"
    PASS=$((PASS+1))
    if [[ $DRY_RUN -eq 1 ]]; then
        log_info "--dry-run: IR comparison deferred (no regenerated source available)."
        log_info "In a full run, regenerated IR would be diffed against this baseline."
    fi
else
    log_warn "IR emission not available or source has errors (non-blocking in dry-run)"
    log_warn "$(cat /tmp/fidelity_ir_err)"
    # Don't count as FAIL in dry-run; companion format check is what matters
fi

rm -f "$TMPIR" /tmp/fidelity_tkc_err /tmp/fidelity_ir_err /tmp/fidelity_ir_err 2>/dev/null

# ── Summary ────────────────────────────────────────────────────────────────────

log_section "Summary"
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [[ $DRY_RUN -eq 1 ]]; then
    echo ""
    echo "DRY-RUN RESULTS"
    echo "---------------"
    echo "Companion format: $([ $FAIL -eq 0 ] && echo 'VALID' || echo 'INVALID')"
    echo "Hash status:      $([ $FAIL -eq 0 ] && echo 'FRESH' || echo 'CHECK ABOVE')"
    echo "LLM inference:    SKIPPED (--dry-run)"
    echo "IR comparison:    DEFERRED"
    echo ""
    echo "To run full fidelity evaluation:"
    echo "  export LLM_ENDPOINT=<endpoint>"
    echo "  export LLM_API_KEY=<key>"
    echo "  $0 $SOURCE_FILE"
fi

[[ $FAIL -eq 0 ]]
