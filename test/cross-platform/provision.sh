#!/usr/bin/env bash
# provision.sh — Lightsail provisioning for toke cross-platform testing.
#
# Spins up Lightsail instances across multiple OSes, runs collect_results.sh
# on each, downloads the result JSON, and tears down the instances.
#
# Prerequisites:
#   - AWS CLI v2 configured (aws configure) with Lightsail permissions
#   - An SSH key pair registered in Lightsail (see KEY_PAIR_NAME below)
#   - The matching private key at SSH_KEY_PATH
#
# Usage:
#   ./provision.sh create     — spin up all instances
#   ./provision.sh test       — run tests on all running instances
#   ./provision.sh teardown   — delete all instances (with confirmation)
#   ./provision.sh all        — create + test + teardown
#
# Story 70.1.1

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────────────

REGION="us-west-2"
AZ="${REGION}a"
TAG_KEY="project"
TAG_VALUE="toke-crossplatform"
PREFIX="toke-xplat"

# SSH key pair — must be registered in Lightsail and the private key must
# exist locally.  Create one with:
#   aws lightsail create-key-pair --key-pair-name toke-xplat --region us-west-2
KEY_PAIR_NAME="${TOKE_XPLAT_KEY_NAME:-toke-xplat}"
SSH_KEY_PATH="${TOKE_XPLAT_KEY_PATH:-${HOME}/.ssh/toke-xplat.pem}"

# GitHub repo to clone on each instance (HTTPS — no deploy key needed).
REPO_URL="https://github.com/karwalski/toke.git"

# Where to stash downloaded result files.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"

# Poll interval (seconds) while waiting for instances to become running.
POLL_INTERVAL=10

# SSH connection timeout and retry settings.
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=15"

# ── Instance matrix ─────────────────────────────────────────────────────────
#
# Format: NAME|BLUEPRINT|BUNDLE|SSH_USER
#
# Blueprint and bundle IDs are approximate — verify with:
#   aws lightsail get-blueprints --region us-west-2
#   aws lightsail get-bundles    --region us-west-2

INSTANCES=(
    "ubuntu|ubuntu_24_04|nano_3_2|ubuntu"
    "debian|debian_12|nano_3_2|admin"
    "amazon-linux|amazon_linux_2023|nano_3_2|ec2-user"
    "centos|centos_stream_9|nano_3_2|centos"
    "freebsd|freebsd_14|nano_3_2|ec2-user"
    "windows|windows_server_2022|small_3_2|Administrator"
)

# ── Helpers ──────────────────────────────────────────────────────────────────

log()  { printf "[%s] %s\n" "$(date +%H:%M:%S)" "$*"; }
err()  { log "ERROR: $*" >&2; }
die()  { err "$@"; exit 1; }

instance_name() {
    local short="$1"
    echo "${PREFIX}-${short}"
}

parse_entry() {
    # Split a matrix entry on '|' into global vars.
    IFS='|' read -r INST_SHORT INST_BLUEPRINT INST_BUNDLE INST_USER <<< "$1"
    INST_NAME="$(instance_name "${INST_SHORT}")"
}

get_instance_ip() {
    local name="$1"
    aws lightsail get-instance \
        --instance-name "${name}" \
        --region "${REGION}" \
        --query 'instance.publicIpAddress' \
        --output text 2>/dev/null || echo ""
}

get_instance_state() {
    local name="$1"
    aws lightsail get-instance \
        --instance-name "${name}" \
        --region "${REGION}" \
        --query 'instance.state.name' \
        --output text 2>/dev/null || echo "unknown"
}

wait_for_running() {
    local name="$1"
    local max_wait=300  # 5 minutes
    local elapsed=0
    log "Waiting for ${name} to reach 'running' state..."
    while [ "${elapsed}" -lt "${max_wait}" ]; do
        local state
        state="$(get_instance_state "${name}")"
        if [ "${state}" = "running" ]; then
            log "${name} is running."
            return 0
        fi
        sleep "${POLL_INTERVAL}"
        elapsed=$((elapsed + POLL_INTERVAL))
    done
    err "${name} did not reach running state within ${max_wait}s (last state: $(get_instance_state "${name}"))"
    return 1
}

