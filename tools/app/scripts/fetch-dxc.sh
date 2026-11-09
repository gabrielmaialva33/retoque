#!/usr/bin/env bash
# Kit-build helper only. The installed app uses the Rust downloader, with explicit consent.
set -euo pipefail
DEST="${1:?usage: fetch-dxc.sh <Windows kit folder>}"
URL='https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip'
SHA='9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4'
mkdir -p "$DEST"
STAGING="$(mktemp -d "$DEST/.dxc-fetch-XXXXXX")"
trap 'rm -rf -- "$STAGING"' EXIT
curl --fail --location --proto '=https' --proto-redir '=https' --max-redirs 2 --max-time 120 \
    --max-filesize 67108864 "$URL" -o "$STAGING/dxc.zip"
printf '%s  %s\n' "$SHA" "$STAGING/dxc.zip" | sha256sum --check --status
for name in dxcompiler.dll dxil.dll; do
    unzip -p "$STAGING/dxc.zip" "bin/x64/$name" > "$STAGING/$name"
    test -s "$STAGING/$name"
done
mkdir -p "$DEST/Licenses/DXC"
for name in LICENSE-LLVM.txt LICENSE-MS.txt LICENSE-MIT.txt ReleaseNotes.md; do
    unzip -p "$STAGING/dxc.zip" "$name" > "$DEST/Licenses/DXC/$name"
done
mv "$STAGING/dxcompiler.dll" "$STAGING/dxil.dll" "$DEST/"
echo '[ok] Official DXC v1.8.2505.1 libraries verified and staged.'
