#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    cat <<EOF
Usage: build.sh [preset] [options]

  preset    CMake preset name (default: auto-detected)

Options:
  --test      Run ctest after build
  --coverage  Run coverage.sh after --test
  --open      Open the coverage report in browser after generation (requires --coverage)
  --clean     Remove build directory before configuring
  --help, -h  Show this help message

Examples:
  build.sh
  build.sh --test
  build.sh linux-clang-coverage --test --coverage
EOF
}

PRESET=""
RUN_TEST=0
DO_CLEAN=0
DO_COVERAGE=0
DO_OPEN=0
USER_SET_PRESET=0

for arg in "$@"; do
    case "$arg" in
        --test)     RUN_TEST=1 ;;
        --coverage) DO_COVERAGE=1 ;;
        --open)     DO_OPEN=1 ;;
        --clean)    DO_CLEAN=1 ;;
        --help|-h)  usage; exit 0 ;;
        --*)        echo "unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)          PRESET="$arg"; USER_SET_PRESET=1 ;;
    esac
done

if [[ "$DO_COVERAGE" -eq 1 && "$RUN_TEST" -eq 0 ]]; then
    echo "error: --coverage requires --test" >&2
    exit 1
fi

if [[ -z "$PRESET" ]]; then
    case "$(uname -s)" in
        Darwin)
            if [[ "$DO_COVERAGE" -eq 1 ]]; then
                PRESET="macos-appleclang-coverage"
            else
                PRESET="macos-appleclang-debug"
            fi
            ;;
        Linux)
            if [[ "$DO_COVERAGE" -eq 1 ]]; then
                PRESET="linux-clang-coverage"
            else
                PRESET="linux-clang-debug"
            fi
            ;;
        *)
            echo "error: cannot auto-detect preset on this platform, pass one explicitly" >&2
            exit 1
            ;;
    esac
    echo "preset: $PRESET (auto-detected)"
else
    echo "preset: $PRESET"
fi

if [[ "$DO_COVERAGE" -eq 1 && "$PRESET" != *"coverage"* ]]; then
    echo "error: --coverage requires a coverage-enabled preset, got '$PRESET'" >&2
    if [[ "$USER_SET_PRESET" -eq 1 ]]; then
        echo "hint: use a preset such as linux-clang-coverage, linux-gcc-coverage, or macos-appleclang-coverage" >&2
    fi
    exit 1
fi

BUILD_DIR="${ROOT_DIR}/build/${PRESET}"

if [[ "$DO_CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "clean: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

if [[ "$RUN_TEST" -eq 1 ]]; then
    if [[ "$PRESET" == *"coverage"* && -f "${BUILD_DIR}/qppjs-build-meta.json" ]]; then
        COVERAGE_BACKEND="$(python3 - <<'PY' "${BUILD_DIR}/qppjs-build-meta.json"
import json
import sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    data = json.load(f)
print(data.get('coverage_backend', ''))
PY
)"
        if [[ "$COVERAGE_BACKEND" == "llvm-cov" ]]; then
            LLVM_PROFILE_FILE="${BUILD_DIR}/default.profraw" ctest --preset "$PRESET"
        else
            ctest --preset "$PRESET"
        fi
    else
        ctest --preset "$PRESET"
    fi
fi

if [[ "$DO_COVERAGE" -eq 1 ]]; then
    COVERAGE_ARGS=("$BUILD_DIR")
    [[ "$DO_OPEN" -eq 1 ]] && COVERAGE_ARGS+=("--open")
    "${SCRIPT_DIR}/coverage.sh" "${COVERAGE_ARGS[@]}"
fi
