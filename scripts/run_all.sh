#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BIN_DIR="${BIN_DIR:-${BUILD_DIR}/bin}"
ENV_PATH="${DIST_ENV_PATH:-${ROOT_DIR}/.env}"

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
("${BIN_DIR}/feature_extractor") &
PIDS+=($!)
("${BIN_DIR}/image_generator") &
PIDS+=($!)

wait -n "${PIDS[@]}"
cleanup

