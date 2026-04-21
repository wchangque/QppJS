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
  --test    Run ctest after build
  --clean   Remove build directory before configuring
  --help, -h  Show this help message

Available presets:
  macos-appleclang-debug     macOS Apple Clang, Debug + ASan
  macos-appleclang-release   macOS Apple Clang, Release
  linux-gcc-debug            Linux GCC, Debug + ASan
  linux-gcc-release          Linux GCC, Release
  linux-clang-debug          Linux Clang, Debug + ASan
  linux-clang-release        Linux Clang, Release
  windows-msvc-debug         Windows MSVC, Debug
  windows-msvc-release       Windows MSVC, Release

Examples:
  build.sh                              # auto-detect preset, build only
  build.sh --test                       # auto-detect preset, build + test
  build.sh --clean --test               # clean, build + test
  build.sh linux-gcc-release --test     # specific preset, build + test
EOF
}

# ── 参数解析 ──────────────────────────────────────────────────────
PRESET=""
RUN_TEST=0
DO_CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --test)   RUN_TEST=1 ;;
        --clean)  DO_CLEAN=1 ;;
        --help|-h) usage; exit 0 ;;
        --*)      echo "unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)        PRESET="$arg" ;;
    esac
done

# ── 自动检测 preset ───────────────────────────────────────────────
if [[ -z "$PRESET" ]]; then
    case "$(uname -s)" in
        Darwin)  PRESET="macos-appleclang-debug" ;;
        Linux)
            if command -v clang++ &>/dev/null; then
                PRESET="linux-clang-debug"
            else
                PRESET="linux-gcc-debug"
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
