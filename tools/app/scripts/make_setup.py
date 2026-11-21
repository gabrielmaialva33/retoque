#!/usr/bin/env python3
"""Build Retoque-Setup-<version>.exe: the retoque-setup stub followed by the Windows app archive, the release
SHA256SUMS and release.json, then a fixed trailer (layout in tools/app/crates/retoque-setup/src/lib.rs).

The given SHA256SUMS is embedded unchanged and must list the app archive with its real hash; release.json describes
the app archive and that embedded SHA256SUMS exactly as the app's update check would read them from GitHub
(Release::validate_kind(true) in payload_fetch.rs). The setup's own checksum is then merged into <out>/SHA256SUMS,
which is the file to publish (the embedded copy cannot list the setup that contains it). Stdlib only; nothing is
executed, uploaded or published."""
import argparse
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import struct
import tempfile
import zipfile

from make_app_release import regular_bytes, sums_parse

MAGIC = b"RTQSETUP1\0"
TRAILER = struct.Struct("<QQQ32s10s")
MAX_ZIP = 256 * 1024 * 1024  # payload_fetch::MAX_DOWNLOAD
MAX_SUMS = 64 * 1024  # payload_fetch::MAX_SUMS_BYTES
MAX_RELEASE = 64 * 1024  # app_release::MAX_SETUP_RELEASE
MAX_STUB = 16 * 1024 * 1024
ASSET_PREFIX = "https://github.com/gabrielmaialva33/retoque/releases/download/"
FIELDS = ("tag", "version", "published_at", "asset", "url", "size", "digest", "sums_url", "sums_size", "sums_digest")
HEX64 = re.compile(r"[0-9a-fA-F]{64}")


def normalize_version(raw):
    """payload_fetch::normalize_version: `retoque-vX.Y.Z`, `vX.Y.Z` or `X.Y.Z` (optional -prerelease) to `X.Y.Z`."""
    if not isinstance(raw, str):
        return None
    if raw.startswith("retoque-"):
        if not raw.startswith("retoque-v"):
            return None
        stripped = raw[len("retoque-v"):]
    else:
        stripped = raw[1:] if raw.startswith("v") else raw
    if not stripped or len(stripped) > 64:
        return None
    core, _, pre = stripped.partition("-")
    parts = core.split(".")
    if len(parts) != 3 or any(not re.fullmatch(r"0|[1-9][0-9]{0,7}", p) for p in parts):
        return None
    if "-" in stripped:
        if (not pre or len(pre) > 32 or not pre[0].isascii() or not pre[0].isalnum() or
                any(not re.fullmatch(r"[A-Za-z0-9-]+", seg) for seg in pre.split("."))):
            return None
        return f"{'.'.join(str(int(p)) for p in parts)}-{pre}"
    return ".".join(str(int(p)) for p in parts)


def asset_name(version):
    return f"retoque-app-{version}-windows-x64.zip"


def validate_release(release):
    """Release::validate_kind(true): official names and URLs for the app asset, bounded sizes, SHA-256 hex digests."""
    if not isinstance(release, dict) or set(release) != set(FIELDS):
        raise ValueError("release.json must have exactly the fields " + ", ".join(FIELDS))
    text = ("tag", "version", "published_at", "asset", "url", "digest", "sums_url")
    if (any(not isinstance(release[k], str) for k in text) or
            any(type(release[k]) is not int for k in ("size", "sums_size")) or
            not (release["sums_digest"] is None or isinstance(release["sums_digest"], str))):
        raise ValueError("release.json has a field of the wrong type")
    version = normalize_version(release["tag"])
    published = release["published_at"]
    if (version is None or release["version"] != version or release["asset"] != asset_name(version) or
            release["url"] != f"{ASSET_PREFIX}{release['tag']}/{release['asset']}" or
            release["sums_url"] != f"{ASSET_PREFIX}{release['tag']}/SHA256SUMS" or
            not 0 < release["size"] <= MAX_ZIP or not 0 < release["sums_size"] <= MAX_SUMS or
            not published or len(published) > 40 or not published.endswith("Z") or
            any(ord(c) < 32 or 0x7f <= ord(c) <= 0x9f for c in published) or
            not HEX64.fullmatch(release["digest"]) or
            (release["sums_digest"] is not None and not HEX64.fullmatch(release["sums_digest"]))):
        raise ValueError("release.json is not valid official app release metadata")
    return version


