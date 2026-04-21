#!/usr/bin/env bash
set -euo pipefail

# Usage: build.sh [preset] [--test] [--clean]
#   preset   CMake configure preset name (default: auto-detected)
#   --test   run ctest after build
#   --clean  remove build directory before configuring

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    cat <<EOF
Usage: build.sh [preset] [options]

  preset    CMake configure preset name (default: auto-detected)

Options:
  --test      Run ctest after build
  --coverage  Generate HTML coverage report after --test (requires --test)
  --open      Open the coverage report in browser after generation (requires --coverage)
  --clean     Remove build directory before configuring
  --help, -h  Show this help message

Available presets:
  macos-appleclang-debug      macOS Apple Clang, Debug + ASan
  macos-appleclang-release    macOS Apple Clang, Release
  macos-appleclang-coverage   macOS Apple Clang, Coverage
  macos-llvmclang-debug       macOS LLVM Clang, Debug + ASan
  macos-llvmclang-release     macOS LLVM Clang, Release
  macos-llvmclang-coverage    macOS LLVM Clang, Coverage
  linux-gcc-debug             Linux GCC, Debug + ASan
  linux-gcc-release           Linux GCC, Release
  linux-gcc-coverage          Linux GCC, Coverage
  linux-clang-debug           Linux Clang, Debug + ASan
  linux-clang-release         Linux Clang, Release
  linux-clang-coverage        Linux Clang, Coverage
  windows-msvc-debug          Windows MSVC, Debug
  windows-msvc-release        Windows MSVC, Release

Examples:
  build.sh                                           # auto-detect preset, build only
  build.sh --test                                    # auto-detect preset, build + test
  build.sh --clean --test                            # clean, build + test
  build.sh linux-gcc-release --test                  # specific preset, build + test
  build.sh macos-llvmclang-coverage --test --coverage --open  # build + test + coverage report
EOF
}

# ── 参数解析 ──────────────────────────────────────────────────────
PRESET=""
RUN_TEST=0
DO_CLEAN=0
DO_COVERAGE=0
DO_OPEN=0

for arg in "$@"; do
    case "$arg" in
        --test)     RUN_TEST=1 ;;
        --coverage) DO_COVERAGE=1 ;;
        --open)     DO_OPEN=1 ;;
        --clean)    DO_CLEAN=1 ;;
        --help|-h)  usage; exit 0 ;;
        --*)        echo "unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)          PRESET="$arg" ;;
    esac
done

if [[ "$DO_COVERAGE" -eq 1 && "$RUN_TEST" -eq 0 ]]; then
    echo "error: --coverage requires --test" >&2
    exit 1
fi

# --coverage 时把 *-debug / *-release preset 自动换成对应的 *-coverage variant
# 只对以 -debug 或 -release 结尾的 preset 做替换，已是 *-coverage 的不处理
# 替换后验证 coverage preset 在 CMakePresets.json 中存在，否则报错
if [[ "$DO_COVERAGE" -eq 1 && -n "$PRESET" ]]; then
    if [[ "$PRESET" == *-debug || "$PRESET" == *-release ]]; then
        COVERAGE_PRESET="${PRESET%-debug}"
        COVERAGE_PRESET="${COVERAGE_PRESET%-release}"
        COVERAGE_PRESET="${COVERAGE_PRESET}-coverage"
        if ! grep -q "\"name\": \"${COVERAGE_PRESET}\"" "${ROOT_DIR}/CMakePresets.json"; then
            echo "error: --coverage: no coverage preset found for '$PRESET' (looked for '$COVERAGE_PRESET')" >&2
            exit 1
        fi
        echo "note: --coverage: switching preset $PRESET -> $COVERAGE_PRESET"
        PRESET="$COVERAGE_PRESET"
    fi
fi

# ── 自动检测 preset ───────────────────────────────────────────────
if [[ -z "$PRESET" ]]; then
    case "$(uname -s)" in
        Darwin)
            if [[ "$DO_COVERAGE" -eq 1 ]]; then
                if [[ -x "/opt/homebrew/opt/llvm/bin/clang++" ]]; then
                    PRESET="macos-llvmclang-coverage"
                else
                    PRESET="macos-appleclang-coverage"
                fi
            else
                PRESET="macos-appleclang-debug"
            fi
            ;;
        Linux)
            if command -v clang++ &>/dev/null; then
                if [[ "$DO_COVERAGE" -eq 1 ]]; then
                    PRESET="linux-clang-coverage"
                else
                    PRESET="linux-clang-debug"
                fi
            else
                if [[ "$DO_COVERAGE" -eq 1 ]]; then
                    PRESET="linux-gcc-coverage"
                else
                    PRESET="linux-gcc-debug"
                fi
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

BUILD_DIR="${ROOT_DIR}/build/${PRESET}"

# ── clean ─────────────────────────────────────────────────────────
if [[ "$DO_CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
    echo "clean: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ── configure ────────────────────────────────────────────────────
cd "$ROOT_DIR"
cmake --preset "$PRESET"

# ── build ─────────────────────────────────────────────────────────
cmake --build --preset "$PRESET"

# ── test ──────────────────────────────────────────────────────────
if [[ "$RUN_TEST" -eq 1 ]]; then
    ctest --preset "$PRESET"
fi

# ── coverage ──────────────────────────────────────────────────────
if [[ "$DO_COVERAGE" -eq 1 ]]; then
    COVERAGE_ARGS=("$PRESET")
    [[ "$DO_OPEN" -eq 1 ]] && COVERAGE_ARGS+=("--open")
    "${SCRIPT_DIR}/coverage.sh" "${COVERAGE_ARGS[@]}"
fi