wait_for_ssh() {
    local ip="$1" user="$2"
    local max_wait=180
    local elapsed=0
    log "Waiting for SSH on ${user}@${ip}..."
    while [ "${elapsed}" -lt "${max_wait}" ]; do
        if ssh ${SSH_OPTS} -i "${SSH_KEY_PATH}" "${user}@${ip}" "echo ok" >/dev/null 2>&1; then
            log "SSH ready on ${ip}."
            return 0
        fi
        sleep "${POLL_INTERVAL}"
        elapsed=$((elapsed + POLL_INTERVAL))
    done
    err "SSH not available on ${ip} after ${max_wait}s."
    return 1
}

# ── Build-dependency installation (per OS) ───────────────────────────────────
#
# toke needs: C compiler (cc), make, git, zlib-dev, LLVM/clang (for llvm.c).
# collect_results.sh also needs: sed, uname, date, wc — usually pre-installed.

install_deps_script() {
    local blueprint="$1"
    case "${blueprint}" in
        ubuntu_24_04)
            cat <<'SCRIPT'
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential clang llvm-dev libz-dev git
SCRIPT
            ;;
        debian_12)
            cat <<'SCRIPT'
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
sudo apt-get install -y -qq build-essential clang llvm-dev libz-dev git
SCRIPT
            ;;
        amazon_linux_2023)
            cat <<'SCRIPT'
sudo dnf install -y gcc clang llvm-devel zlib-devel make git
SCRIPT
            ;;
        centos_stream_9)
            cat <<'SCRIPT'
sudo dnf install -y gcc clang llvm-devel zlib-devel make git
SCRIPT
            ;;
        freebsd_14)
            cat <<'SCRIPT'
sudo pkg install -y llvm17 gmake git
# FreeBSD cc is clang by default; zlib is in base.
SCRIPT
            ;;
        windows_server_2022)
            # Windows testing is manual / via WSL — emit a placeholder.
            cat <<'SCRIPT'
echo "Windows: manual setup required (install MSYS2/WSL, clang, make, git, zlib)"
SCRIPT
            ;;
        *)
            echo 'echo "Unknown OS — install clang, make, git, zlib-dev manually."'
            ;;
    esac
}

# ── create ───────────────────────────────────────────────────────────────────

do_create() {
    log "Creating Lightsail instances in ${REGION}..."

    # Pre-flight checks.
    command -v aws >/dev/null 2>&1 || die "aws CLI not found. Install and configure it first."
    [ -f "${SSH_KEY_PATH}" ] || die "SSH key not found at ${SSH_KEY_PATH}. Set TOKE_XPLAT_KEY_PATH."

    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"

        # Skip if the instance already exists.
        if aws lightsail get-instance --instance-name "${INST_NAME}" --region "${REGION}" >/dev/null 2>&1; then
            log "${INST_NAME} already exists — skipping create."
            continue
        fi

        log "Creating ${INST_NAME}  (blueprint=${INST_BLUEPRINT}, bundle=${INST_BUNDLE})..."
        aws lightsail create-instances \
            --instance-names "${INST_NAME}" \
            --availability-zone "${AZ}" \
            --blueprint-id "${INST_BLUEPRINT}" \
            --bundle-id "${INST_BUNDLE}" \
            --key-pair-name "${KEY_PAIR_NAME}" \
            --tags "key=${TAG_KEY},value=${TAG_VALUE}" \
            --region "${REGION}" \
            --output text >/dev/null

        log "  -> ${INST_NAME} creation request sent."
    done

    # Wait for all instances to become running.
    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"
        wait_for_running "${INST_NAME}" || true
    done

    # Print summary table.
    log ""
    log "Instance summary:"
    printf "  %-25s %-18s %-10s\n" "NAME" "IP" "STATE"
    printf "  %-25s %-18s %-10s\n" "----" "--" "-----"
    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"
        local ip state
        ip="$(get_instance_ip "${INST_NAME}")"
        state="$(get_instance_state "${INST_NAME}")"
        printf "  %-25s %-18s %-10s\n" "${INST_NAME}" "${ip:-pending}" "${state}"
    done
}

# ── test ─────────────────────────────────────────────────────────────────────

