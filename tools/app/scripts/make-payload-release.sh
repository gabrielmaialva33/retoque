#!/usr/bin/env bash
# Assemble retoque-payload-<version>-x64.zip and SHA256SUMS for gabrielmaialva33/retoque.
# Never executes `gh release create`, never publishes or pushes, and never packages
# NVIDIA models, _nvngx.dll, dlssg_for_sm86, Lossless Scaling files or build intermediates.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

REPO_ROOT="${REPO_ROOT:-$DEFAULT_REPO_ROOT}"
OUT_DIR="${OUT_DIR:-}"
RAW_VERSION="${VERSION:-0.3.0}"
DEST_DIR=""

usage() {
    cat >&2 <<'EOF'
usage: make-payload-release.sh [--out <dest-dir>] [--build-dir <x64-out>] [--repo-root <repo>] [--version <ver>] [<dest-dir> [version]]
EOF
    exit 1
}

POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--out|--output)
            [[ $# -ge 2 ]] || usage
            DEST_DIR="$2"
            shift 2
            ;;
        --build-dir|--x64-out)
            [[ $# -ge 2 ]] || usage
            OUT_DIR="$2"
            shift 2
            ;;
        --repo-root)
            [[ $# -ge 2 ]] || usage
            REPO_ROOT="$2"
            shift 2
            ;;
        -v|--version)
            [[ $# -ge 2 ]] || usage
            RAW_VERSION="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        --)
            shift
            POSITIONAL+=("$@")
            break
            ;;
        -*)
            echo "[-] REFUSING: unknown option: $1" >&2
            usage
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

is_version_like() {
    [[ "$1" =~ ^(retoque-v|v)?[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9][A-Za-z0-9.-]*)?$ ]]
}

if [[ ${#POSITIONAL[@]} -eq 1 ]]; then
    DEST_DIR="${DEST_DIR:-${POSITIONAL[0]}}"
elif [[ ${#POSITIONAL[@]} -eq 2 ]]; then
    if [[ -z "$OUT_DIR" && -d "${POSITIONAL[0]}" && ( -f "${POSITIONAL[0]}/OptiScaler.dll" || -f "${POSITIONAL[0]}/nvngx.dll_dlssnr.dll" ) ]]; then
        OUT_DIR="${POSITIONAL[0]}"
        DEST_DIR="${DEST_DIR:-${POSITIONAL[1]}}"
    elif is_version_like "${POSITIONAL[0]}" && ! is_version_like "${POSITIONAL[1]}"; then
        RAW_VERSION="${POSITIONAL[0]}"
        DEST_DIR="${DEST_DIR:-${POSITIONAL[1]}}"
    else
        DEST_DIR="${DEST_DIR:-${POSITIONAL[0]}}"
        RAW_VERSION="${POSITIONAL[1]}"
    fi
elif [[ ${#POSITIONAL[@]} -eq 3 ]]; then
    OUT_DIR="${OUT_DIR:-${POSITIONAL[0]}}"
    DEST_DIR="${DEST_DIR:-${POSITIONAL[1]}}"
    RAW_VERSION="${POSITIONAL[2]}"
elif [[ ${#POSITIONAL[@]} -gt 3 ]]; then
    usage
fi

[[ -n "$DEST_DIR" ]] || usage
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/x64/out}"

if ! is_version_like "$RAW_VERSION"; then
    echo "[-] REFUSING: version must match [retoque-v|v]X.Y.Z[-prerelease], got: $RAW_VERSION" >&2
    exit 1
fi

VERSION_NUM="${RAW_VERSION#retoque-v}"
VERSION_NUM="${VERSION_NUM#v}"
RELEASE_TAG="retoque-v${VERSION_NUM}"
ZIP_NAME="retoque-payload-${VERSION_NUM}-x64.zip"

require_regular_file() {
    local path="$1"
    local label="$2"
    if [[ -L "$path" || ! -f "$path" || ! -s "$path" ]]; then
        echo "[-] REFUSING: missing, empty or symlinked required file ($label): $path" >&2
        exit 1
    fi
}

require_regular_file "${OUT_DIR}/OptiScaler.dll" "OptiScaler.dll"
require_regular_file "${OUT_DIR}/nvngx.dll_dlssnr.dll" "nvngx.dll_dlssnr.dll"
require_regular_file "${REPO_ROOT}/OptiScaler.ini" "OptiScaler.ini"
require_regular_file "${REPO_ROOT}/Licenses/RenoDX_ATTRIBUTION.txt" "Licenses/RenoDX_ATTRIBUTION.txt"
require_regular_file "${REPO_ROOT}/retoque/translations/optional/pt-BR.lang" "translations/optional/pt-BR.lang"
require_regular_file "${REPO_ROOT}/retoque/translations/README.md" "translations/README.md"

# Reject forbidden proprietary/external files if any were placed in the build output directory.
FORBIDDEN_OUT="$(find "$OUT_DIR" \( -type f -o -type l \) \( \
    -iname 'nvngx_dlssnr*.dll' -o \
    -ipath '*/dlssnrmodels/*' -o \
    -iname '_nvngx.dll' -o \
    -iname 'nvngx_dlss.dll' -o \
    -iname 'nvngx_dlssd.dll' -o \
    -iname 'nvngx_dlssg.dll' -o \
    -iname '*sm86*' -o \
    -iname 'version.dll' -o \
    -iname 'lossless.dll' -o \
    -iname 'lsfg-vk.dll' -o \
    -iname '*.cso' -o \
    -iname '*.spv' -o \
    -iname '*latewarp*' \
\))"
if [[ -n "$FORBIDDEN_OUT" ]]; then
    echo "[-] REFUSING: forbidden file(s) found in build output directory:" >&2
    echo "$FORBIDDEN_OUT" >&2
    exit 1
fi

if ! grep -qaF "OptiScaler working as" "${OUT_DIR}/OptiScaler.dll"; then
    echo "[-] REFUSING: ${OUT_DIR}/OptiScaler.dll is missing the OptiScaler marker" >&2
    exit 1
fi

for exp in dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version; do
    if ! grep -qaF "$exp" "${OUT_DIR}/nvngx.dll_dlssnr.dll"; then
        echo "[-] REFUSING: stale forwarder ${OUT_DIR}/nvngx.dll_dlssnr.dll (missing export $exp)" >&2
        exit 1
    fi
done

# Verify that OptiScaler.dll and nvngx.dll_dlssnr.dll carry matching PE VERSIONINFO file versions.
python3 - "${OUT_DIR}/OptiScaler.dll" "${OUT_DIR}/nvngx.dll_dlssnr.dll" <<'PY'
from pathlib import Path
import struct
import sys

VS_KEY = "VS_VERSION_INFO\0".encode("utf-16le")

def u16(b: bytes, at: int) -> int | None:
    if at < 0 or at + 2 > len(b):
        return None
    return struct.unpack_from("<H", b, at)[0]

def u32(b: bytes, at: int) -> int | None:
    if at < 0 or at + 4 > len(b):
        return None
    return struct.unpack_from("<I", b, at)[0]

def entries(b: bytes, at: int):
    named = u16(b, at + 12)
    ids = u16(b, at + 14)
    if named is None or ids is None:
        return None
    count = named + ids
    if count > 128 or at + 16 + count * 8 > len(b):
        return None
    out = []
    for i in range(count):
        off = at + 16 + i * 8
        out.append((u32(b, off), u32(b, off + 4)))
    return out

def fixed_pair(b: bytes):
    length = u16(b, 0)
    val_len = u16(b, 2)
    typ = u16(b, 4)
    if length is None or length > len(b) or val_len != 52 or typ != 0:
        return None
    b = b[:length]
    if b[6:6 + len(VS_KEY)] != VS_KEY:
        return None
    at = (6 + len(VS_KEY) + 3) & ~3
    if at + 52 > len(b):
        return None
    if u32(b, at) != 0xFEEF04BD or u32(b, at + 4) != 0x00010000:
        return None
    ms = u32(b, at + 8)
    ls = u32(b, at + 12)
    return (ms, ls)

def read_pe_version(path: Path) -> str | None:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        return None
    pe = u32(data, 60)
    if pe is None or pe > 16 * 1024 * 1024 or pe + 24 > len(data):
        return None
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    sections = u16(data, pe + 6)
    opt_len = u16(data, pe + 20)
    if not sections or sections > 96 or opt_len is None or opt_len > 4096:
        return None
    opt_off = pe + 24
    if opt_off + opt_len + sections * 40 > len(data):
        return None
    magic = u16(data, opt_off)
    dd_off = 96 if magic == 0x10B else (112 if magic == 0x20B else None)
    if dd_off is None or dd_off + 24 > opt_len:
        return None
    if (u32(data, opt_off + dd_off - 4) or 0) < 3:
        return None
    rva = u32(data, opt_off + dd_off + 16)
    size = u32(data, opt_off + dd_off + 20)
    if not rva or not size or size < 16 or size > 4 * 1024 * 1024:
        return None
    sec_base = opt_off + opt_len
    resource = None
    for i in range(sections):
        s = sec_base + i * 40
        va = u32(data, s + 12)
        raw_size = u32(data, s + 16)
        raw_ptr = u32(data, s + 20)
        if va is None or raw_size is None or raw_ptr is None:
            return None
        if rva >= va and (rva - va) + size <= raw_size:
            if resource is not None:
                return None
            start = raw_ptr + (rva - va)
            if start + size > len(data):
                return None
            resource = data[start:start + size]
    if resource is None:
        return None
    first = None
    root_entries = entries(resource, 0)
    if root_entries is None:
        return None
    for kind, names in root_entries:
        if kind != 16:
            continue
        if not (names & 0x80000000):
            return None
        name_entries = entries(resource, names & 0x7FFFFFFF)
        if name_entries is None:
            return None
        for _, langs in name_entries:
            if not (langs & 0x80000000):
                return None
            lang_entries = entries(resource, langs & 0x7FFFFFFF)
            if lang_entries is None:
                return None
            for _, leaf in lang_entries:
                if leaf & 0x80000000:
                    return None
                data_rva = u32(resource, leaf)
                data_len = u32(resource, leaf + 4)
                if data_rva is None or data_len is None or data_rva < rva:
                    return None
                rel = data_rva - rva
                if rel + data_len > len(resource):
                    return None
                ver = fixed_pair(resource[rel:rel + data_len])
                if ver is None:
                    return None
                if first is None:
                    first = ver
                elif first != ver:
                    return None
    if first is None:
        return None
    ms, ls = first
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"

opti_path = Path(sys.argv[1])
fwd_path = Path(sys.argv[2])
opti_ver = read_pe_version(opti_path)
fwd_ver = read_pe_version(fwd_path)
if not opti_ver or not fwd_ver:
    print(
        f"[-] REFUSING: missing PE VERSIONINFO resource (OptiScaler.dll={opti_ver!r}, nvngx.dll_dlssnr.dll={fwd_ver!r})",
        file=sys.stderr,
    )
    sys.exit(1)
if opti_ver != fwd_ver:
    print(
        f"[-] REFUSING: OptiScaler.dll file version ({opti_ver}) does not match nvngx.dll_dlssnr.dll ({fwd_ver})",
        file=sys.stderr,
    )
    sys.exit(1)
print(f"[*] Verified matching PE file version: {opti_ver}")
PY

mkdir -p "$DEST_DIR"
DEST_DIR="$(cd "$DEST_DIR" && pwd)"
WORK_DIR="$(mktemp -d "${DEST_DIR}/.payload-stage-XXXXXX")"
STAGE_DIR="${WORK_DIR}/stage"
trap 'rm -rf -- "$WORK_DIR"' EXIT

mkdir -p "$STAGE_DIR/Licenses" "$STAGE_DIR/translations/optional"

cp -f "${OUT_DIR}/OptiScaler.dll" "$STAGE_DIR/OptiScaler.dll"
cp -f "${OUT_DIR}/nvngx.dll_dlssnr.dll" "$STAGE_DIR/nvngx.dll_dlssnr.dll"

sed -E \
    -e 's/^LogToFile=auto/LogToFile=true/' \
    -e 's/^LogLevel=auto/LogLevel=2/' \
    -e 's/^ShortcutKey=auto/ShortcutKey=0x24/' \
    "${REPO_ROOT}/OptiScaler.ini" > "$STAGE_DIR/OptiScaler.ini"

if grep -Eq '^[[:space:]]*Enabled[[:space:]]*=[[:space:]]*true([[:space:]]*[;#].*)?$' "$STAGE_DIR/OptiScaler.ini"; then
    echo "[-] REFUSING: packaged OptiScaler.ini has an Enabled=true key switched on by default" >&2
    exit 1
fi
if ! grep -Eq '^LogToFile=true$' "$STAGE_DIR/OptiScaler.ini" || ! grep -Eq '^LogLevel=2$' "$STAGE_DIR/OptiScaler.ini"; then
    echo "[-] REFUSING: packaged OptiScaler.ini is missing release logging defaults" >&2
    exit 1
fi

for lic in "${REPO_ROOT}/Licenses/"*; do
    if [[ -L "$lic" ]]; then
        echo "[-] REFUSING: symlink in Licenses/: $lic" >&2
        exit 1
    fi
    if [[ -f "$lic" ]]; then
        cp -f "$lic" "$STAGE_DIR/Licenses/"
    fi
done
require_regular_file "$STAGE_DIR/Licenses/RenoDX_ATTRIBUTION.txt" "staged Licenses/RenoDX_ATTRIBUTION.txt"

cp -f "${REPO_ROOT}/retoque/translations/optional/pt-BR.lang" "$STAGE_DIR/translations/optional/pt-BR.lang"
cp -f "${REPO_ROOT}/retoque/translations/README.md" "$STAGE_DIR/translations/README.md"

if [[ -d "${OUT_DIR}/OptiScaler" ]]; then
    if [[ -L "${OUT_DIR}/OptiScaler" ]]; then
        echo "[-] REFUSING: symlinked OptiScaler support directory" >&2
        exit 1
    fi
    cp -r "${OUT_DIR}/OptiScaler" "$STAGE_DIR/OptiScaler"
    find "$STAGE_DIR/OptiScaler" -type f \( -iname '*.pdb' -o -iname '*.lib' -o -iname '*.exp' -o -iname '*.ilk' -o -iname '*latewarp*' \) -delete
fi

if [[ -e "$STAGE_DIR/OptiScaler.lang" ]]; then
    echo "[-] REFUSING: an active localization dictionary must not ship beside the DLL" >&2
    exit 1
fi

# Guard against forbidden files (NVIDIA models, _nvngx.dll, dlssg_for_sm86, Lossless Scaling, build intermediates, links).
FORBIDDEN_STAGE="$(find "$STAGE_DIR" \( -type f -o -type l \) \( \
    -type l -o \
    -iname 'nvngx_dlssnr*.dll' -o \
    -ipath '*/dlssnrmodels/*' -o \
    -iname '_nvngx.dll' -o \
    -iname 'nvngx_dlss.dll' -o \
    -iname 'nvngx_dlssd.dll' -o \
    -iname 'nvngx_dlssg.dll' -o \
    -iname '*sm86*' -o \
    -iname 'version.dll' -o \
    -iname 'dlssg_sm86.ini' -o \
    -iname 'lossless.dll' -o \
    -iname 'lsfg-vk.dll' -o \
    -iname '*.cso' -o \
    -iname '*.spv' -o \
    -iname '*.pdb' -o \
    -iname '*.lib' -o \
    -iname '*.exp' -o \
    -iname '*.ilk' -o \
    -iname '*latewarp*' -o \
    -iname 'OptiScaler.lang' \
\))"
if [[ -n "$FORBIDDEN_STAGE" ]]; then
    echo "[-] REFUSING: forbidden file(s) found in stage directory:" >&2
    echo "$FORBIDDEN_STAGE" >&2
    exit 1
fi

# Write internal SHA256SUMS for archive members (sorted deterministically by relative path).
(
    cd "$STAGE_DIR"
    find . -type f ! -name 'SHA256SUMS' -printf '%P\n' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS
    sha256sum --check --status SHA256SUMS
)

ZIP_PATH="${DEST_DIR}/${ZIP_NAME}"
SUMS_PATH="${DEST_DIR}/SHA256SUMS"
rm -f "$ZIP_PATH" "$SUMS_PATH"

python3 - "$STAGE_DIR" "$ZIP_PATH" <<'PY'
from pathlib import Path
import sys
import zipfile

stage = Path(sys.argv[1])
out_zip = Path(sys.argv[2])
files = sorted(p for p in stage.rglob("*") if p.is_file())
with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for file_path in files:
        rel = file_path.relative_to(stage).as_posix()
        info = zipfile.ZipInfo(rel)
        info.date_time = (2026, 1, 1, 0, 0, 0)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        zf.writestr(info, file_path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
PY

(
    cd "$DEST_DIR"
    sha256sum "$ZIP_NAME" > SHA256SUMS
    sha256sum --check --status SHA256SUMS
)

echo "[ok] Packaged payload archive: $ZIP_PATH"
echo "[ok] Wrote checksums: $SUMS_PATH"
echo ""
echo "Would run (never executed automatically):"
echo "  gh release create ${RELEASE_TAG} --repo gabrielmaialva33/retoque --draft --prerelease --title \"Retoque ${RELEASE_TAG}\" --notes-file \"docs/releases/${RELEASE_TAG}.md\" \"${ZIP_PATH}\" \"${SUMS_PATH}\""
