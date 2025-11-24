#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
TARGET="${TARGET:-all}"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "[build] Missing build directory (${BUILD_DIR}). Run scripts/bootstrap.sh first." >&2
    exit 1
fi

cmake --build "${BUILD_DIR}" --target "${TARGET}" "$@"

