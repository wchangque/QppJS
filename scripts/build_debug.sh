#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"

EXTRA_LINKER_FLAGS=""
if [ -z "${CC:-}" ] && [ -z "${CXX:-}" ]; then
    case "$(uname -s)" in
        Darwin)
            if command -v brew &>/dev/null; then
                LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
                if [ -n "$LLVM_PREFIX" ] && [ -x "${LLVM_PREFIX}/bin/clang" ]; then
                    export CC="${LLVM_PREFIX}/bin/clang"
                    export CXX="${LLVM_PREFIX}/bin/clang++"
                    EXTRA_LINKER_FLAGS="-L${LLVM_PREFIX}/lib/c++ -Wl,-rpath,${LLVM_PREFIX}/lib/c++"
                fi
            fi
            ;;
    esac
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    ${EXTRA_LINKER_FLAGS:+-D CMAKE_EXE_LINKER_FLAGS="${EXTRA_LINKER_FLAGS}"} \
    ${EXTRA_LINKER_FLAGS:+-D CMAKE_SHARED_LINKER_FLAGS="${EXTRA_LINKER_FLAGS}"} \
    -D CMAKE_BUILD_TYPE=Debug \
    -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -D QPPJS_BUILD_TESTS=ON \
    -D QPPJS_ENABLE_ASAN=ON
cmake --build "$BUILD_DIR"
