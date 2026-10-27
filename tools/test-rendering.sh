#!/usr/bin/env bash
# Serial rendering regressions and retained consolidated evidence.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/tools/test-rendering.py" "$@"
