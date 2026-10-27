#!/usr/bin/env bash
# Local MSVC/Wine build. Usage: tools/build-local.sh [Configuration] [MSBuild arguments...]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$ROOT/tools/build_local.py" "$@"