def release_for(tag, published_at, zip_bytes, sums_bytes):
    """The Release the app would build from the GitHub API for this tag's app archive and SHA256SUMS assets."""
    version = normalize_version(tag)
    if version is None:
        raise ValueError("invalid release tag")
    asset = asset_name(version)
    release = dict(tag=tag, version=version, published_at=published_at, asset=asset,
                   url=f"{ASSET_PREFIX}{tag}/{asset}", size=len(zip_bytes),
                   digest=hashlib.sha256(zip_bytes).hexdigest(), sums_url=f"{ASSET_PREFIX}{tag}/SHA256SUMS",
                   sums_size=len(sums_bytes), sums_digest=hashlib.sha256(sums_bytes).hexdigest())
    validate_release(release)
    return release


def trailer(zip_bytes, sums_bytes, release_bytes):
    return TRAILER.pack(len(zip_bytes), len(sums_bytes), len(release_bytes),
                        hashlib.sha256(zip_bytes).digest(), MAGIC)


def read_setup(data):
    """Split a setup file into (stub, zip, SHA256SUMS, release.json) with the checks the stub applies at run time."""
    if len(data) < TRAILER.size:
        raise ValueError("no setup trailer")
    zip_len, sums_len, release_len, digest, magic = TRAILER.unpack(data[-TRAILER.size:])
    if magic != MAGIC:
        raise ValueError("no setup trailer")
    if not (0 < zip_len <= MAX_ZIP and 0 < sums_len <= MAX_SUMS and 0 < release_len <= MAX_RELEASE):
        raise ValueError("setup part has an invalid size")
    start = len(data) - TRAILER.size - release_len - sums_len - zip_len
    if start <= 0:
        raise ValueError("setup is truncated")
    zip_bytes = data[start:start + zip_len]
    sums_bytes = data[start + zip_len:start + zip_len + sums_len]
    release_bytes = data[start + zip_len + sums_len:len(data) - TRAILER.size]
    if hashlib.sha256(zip_bytes).digest() != digest:
        raise ValueError("setup app archive does not match its SHA-256")
    return data[:start], zip_bytes, sums_bytes, release_bytes


def check_stub(stub):
    """An x64 Windows GUI executable that does not already carry a bundle."""
    if len(stub) < 64 or stub[:2] != b"MZ" or len(stub) > MAX_STUB:
        raise ValueError("stub is not a Windows executable")
    pe = struct.unpack_from("<I", stub, 60)[0]
    if (pe + 24 + 70 > len(stub) or stub[pe:pe + 6] != b"PE\0\0\x64\x86" or stub[pe + 24:pe + 26] != b"\x0b\x02" or
            struct.unpack_from("<H", stub, pe + 22)[0] & 0x2002 != 2):
        raise ValueError("stub is not a Windows x64 executable")
    if struct.unpack_from("<H", stub, pe + 24 + 68)[0] != 2:
        raise ValueError("stub is not a Windows GUI executable (build retoque-setup for x86_64-pc-windows-gnu)")
    if stub.endswith(MAGIC):
        raise ValueError("stub already carries a setup bundle")


# The first app version with the `--setup <dir>` mode.
FIRST_SETUP_VERSION = (0, 3, 3)


def version_tuple(version):
    match = re.match(r"(\d+)\.(\d+)\.(\d+)", version)
    if not match:
        raise ValueError(f"unreadable app version {version!r}")
    return tuple(int(part) for part in match.groups())


def check_app_zip(zip_bytes, version, allow_without_setup=False):
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as archive:
        try:
            info = archive.getinfo("retoque-app.exe")
        except KeyError:
            raise ValueError("app archive has no retoque-app.exe") from None
        if info.file_size > 512 * 1024 * 1024:
            raise ValueError("app executable is oversized")
        app = archive.read(info)
    if f"RETOQUE_APP_VERSION={version};".encode() not in app:
        raise ValueError("the app in the archive is not version " + version)
    # The app's `--setup <dir>` mode opens the install/update review; an older app (retoque-v0.3.2 and before)
    # would only open its window from the setup's temporary folder. Decided by the version marker checked above:
    # the literal "--setup" is not a reliable probe, because an optimized build compares that short argument with
    # integer loads and never stores the string whole (the v0.3.3 release build has no "--setup" bytes).
    if version_tuple(version) < FIRST_SETUP_VERSION and not allow_without_setup:
        raise ValueError("the app in the archive predates --setup (retoque-v0.3.3), so the setup could only open it "
                         "(--allow-app-without-setup packages it anyway, as a format check)")


