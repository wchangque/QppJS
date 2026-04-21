#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    cat <<EOF
Usage: coverage.sh <preset> [--open] [--help]

  preset    CMake configure preset name (must be a coverage variant)

Options:
  --open      Open the HTML report in browser after generation
  --help, -h  Show this help message

Coverage presets:
  macos-appleclang-coverage
  linux-gcc-coverage
  linux-clang-coverage

Examples:
  coverage.sh macos-appleclang-coverage
  coverage.sh macos-appleclang-coverage --open
EOF
}

# ── 参数解析 ──────────────────────────────────────────────────────
PRESET=""
DO_OPEN=0

for arg in "$@"; do
    case "$arg" in
        --open)    DO_OPEN=1 ;;
        --help|-h) usage; exit 0 ;;
        --*)       echo "error: unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)         PRESET="$arg" ;;
    esac
done

if [[ -z "$PRESET" ]]; then
    echo "error: preset is required" >&2
    echo
    usage >&2
    exit 1
fi

BUILD_DIR="${ROOT_DIR}/build/${PRESET}"

# ── 前置检查 ──────────────────────────────────────────────────────
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: build directory not found: $BUILD_DIR" >&2
    echo "hint: run 'build.sh ${PRESET} --test' first" >&2
    exit 1
fi

if [[ -z "$(find "$BUILD_DIR" -name "*.gcno" 2>/dev/null | head -1)" ]]; then
    echo "error: no .gcno files found in $BUILD_DIR" >&2
    echo "hint: make sure the preset has QPPJS_ENABLE_COVERAGE=ON and the project is built" >&2
    exit 1
fi

if [[ -z "$(find "$BUILD_DIR" -name "*.gcda" 2>/dev/null | head -1)" ]]; then
    echo "error: no .gcda files found in $BUILD_DIR" >&2
    echo "hint: run tests first with 'build.sh ${PRESET} --test'" >&2
    exit 1
fi

for tool in lcov genhtml; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: '$tool' not found" >&2
        echo "hint: brew install lcov" >&2
        exit 1
    fi
done

# ── gcov 工具选择 ─────────────────────────────────────────────────
GCOV_TOOL=""
WRAPPER=""

make_llvm_cov_wrapper() {
    local llvm_cov_bin="$1"
    WRAPPER="$(mktemp /tmp/llvm-gcov-XXXXXX)"
    cat > "$WRAPPER" <<EOF
#!/usr/bin/env bash
exec "${llvm_cov_bin}" gcov "\$@"
EOF
    chmod +x "$WRAPPER"
    # shellcheck disable=SC2064
    trap "rm -f '${WRAPPER}'" EXIT
    GCOV_TOOL="$WRAPPER"
}

case "$(uname -s)" in
    Darwin)
        if [[ "$PRESET" == *"llvmclang"* ]]; then
            HOMEBREW_LLVM_COV="/opt/homebrew/opt/llvm/bin/llvm-cov"
            if [[ -x "$HOMEBREW_LLVM_COV" ]]; then
                make_llvm_cov_wrapper "$HOMEBREW_LLVM_COV"
            else
                echo "error: Homebrew llvm-cov not found: $HOMEBREW_LLVM_COV" >&2
                echo "hint: brew install llvm" >&2
                exit 1
            fi
        else
            XCRUN_LLVM_COV="$(xcrun --find llvm-cov 2>/dev/null || echo "")"
            if [[ -n "$XCRUN_LLVM_COV" ]]; then
                make_llvm_cov_wrapper "$XCRUN_LLVM_COV"
            else
                echo "warning: xcrun llvm-cov not found, falling back to system gcov" >&2
                GCOV_TOOL="$(command -v gcov)"
            fi
        fi
        ;;
    Linux)
        if [[ "$PRESET" == *"gcc"* ]]; then
            GCOV_TOOL="$(command -v gcov)"
        else
            LLVM_COV_BIN="$(command -v llvm-cov 2>/dev/null || echo "")"
            if [[ -n "$LLVM_COV_BIN" ]]; then
                make_llvm_cov_wrapper "$LLVM_COV_BIN"
            else
                echo "warning: llvm-cov not found, falling back to system gcov" >&2
                GCOV_TOOL="$(command -v gcov)"
            fi
        fi
        ;;
    *)
        echo "error: unsupported platform: $(uname -s)" >&2
        exit 1
        ;;
esac

echo "gcov tool: $GCOV_TOOL"

# ── 收集覆盖率数据 ────────────────────────────────────────────────
RAW_INFO="${BUILD_DIR}/coverage_raw.info"
FILTERED_INFO="${BUILD_DIR}/coverage.info"
REPORT_DIR="${BUILD_DIR}/coverage"

echo "collecting coverage data..."
lcov \
    --gcov-tool "$GCOV_TOOL" \
    --capture \
    --directory "$BUILD_DIR" \
    --output-file "$RAW_INFO" \
    --ignore-errors mismatch,empty,inconsistent,unsupported,format \
    --rc branch_coverage=1 \
    --rc derive_function_end_line=0

echo "filtering coverage data..."
lcov \
    --gcov-tool "$GCOV_TOOL" \
    --remove "$RAW_INFO" \
    '*/tests/*' \
    '*/_deps/*' \
    '*/usr/*' \
    '*/opt/homebrew/*' \
    --output-file "$FILTERED_INFO" \
    --ignore-errors unused,inconsistent,unsupported,format \
    --rc branch_coverage=1 \
    --rc derive_function_end_line=0

echo "generating HTML report..."
genhtml \
    "$FILTERED_INFO" \
    --output-directory "$REPORT_DIR" \
    --branch-coverage \
    --title "QppJS Coverage" \
    --ignore-errors inconsistent,unsupported,corrupt,category \
    --rc branch_coverage=1 \
    --rc derive_function_end_line=0

echo ""
echo "report: ${REPORT_DIR}/index.html"

if [[ "$DO_OPEN" -eq 1 ]]; then
    case "$(uname -s)" in
        Darwin) open "${REPORT_DIR}/index.html" ;;
        Linux)  xdg-open "${REPORT_DIR}/index.html" ;;
    esac
fi
