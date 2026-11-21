#!/usr/bin/env python3
"""Synthetic, local-only setup packaging contract tests. No executable is run."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import make_app_release
import make_setup as setup

ZIP_NAME = "retoque-app-1.2.3-windows-x64.zip"


def pe(subsystem=2, tail=b""):
    exe = bytearray(256)
    exe[:2] = b"MZ"
    struct.pack_into("<I", exe, 60, 64)
    exe[64:70] = b"PE\0\0\x64\x86"
    struct.pack_into("<H", exe, 86, 2)
    exe[88:90] = b"\x0b\x02"
    struct.pack_into("<H", exe, 88 + 68, subsystem)
    return bytes(exe) + tail


class Setup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.stub = self.root / "retoque-setup.exe"
        self.stub.write_bytes(pe())
        self.zip = self.root / ZIP_NAME
        self.write_zip()
        self.sums = self.root / "SHA256SUMS"
        self.write_sums()
        self.out = self.root / "out"

    def write_zip(self, version="1.2.3", setup_mode=True):
        tail = f"RETOQUE_APP_VERSION={version};".encode() + (b"--setup" if setup_mode else b"--apply-app-update")
        with zipfile.ZipFile(self.zip, "w") as archive:
            archive.writestr("retoque-app.exe", pe(tail=tail))
            archive.writestr("OptiScaler.dll", b"synthetic")

    def write_sums(self, digest=None, extra=""):
        digest = digest or hashlib.sha256(self.zip.read_bytes()).hexdigest()
        self.sums.write_text(f"{'a' * 64}  retoque-payload-1.2.3-x64.zip\n{digest}  {ZIP_NAME}\n{extra}")

    def build(self, **kwargs):
        kwargs.setdefault("tag", None if "release_path" in kwargs else "retoque-v1.2.3")
        if kwargs["tag"]:
            kwargs.setdefault("published_at", "2026-11-20T21:05:09Z")
        with contextlib.redirect_stdout(io.StringIO()):
            return setup.build(self.stub, self.zip, self.sums, self.out, **kwargs)

    def release(self):
        return setup.release_for("retoque-v1.2.3", "2026-11-20T21:05:09Z", self.zip.read_bytes(), self.sums.read_bytes())

    def assertRefused(self, pattern, **kwargs):
        with self.assertRaisesRegex(ValueError, pattern):
            self.build(**kwargs)
        self.assertFalse((self.out / "Retoque-Setup-1.2.3.exe").exists())

    def test_setup_carries_the_bundle_and_is_listed_in_the_output_checksums(self):
        result = self.build()
        self.assertEqual(result, self.out / "Retoque-Setup-1.2.3.exe")
        data = result.read_bytes()
        stub, zip_bytes, sums, release = setup.read_setup(data)
        self.assertEqual((stub, zip_bytes, sums), (self.stub.read_bytes(), self.zip.read_bytes(), self.sums.read_bytes()))
        self.assertEqual(data[-10:], b"RTQSETUP1\0")
        self.assertEqual(len(data) - len(stub) - len(zip_bytes) - len(sums) - len(release), 66)
        meta = json.loads(release)
        self.assertEqual(meta, {
            "tag": "retoque-v1.2.3", "version": "1.2.3", "published_at": "2026-11-20T21:05:09Z", "asset": ZIP_NAME,
            "url": f"{setup.ASSET_PREFIX}retoque-v1.2.3/{ZIP_NAME}", "size": len(zip_bytes),
            "digest": hashlib.sha256(zip_bytes).hexdigest(), "sums_url": f"{setup.ASSET_PREFIX}retoque-v1.2.3/SHA256SUMS",
            "sums_size": len(sums), "sums_digest": hashlib.sha256(sums).hexdigest()})
        listed = make_app_release.sums_parse((self.out / "SHA256SUMS").read_bytes())
        self.assertEqual(listed[result.name], hashlib.sha256(data).hexdigest())
        self.assertEqual({k: v for k, v in listed.items() if k != result.name}, make_app_release.sums_parse(sums))

    def test_supplied_release_json_must_describe_the_archive_and_sums(self):
        good = self.release()
        path = self.root / "release.json"
        path.write_text(json.dumps(good))
        self.assertEqual(json.loads(setup.read_setup(self.build(release_path=path).read_bytes())[3]), good)
        (self.out / "Retoque-Setup-1.2.3.exe").unlink()
        for field, value, pattern in [("size", good["size"] + 1, "archive"), ("digest", "0" * 64, "archive"),
                                      ("sums_size", good["sums_size"] + 1, "SHA256SUMS"),
                                      ("sums_digest", "0" * 64, "SHA256SUMS"),
                                      ("version", "1.2.4", "not valid"), ("published_at", "yesterday", "not valid"),
                                      ("url", good["url"].replace("gabrielmaialva33", "evil"), "not valid"),
                                      ("asset", "retoque-payload-1.2.3-x64.zip", "not valid"),
                                      ("size", "12", "wrong type"), ("extra", 1, "exactly the fields")]:
            with self.subTest(field=field):
                bad = dict(good, **{field: value})
                path.write_text(json.dumps(bad))
                self.assertRefused(pattern, release_path=path)
        # A release.json without a sums digest is still accepted, as the app accepts it.
        path.write_text(json.dumps(dict(good, sums_digest=None)))
        self.build(release_path=path)

    def test_sums_must_list_the_archive_with_its_real_hash(self):
        self.write_sums(digest="0" * 64)
        self.assertRefused("SHA256SUMS does not list")
        self.sums.write_text(f"{'a' * 64}  retoque-payload-1.2.3-x64.zip\n")
        self.assertRefused("SHA256SUMS does not list")

    def test_stub_and_archive_are_checked(self):
        self.stub.write_bytes(pe(subsystem=3))
        self.assertRefused("GUI")
        self.stub.write_bytes(b"ELF")
        self.assertRefused("Windows")
        self.stub.write_bytes(pe() + b"RTQSETUP1\0")
        self.assertRefused("already")
        self.stub.write_bytes(pe())
        self.write_zip(version="1.2.4")
        self.write_sums()
        self.assertRefused("not version 1.2.3")
        # An app from before the setup existed would only open its window; refused unless asked for explicitly.
        self.write_zip(setup_mode=False)
        self.write_sums()
        self.assertRefused("predates --setup")
        self.build(allow_without_setup=True)
        (self.out / "Retoque-Setup-1.2.3.exe").unlink()
        renamed = self.root / "app.zip"
        self.write_zip()
        self.write_sums()
        self.zip.rename(renamed)
        self.zip = renamed
        self.assertRefused("must be named")

    def test_existing_output_checksums_are_merged_not_contradicted(self):
        self.out.mkdir()
        (self.out / "SHA256SUMS").write_text(f"{'b' * 64}  retoque-app-1.2.3-linux-x64.tar.gz\n")
        self.build()
        listed = make_app_release.sums_parse((self.out / "SHA256SUMS").read_bytes())
        self.assertEqual(set(listed), {"retoque-app-1.2.3-linux-x64.tar.gz", "retoque-payload-1.2.3-x64.zip", ZIP_NAME,
                                       "Retoque-Setup-1.2.3.exe"})
        (self.out / "SHA256SUMS").write_text(f"{'c' * 64}  {ZIP_NAME}\n")
        with self.assertRaisesRegex(ValueError, "another hash"):
            self.build()
        # Rewriting the given file in place (the release flow's single SHA256SUMS) keeps its lines.
        self.out = self.root
        self.build()
        self.assertIn("Retoque-Setup-1.2.3.exe", make_app_release.sums_parse(self.sums.read_bytes()))

    def test_versions_match_the_app(self):
        for raw, expected in [("retoque-v0.3.2", "0.3.2"), ("v1.10.0", "1.10.0"), ("1.2.3-rc.1", "1.2.3-rc.1"),
                              ("retoque-v1.0.0-beta-2", "1.0.0-beta-2"), ("retoque-0.3.2", None), ("01.2.3", None),
                              ("1.2", None), ("1.2.3.4", None), ("1.2.3-", None), ("1.2.3-a..b", None),
                              ("1.2.3-é", None), ("123456789.0.0", None)]:
            with self.subTest(raw=raw):
                self.assertEqual(setup.normalize_version(raw), expected)

    def test_trailer_reader_refuses_damage(self):
        data = self.build().read_bytes()
        for bad in [data[:-1], data[:-1] + b"X", data[66 + 300:], data[:300] + bytes([data[300] ^ 1]) + data[301:]]:
            with self.assertRaises(ValueError):
                setup.read_setup(bad)


class AppReleaseWithSetup(unittest.TestCase):
    def test_app_release_can_build_the_setup_too(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            exe = root / "app.exe"
            exe.write_bytes(pe(tail=b"RETOQUE_APP_VERSION=1.2.3;--setup"))
            stub = root / "retoque-setup.exe"
            stub.write_bytes(pe())
            payload = root / "payload.zip"
            entries = {name: b"synthetic payload file" for name in make_app_release.REQUIRED}
            with zipfile.ZipFile(payload, "w") as archive:
                for name, data in entries.items():
                    archive.writestr(name, data)
                archive.writestr("SHA256SUMS", "".join(
                    f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in entries.items()))
            out = root / "out"
            with contextlib.redirect_stdout(io.StringIO()) as log:
                make_app_release.build(exe, payload, out, "retoque-v1.2.3", stub, "2026-11-20T21:05:09Z")
            self.assertIn("Retoque-Setup-1.2.3.exe", log.getvalue())
            listed = make_app_release.sums_parse((out / "SHA256SUMS").read_bytes())
            self.assertEqual(set(listed), {ZIP_NAME, "Retoque-Setup-1.2.3.exe"})
            _, zip_bytes, sums, release = setup.read_setup((out / "Retoque-Setup-1.2.3.exe").read_bytes())
            self.assertEqual(zip_bytes, (out / ZIP_NAME).read_bytes())
            self.assertEqual(make_app_release.sums_parse(sums), {ZIP_NAME: listed[ZIP_NAME]})
            self.assertEqual(json.loads(release)["sums_size"], len(sums))


if __name__ == "__main__":
    unittest.main()