def build(stub_path, zip_path, sums_path, out, release_path=None, tag=None, published_at=None,
          allow_without_setup=False):
    stub = regular_bytes(stub_path)
    check_stub(stub)
    zip_bytes = regular_bytes(zip_path)
    sums_bytes = regular_bytes(sums_path)
    if len(zip_bytes) > MAX_ZIP or len(sums_bytes) > MAX_SUMS:
        raise ValueError("app archive or SHA256SUMS exceeds the app's download limit")
    if (release_path is None) == (tag is None):
        raise ValueError("give either --release-json or --tag")
    if release_path is not None:
        release_bytes = regular_bytes(release_path)
        if len(release_bytes) > MAX_RELEASE:
            raise ValueError("release.json exceeds 64 KiB")
        try:
            release = json.loads(release_bytes)
        except ValueError:
            raise ValueError("release.json is not JSON") from None
    else:
        release = release_for(tag, published_at, zip_bytes, sums_bytes)
        release_bytes = json.dumps(release, indent=2).encode() + b"\n"
    version = validate_release(release)
    zip_hash = hashlib.sha256(zip_bytes).hexdigest()
    if zip_path.name != release["asset"]:
        raise ValueError(f"app archive must be named {release['asset']}")
    if release["size"] != len(zip_bytes) or release["digest"].lower() != zip_hash:
        raise ValueError("release.json does not describe this app archive (size or digest)")
    if (release["sums_size"] != len(sums_bytes) or
            release["sums_digest"] is not None and release["sums_digest"].lower() != hashlib.sha256(sums_bytes).hexdigest()):
        raise ValueError("release.json does not describe this SHA256SUMS (size or digest)")
    sums = sums_parse(sums_bytes)
    if sums.get(release["asset"]) != zip_hash:
        raise ValueError("SHA256SUMS does not list the app archive with its real hash")
    check_app_zip(zip_bytes, version, allow_without_setup)

    out.mkdir(parents=True, exist_ok=True)
    if any(p.is_symlink() for p in [out, *out.parents]):
        raise ValueError("linked output directory refused")
    name = f"Retoque-Setup-{version}.exe"
    out_sums = out / "SHA256SUMS"
    merged = dict(sums_parse(regular_bytes(out_sums))) if out_sums.exists() else {}
    for member, digest in sums.items():
        if merged.setdefault(member, digest) != digest:
            raise ValueError(f"{out_sums} lists {member} with another hash")
    with tempfile.TemporaryDirectory(prefix=".setup-package-", dir=out) as temp:
        dest = Path(temp) / name
        with open(dest, "wb") as f:
            for part in (stub, zip_bytes, sums_bytes, release_bytes, trailer(zip_bytes, sums_bytes, release_bytes)):
                f.write(part)
        # Read the result back the way the stub will.
        _, z, s, r = read_setup(dest.read_bytes())
        if (z, s, r) != (zip_bytes, sums_bytes, release_bytes):
            raise ValueError("written setup does not read back")
        merged[name] = hashlib.sha256(dest.read_bytes()).hexdigest()
        next_sums = Path(temp) / "SHA256SUMS"
        next_sums.write_text("".join(f"{digest}  {member}\n" for member, digest in sorted(merged.items())))
        dest.replace(out / name)
        next_sums.replace(out_sums)
    print(f"Setup: {out / name}\nSHA-256: {merged[name]}\nChecksums: {out_sums}")
    print("Embedded release.json:\n" + release_bytes.decode().rstrip())
    print("Would upload (never executed automatically):")
    print(shlex.join(["gh", "release", "upload", release["tag"], "--repo", "gabrielmaialva33/retoque", "--clobber",
                      str(out / name), str(out_sums)]))
    return out / name


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stub", type=Path, required=True,
                        help="target/x86_64-pc-windows-gnu/release/retoque-setup.exe")
    parser.add_argument("--app-zip", type=Path, required=True, help="retoque-app-<version>-windows-x64.zip")
    parser.add_argument("--sums", type=Path, required=True, help="the release SHA256SUMS listing the app archive")
    meta = parser.add_mutually_exclusive_group(required=True)
    meta.add_argument("--release-json", type=Path, help="app release metadata (payload_fetch::Release JSON)")
    meta.add_argument("--tag", help="build release.json for this tag (retoque-vX.Y.Z) from the archive and sums")
    parser.add_argument("--published-at", help="with --tag: publication time, UTC ending in Z (default: now)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--allow-app-without-setup", action="store_true",
                        help="package an app that has no --setup mode (only to check the format with old releases)")
    args = parser.parse_args()
    if args.published_at and not args.tag:
        parser.error("--published-at needs --tag")
    try:
        build(args.stub, args.app_zip, args.sums, args.out, args.release_json, args.tag,
              args.published_at or utc_now(), args.allow_app_without_setup)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"REFUSING: {error}\n")


if __name__ == "__main__":
    main()