do_test() {
    log "Running tests on all instances..."

    mkdir -p "${RESULTS_DIR}"

    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"

        # Skip Windows — no SSH-based automation.
        if [ "${INST_BLUEPRINT}" = "windows_server_2022" ]; then
            log "SKIP ${INST_NAME}: Windows requires manual testing."
            continue
        fi

        local ip
        ip="$(get_instance_ip "${INST_NAME}")"
        if [ -z "${ip}" ] || [ "${ip}" = "None" ]; then
            err "${INST_NAME}: no public IP — is the instance running?"
            continue
        fi

        log "── ${INST_NAME} (${ip}) ──"

        # Wait for SSH to become available.
        if ! wait_for_ssh "${ip}" "${INST_USER}"; then
            err "${INST_NAME}: SSH not reachable — skipping."
            continue
        fi

        # Determine make command (FreeBSD uses gmake).
        local make_cmd="make"
        if [ "${INST_BLUEPRINT}" = "freebsd_14" ]; then
            make_cmd="gmake"
        fi

        # Install dependencies.
        log "  Installing build dependencies..."
        local dep_script
        dep_script="$(install_deps_script "${INST_BLUEPRINT}")"
        ssh ${SSH_OPTS} -i "${SSH_KEY_PATH}" "${INST_USER}@${ip}" "${dep_script}" 2>&1 | \
            sed 's/^/    [deps] /' || {
            err "${INST_NAME}: dependency installation failed."
            continue
        }

        # Clone toke repo.
        log "  Cloning toke repo..."
        ssh ${SSH_OPTS} -i "${SSH_KEY_PATH}" "${INST_USER}@${ip}" \
            "rm -rf ~/toke && git clone --depth 1 ${REPO_URL} ~/toke" 2>&1 | \
            sed 's/^/    [git] /' || {
            err "${INST_NAME}: git clone failed."
            continue
        }

        # Run collect_results.sh.
        log "  Running collect_results.sh..."
        local result_file="${RESULTS_DIR}/${INST_SHORT}.json"
        ssh ${SSH_OPTS} -i "${SSH_KEY_PATH}" "${INST_USER}@${ip}" \
            "cd ~/toke && MAKE=${make_cmd} sh test/cross-platform/collect_results.sh" \
            > "${result_file}" 2>/dev/null || {
            err "${INST_NAME}: collect_results.sh failed."
            # Still try to grab whatever was produced.
        }

        if [ -s "${result_file}" ]; then
            log "  Result saved to ${result_file}"
        else
            err "  ${result_file} is empty — check instance logs."
        fi
    done

    log ""
    log "All results saved to ${RESULTS_DIR}/"
    ls -la "${RESULTS_DIR}/"*.json 2>/dev/null || log "(no result files found)"
}

# ── teardown ─────────────────────────────────────────────────────────────────

do_teardown() {
    log "Teardown will DELETE these Lightsail instances:"
    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"
        echo "  - ${INST_NAME}"
    done

    # Safety confirmation (skip if TOKE_XPLAT_YES=1).
    if [ "${TOKE_XPLAT_YES:-}" != "1" ]; then
        printf "\nType 'yes' to confirm deletion: "
        read -r confirm
        if [ "${confirm}" != "yes" ]; then
            log "Teardown cancelled."
            exit 0
        fi
    fi

    for entry in "${INSTANCES[@]}"; do
        parse_entry "${entry}"
        log "Deleting ${INST_NAME}..."
        if aws lightsail delete-instance \
            --instance-name "${INST_NAME}" \
            --region "${REGION}" \
            --output text >/dev/null 2>&1; then
            log "  -> ${INST_NAME} deleted."
        else
            err "  -> ${INST_NAME}: delete failed (may not exist)."
        fi
    done

    log "Teardown complete."
}

# ── Main ─────────────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  create     Spin up all Lightsail instances
  test       Run tests on all running instances
  teardown   Delete all instances (with confirmation)
  all        create + test + teardown

Environment variables:
  TOKE_XPLAT_KEY_NAME   Lightsail key pair name  (default: toke-xplat)
  TOKE_XPLAT_KEY_PATH   Path to SSH private key  (default: ~/.ssh/toke-xplat.pem)
  TOKE_XPLAT_YES=1      Skip teardown confirmation prompt
EOF
}

case "${1:-}" in
    create)
        do_create
        ;;
    test)
        do_test
        ;;
    teardown)
        do_teardown
        ;;
    all)
        do_create
        do_test
        do_teardown
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage
        exit 1
        ;;
esac
