#!/usr/bin/env bash
# Package locally; never publish, upload, install or run the supplied executable.
# --setup-stub <retoque-setup.exe> also builds Retoque-Setup-<version>.exe (make_setup.py; docs/releases/APP-UPDATE.md).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/make_app_release.py" "$@"
