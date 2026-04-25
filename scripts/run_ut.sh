#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/debug"

usage() {
    cat <<EOF
Usage: run_ut.sh [--help]

Options:
  --help, -h  Show this help message
EOF
}

for arg in "$@"; do
    case "$arg" in
        --help|-h) usage; exit 0 ;;
        --*)       echo "error: unknown option: $arg" >&2; echo; usage >&2; exit 1 ;;
        *)         echo "error: run_ut.sh does not accept positional arguments" >&2; echo; usage >&2; exit 1 ;;
    esac
done

"${SCRIPT_DIR}/build_debug.sh"

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
fi

ctest "${CTEST_ARGS[@]}"
