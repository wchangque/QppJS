#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"
DO_CLEAN=0
DO_QUIET=0

usage() {
    cat <<EOF
Usage: run_ut.sh [--clean] [--quiet] [--help]

Options:
  --clean     Remove build directory before building
  --quiet     Only write failed test or leak output to build/debug/run_ut_failures.txt
  --help, -h  Show this help message
EOF
}

for arg in "$@"; do
    case "$arg" in
        --clean)    DO_CLEAN=1 ;;
        --quiet)    DO_QUIET=1 ;;
        --help|-h)  usage; exit 0 ;;
        --*)        echo "error: unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)          echo "error: run_ut.sh does not accept positional arguments" >&2; echo; usage >&2; exit 1 ;;
    esac
done

if [[ "$DO_CLEAN" -eq 1 ]]; then
    "${SCRIPT_DIR}/clean.sh"
fi

if [[ "$DO_QUIET" -eq 1 ]]; then
    BUILD_LOG="${ROOT_DIR}/build/run_ut_build.log"
    mkdir -p "${ROOT_DIR}/build"
    if ! "${SCRIPT_DIR}/build_debug.sh" > "$BUILD_LOG" 2>&1; then
        echo "build failed. Build log written to: $BUILD_LOG" >&2
        exit 1
    fi
    rm -f "$BUILD_LOG"
else
    "${SCRIPT_DIR}/build_debug.sh"
fi

CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure -E '^qppjs_cli_')
if [[ -f "${BUILD_DIR}/qppjs-build-meta.json" ]]; then
    IS_MULTI_CONFIG="$(python3 - <<'PY' "${BUILD_DIR}/qppjs-build-meta.json"
import json
import sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    data = json.load(f)
print('true' if data.get('is_multi_config', False) else 'false')
PY
)"
    if [[ "$IS_MULTI_CONFIG" == "true" ]]; then
        CTEST_ARGS+=(-C Debug)
    fi
fi

if [[ "$(uname -s)" == "Darwin" ]]; then
    export ASAN_OPTIONS="${ASAN_OPTIONS:-}detect_leaks=1"
    SUPP_FILE="${ROOT_DIR}/lsan_suppressions.txt"
    if [[ -f "$SUPP_FILE" ]]; then
        export LSAN_OPTIONS="${LSAN_OPTIONS:-}suppressions=${SUPP_FILE}"
    fi
fi

if [[ "$DO_QUIET" -eq 1 ]]; then
    REPORT_FILE="${BUILD_DIR}/run_ut_failures.txt"
    RAW_LOG="${BUILD_DIR}/run_ut_raw.log"
    : > "$REPORT_FILE"
    if ctest "${CTEST_ARGS[@]}" > "$RAW_LOG" 2>&1; then
        rm -f "$RAW_LOG" "$REPORT_FILE"
        exit 0
    fi
    python3 - <<'PY' "$RAW_LOG" "$REPORT_FILE"
import re
import sys

raw_log, report_file = sys.argv[1], sys.argv[2]
with open(raw_log, 'r', encoding='utf-8', errors='replace') as f:
    text = f.read()

blocks = []
current = []
current_failed = False
start_re = re.compile(r'^\s*Start\s+\d+:\s+(.+)$')
end_re = re.compile(r'^\d+/\d+\s+Test\s+#\d+:\s+.+\*\*\*Failed')

for line in text.splitlines():
    if start_re.match(line):
        if current and current_failed:
            blocks.append('\n'.join(current))
        current = [line]
        current_failed = False
        continue
    if current:
        current.append(line)
        if end_re.match(line) or 'LeakSanitizer: detected memory leaks' in line:
            current_failed = True
if current and current_failed:
    blocks.append('\n'.join(current))

failed_summary = []
in_summary = False
for line in text.splitlines():
    if re.match(r'^\d+% tests passed, \d+ tests failed out of \d+', line):
        in_summary = True
    if in_summary:
        failed_summary.append(line)

with open(report_file, 'w', encoding='utf-8') as f:
    f.write('QppJS run_ut failed tests / leak report\n')
    f.write('========================================\n\n')
    if blocks:
        f.write('\n\n'.join(blocks))
        f.write('\n')
    else:
        f.write('No per-test failure block found. Full ctest failure summary follows.\n')
    if failed_summary:
        f.write('\nSummary\n-------\n')
        f.write('\n'.join(failed_summary))
        f.write('\n')
PY
    rm -f "$RAW_LOG"
    echo "UT failed. Failure and leak details written to: $REPORT_FILE" >&2
    exit 1
fi

ctest "${CTEST_ARGS[@]}"
