#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -D CMAKE_BUILD_TYPE=Debug \
    -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -D QPPJS_BUILD_TESTS=ON \
    -D QPPJS_ENABLE_ASAN=ON
cmake --build "$BUILD_DIR"
