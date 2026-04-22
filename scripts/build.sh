#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
error: scripts/build.sh 已废弃。
use one of:
  ./scripts/build_release.sh
  ./scripts/build_debug.sh
  ./scripts/build_test.sh
  ./scripts/coverage.sh
EOF
exit 1
