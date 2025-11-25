#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB_PATH="${1:-${ROOT_DIR}/storage/dist_imaging.sqlite}"
RAW_DIR="${2:-${ROOT_DIR}/storage/raw_frames}"

echo "[cleanup] Database: ${DB_PATH}"
echo "[cleanup] Raw frames dir: ${RAW_DIR}"

if [[ -f "${DB_PATH}" ]]; then
    rm -f "${DB_PATH}"
    echo "[cleanup] Removed ${DB_PATH}"
else
    echo "[cleanup] No database file found, skipping"
fi

if [[ -d "${RAW_DIR}" ]]; then
    find "${RAW_DIR}" -mindepth 1 -delete
    echo "[cleanup] Emptied ${RAW_DIR}"
else
    echo "[cleanup] Raw frames directory not found, skipping"
fi
