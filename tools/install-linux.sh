#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# In the zip the files are beside us; in a checkout use the usual build output.
SOURCE=$HERE
if [[ ! -f $SOURCE/OptiScaler.dll && -f $HERE/../OptiScaler.ini ]]; then SOURCE="$HERE/../x64/out"; fi
if (( $# == 0 )); then
    read -r -p "Game folder (beside the executable): " game
    game=${game#\"} game=${game%\"}
    read -r -p "Proxy DLL name [dxgi.dll]: " proxy
    read -r -p "Turn Neural Rendering on? [Y/n] " nr
    args=()
    case ${nr,,} in n|no) ;; *) args+=(--enable-nr) ;; esac
    set -- "${args[@]}" "$game" "${proxy:-dxgi.dll}"
fi
exec bash "$HERE/deploy-to-game.sh" --source-dir "$SOURCE" "$@"
