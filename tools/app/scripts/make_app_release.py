#!/usr/bin/env python3
"""Build a Windows app archive from an exe and a previously packaged payload archive."""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import struct
import tempfile
import zipfile

MAX_BYTES = 512 * 1024 * 1024
REQUIRED = {"OptiScaler.dll", "nvngx.dll_dlssnr.dll", "OptiScaler.ini",
            "Licenses/RenoDX_ATTRIBUTION.txt", "translations/README.md", "translations/optional/pt-BR.lang"}


def allowed(name):
    parts = name.split("/")
    if (not name or len(name) > 512 or len(parts) > 6 or
        any(not p or p in {".", ".."} or p.endswith((".", " ")) or
            any(ord(c) < 32 or c in '\\:<>"|?*' for c in p) or
            re.fullmatch(r"CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³]", p.split(".")[0].rstrip(), re.I)
            for p in parts)):
        return False
    lower = name.lower()
    leaf = parts[-1].lower()
    if (any(p.lower() in {"dlssnrmodels", "dlssg_sm86"} for p in parts) or
        leaf.startswith("nvngx_dlssnr") or "sm86" in leaf or "latewarp" in leaf or
        leaf in {"_nvngx.dll", "nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll", "lossless.dll",
                 "lsfg-vk.dll", "version.dll", "optiscaler.lang"} or
        lower.endswith((".pdb", ".lib", ".exp", ".ilk", ".cso", ".spv"))):
        return False
    return (name in REQUIRED or name == "SHA256SUMS" or
            len(parts) == 2 and parts[0] == "Licenses" and
            (leaf.endswith((".txt", ".md")) or leaf == "license") or
            len(parts) == 3 and parts[:2] == ["translations", "optional"] and name.endswith(".lang") or
            len(parts) >= 2 and parts[0] == "OptiScaler")


def sums_parse(data):
    result = {}
    for line in data.decode("utf-8").splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([a-fA-F0-9]{64}) [ *](.+)", line)
        if not match or match[2] in result:
            raise ValueError("malformed or duplicate SHA256SUMS entry")
        result[match[2]] = match[1].lower()
    return result


def regular_bytes(path):
    if any(p.is_symlink() for p in [path, *path.parents]) or not path.is_file() or not 0 < path.stat().st_size <= MAX_BYTES:
        raise ValueError("missing, linked, empty or oversized input")
    return path.read_bytes()


def build(exe, payload, out, raw_version):
    version = re.sub(r"^(?:retoque-v|v)", "", raw_version)
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,7})\.(?:0|[1-9][0-9]{0,7})\.(?:0|[1-9][0-9]{0,7})(?:-[A-Za-z0-9]+(?:[.-][A-Za-z0-9]+)*)?", version):
        raise ValueError("invalid release version")
    app = regular_bytes(exe)
    if len(app) < 64 or app[:2] != b"MZ":
        raise ValueError("app is not a Windows executable")
    pe = struct.unpack_from("<I", app, 60)[0]
    if (pe > 4070 or pe + 26 > len(app) or app[pe:pe+6] != b"PE\0\0\x64\x86" or
        app[pe+24:pe+26] != b"\x0b\x02" or struct.unpack_from("<H", app, pe+22)[0] & 0x2002 != 2):
        raise ValueError("app is not a Windows x64 executable")
    if ("RETOQUE_APP_VERSION=" + version + ";").encode() not in app:
        raise ValueError("app build version does not match release; bump workspace.package.version and rebuild")
    import io
    entries = {}
    seen = set()
    total = len(app)
    with zipfile.ZipFile(io.BytesIO(regular_bytes(payload))) as zf:
        if len(zf.infolist()) > 2048:
            raise ValueError("too many payload entries")
        for info in zf.infolist():
            name = info.filename
            kind = (info.external_attr >> 16) & 0o170000
            if (not allowed(name) or name.lower() in seen or info.flag_bits & 1 or
                kind not in {0, 0o100000} or info.is_dir() or info.file_size == 0):
                raise ValueError("unsafe, duplicate or forbidden payload member: " + name)
            seen.add(name.lower())
            total += info.file_size
            if total > MAX_BYTES:
                raise ValueError("app bundle exceeds size limit")
            entries[name] = zf.read(info)
    if not REQUIRED <= entries.keys() or "SHA256SUMS" not in entries:
        raise ValueError("payload lacks required files or internal checksums")
    sums = sums_parse(entries.pop("SHA256SUMS"))
    if sums.keys() != entries.keys() or any(hashlib.sha256(data).hexdigest() != sums[name] for name, data in entries.items()):
        raise ValueError("payload internal checksum mismatch")
    entries["retoque-app.exe"] = app
    entries["SHA256SUMS"] = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(entries.items())).encode()
    out.mkdir(parents=True, exist_ok=True)
    if any(p.is_symlink() for p in [out, *out.parents]):
        raise ValueError("linked output directory refused")
    name = f"retoque-app-{version}-windows-x64.zip"
    sums_path = out / "SHA256SUMS"
    old_sums = sums_parse(regular_bytes(sums_path)) if sums_path.exists() else {}
    # Preserve the payload archive's checksum when both assets share an output directory.
    with tempfile.TemporaryDirectory(prefix=".app-package-", dir=out) as temp:
        dest = Path(temp) / name
        with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for member, data in sorted(entries.items()):
                info = zipfile.ZipInfo(member, (2026, 1, 1, 0, 0, 0))
                info.external_attr = 0o100644 << 16
                zf.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
        if dest.stat().st_size > 256 * 1024 * 1024:
            raise ValueError("compressed app archive exceeds the download limit")
        old_sums[name] = hashlib.sha256(dest.read_bytes()).hexdigest()
        next_sums = Path(temp) / "SHA256SUMS"
        next_sums.write_text("".join(f"{digest}  {member}\n" for member, digest in sorted(old_sums.items())))
        dest.replace(out / name)
        next_sums.replace(sums_path)
    print(f"Packaged: {out / name}\nChecksums: {sums_path}\nWould run (never executed automatically):")
    print(shlex.join(["gh", "release", "create", f"retoque-v{version}", "--repo", "gabrielmaialva33/retoque", "--draft",
                      "--title", f"Retoque {version}", str(out / name), str(sums_path)]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--payload", type=Path, required=True, help="archive from make-payload-release.sh")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        build(args.exe, args.payload, args.out, args.version)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"REFUSING: {error}\n")


if __name__ == "__main__":
    main()
