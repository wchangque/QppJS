#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    cat <<EOF
Usage: coverage.sh <build-dir> [--open] [--help]

  build-dir  CMake build directory with qppjs-build-meta.json

Options:
  --open      Open the HTML report in browser after generation
  --help, -h  Show this help message

Examples:
  coverage.sh build/linux-gcc-coverage
  coverage.sh build/linux-clang-coverage --open
EOF
}

BUILD_DIR=""
DO_OPEN=0

for arg in "$@"; do
    case "$arg" in
        --open)    DO_OPEN=1 ;;
        --help|-h) usage; exit 0 ;;
        --*)       echo "error: unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)         BUILD_DIR="$arg" ;;
    esac
done

if [[ -z "$BUILD_DIR" ]]; then
    echo "error: build directory is required" >&2
    echo
    usage >&2
    exit 1
fi

case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="${ROOT_DIR}/${BUILD_DIR}" ;;
esac

META_FILE="${BUILD_DIR}/qppjs-build-meta.json"
if [[ ! -f "$META_FILE" ]]; then
    echo "error: build metadata not found: $META_FILE" >&2
    echo "hint: run cmake configure/build first" >&2
    exit 1
fi

meta_value() {
    local key="$1"
    python3 - <<'PY' "$META_FILE" "$key"
import json
import sys
path, key = sys.argv[1], sys.argv[2]
with open(path, 'r', encoding='utf-8') as f:
    data = json.load(f)
value = data.get(key, "")
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
PY
}

COVERAGE_ENABLED="$(meta_value coverage_enabled)"
COVERAGE_BACKEND="$(meta_value coverage_backend)"
PLATFORM="$(meta_value platform)"
TEST_BINARY="$(meta_value test_binary)"

if [[ "$COVERAGE_ENABLED" != "true" ]]; then
    echo "error: coverage is not enabled for build directory: $BUILD_DIR" >&2
    exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: build directory not found: $BUILD_DIR" >&2
    exit 1
fi

for tool in lcov genhtml; do
    if ! command -v "$tool" &>/dev/null; then
        echo "error: '$tool' not found" >&2
        echo "hint: install lcov" >&2
        exit 1
    fi
done

RAW_INFO="${BUILD_DIR}/coverage_raw.info"
FILTERED_INFO="${BUILD_DIR}/coverage.info"
REPORT_DIR="${BUILD_DIR}/coverage"
GCOV_TOOL=""
WRAPPER=""
LLVM_PROFILE_PATTERN="${BUILD_DIR}/default-*.profraw"

cleanup() {
    if [[ -n "$WRAPPER" ]]; then
        rm -f "$WRAPPER"
    fi
}
trap cleanup EXIT

make_llvm_cov_wrapper() {
    local llvm_cov_bin="$1"
    WRAPPER="$(mktemp /tmp/llvm-gcov-XXXXXX)"
    cat > "$WRAPPER" <<EOF
#!/usr/bin/env bash
exec "${llvm_cov_bin}" gcov "\$@"
EOF
    chmod +x "$WRAPPER"
    GCOV_TOOL="$WRAPPER"
}

case "$COVERAGE_BACKEND" in
    gcov)
        if [[ -z "$(find "$BUILD_DIR" -name "*.gcno" 2>/dev/null | head -1)" ]]; then
            echo "error: no .gcno files found in $BUILD_DIR" >&2
            exit 1
        fi
        if [[ -z "$(find "$BUILD_DIR" -name "*.gcda" 2>/dev/null | head -1)" ]]; then
            echo "error: no .gcda files found in $BUILD_DIR" >&2
            exit 1
        fi
        GCOV_TOOL="$(command -v gcov)"
        ;;
    llvm-cov)
        PROFRAW_FILES=( ${LLVM_PROFILE_PATTERN} )
        if [[ ${#PROFRAW_FILES[@]} -eq 0 || ! -f "${PROFRAW_FILES[0]}" ]]; then
            echo "error: no llvm profile data found matching: $LLVM_PROFILE_PATTERN" >&2
            echo "hint: run tests with LLVM_PROFILE_FILE=${BUILD_DIR}/default-%p.profraw" >&2
            exit 1
        fi
        if [[ -z "$TEST_BINARY" || ! -x "$TEST_BINARY" ]]; then
            echo "error: test binary not found: $TEST_BINARY" >&2
            exit 1
        fi
        LLVM_COV_BIN="$(command -v llvm-cov 2>/dev/null || echo "")"
        LLVM_PROFDATA_BIN="$(command -v llvm-profdata 2>/dev/null || echo "")"
        if [[ -z "$LLVM_COV_BIN" || -z "$LLVM_PROFDATA_BIN" ]]; then
            echo "error: llvm-cov/llvm-profdata not found" >&2
            exit 1
        fi
        echo "merging ${#PROFRAW_FILES[@]} profraw file(s)..."
        "$LLVM_PROFDATA_BIN" merge -sparse "${PROFRAW_FILES[@]}" -o "${BUILD_DIR}/default.profdata"
        "$LLVM_COV_BIN" export "$TEST_BINARY" -instr-profile="${BUILD_DIR}/default.profdata" -format=lcov > "$RAW_INFO"
        ;;
    *)
        echo "error: unsupported coverage backend '$COVERAGE_BACKEND' on platform '$PLATFORM'" >&2
        exit 1
        ;;
esac

if [[ "$COVERAGE_BACKEND" == "gcov" ]]; then
    echo "collecting coverage data..."
    lcov \
        --gcov-tool "$GCOV_TOOL" \
        --capture \
        --directory "$BUILD_DIR" \
        --output-file "$RAW_INFO" \
        --ignore-errors mismatch,empty,inconsistent,unsupported,format,gcov \
        --rc branch_coverage=1 \
        --rc derive_function_end_line=0 \
        --rc geninfo_unexecuted_blocks=1
fi

echo "filtering coverage data..."
lcov \
    ${GCOV_TOOL:+--gcov-tool "$GCOV_TOOL"} \
    --remove "$RAW_INFO" \
    '*/tests/*' \
    '*/_deps/*' \
    '*/usr/*' \
    '*/opt/homebrew/*' \
    --output-file "$FILTERED_INFO" \
    --ignore-errors unused,unused,unused,inconsistent,unsupported,format \
    --rc branch_coverage=1 \
    --rc derive_function_end_line=0 \
    --rc geninfo_unexecuted_blocks=1

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
