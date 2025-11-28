#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BIN_DIR="${BIN_DIR:-${BUILD_DIR}/bin}"
ENV_PATH="${DIST_ENV_PATH:-${ROOT_DIR}/.env}"
GENERATE_ONCE=false
ENABLE_ANNOTATED=false

usage() {
    cat <<'EOF'
Usage: ./scripts/run_all.sh [--once] [--annotated]

  --once        Run the image generator for a single pass through the dataset.
  --annotated   Ask the feature extractor to emit annotated frames (and store
                them in the logger's annotated output directory).
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --once)
            GENERATE_ONCE=true
            shift
            ;;
        --annotated)
            ENABLE_ANNOTATED=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[run_all] Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

for exe in image_generator feature_extractor data_logger; do
    if [[ ! -x "${BIN_DIR}/${exe}" ]]; then
        echo "[run_all] Missing executable ${BIN_DIR}/${exe}. Run scripts/build.sh first." >&2
        exit 1
    fi
done

if [[ ! -f "${ENV_PATH}" ]]; then
    echo "[run_all] Expected environment file at ${ENV_PATH}" >&2
    exit 1
fi

export DIST_ENV_PATH="${ENV_PATH}"

echo "[run_all] Launching processes (Ctrl+C to stop)"

declare -a PIDS=()
declare -a FE_ARGS=()
declare -a GEN_ARGS=()

if [[ "${ENABLE_ANNOTATED}" == true ]]; then
    FE_ARGS+=(--annotated)
fi
if [[ "${GENERATE_ONCE}" == true ]]; then
    GEN_ARGS+=(--once)
fi

cleanup() {
    for pid in "${PIDS[@]:-}"; do
        if kill -0 "${pid}" >/dev/null 2>&1; then
            kill "${pid}" 2>/dev/null || true
        fi
    done
}

trap cleanup EXIT INT TERM

("${BIN_DIR}/data_logger") &
PIDS+=($!)
if [[ ${#FE_ARGS[@]} -gt 0 ]]; then
    ("${BIN_DIR}/feature_extractor" "${FE_ARGS[@]}") &
else
    ("${BIN_DIR}/feature_extractor") &
fi
PIDS+=($!)
if [[ "${GENERATE_ONCE}" == true ]]; then
    if [[ ${#GEN_ARGS[@]} -gt 0 ]]; then
        ("${BIN_DIR}/image_generator" "${GEN_ARGS[@]}") &
    else
        ("${BIN_DIR}/image_generator") &
    fi
    GENERATOR_PID=$!
    wait "${GENERATOR_PID}"
    gen_status=$?
    echo "[run_all] Generator completed (--once). Stopping remaining processes."
    cleanup
    trap - EXIT
    exit "${gen_status}"
fi

if [[ ${#GEN_ARGS[@]} -gt 0 ]]; then
    ("${BIN_DIR}/image_generator" "${GEN_ARGS[@]}") &
else
    ("${BIN_DIR}/image_generator") &
fi
PIDS+=($!)

for pid in "${PIDS[@]}"; do
    wait "${pid}"
done
cleanup
