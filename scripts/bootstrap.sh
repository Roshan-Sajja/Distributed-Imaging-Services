#!/usr/bin/env bash
set -euo pipefail

# Convenience wrapper to (re)configure the CMake build directory.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
GENERATOR="${GENERATOR:-Ninja}"

echo "[bootstrap] Configuring build directory at ${BUILD_DIR}"
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "$@"
else
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" "$@"
fi

