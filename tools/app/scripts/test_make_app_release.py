#!/usr/bin/env python3
"""Synthetic, local-only package contract tests. No executable is run."""
import contextlib
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

import make_app_release as package


class AppPackage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.exe = self.root / "app.exe"
        exe = bytearray(128)
        exe[:2] = b"MZ"
        struct.pack_into("<I", exe, 60, 64)
        exe[64:70] = b"PE\0\0\x64\x86"
        struct.pack_into("<H", exe, 86, 2)
        exe[88:90] = b"\x0b\x02"
        exe.extend(b"RETOQUE_APP_VERSION=1.2.3;")
        self.exe.write_bytes(exe)
        self.payload = self.root / "payload.zip"
        self.out = self.root / "out"
        self.write_payload()

    def write_payload(self, extra=None):
        entries = {name: b"synthetic payload file" for name in package.REQUIRED}
        if extra:
            entries[extra] = b"forbidden"
        sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in entries.items())
        with zipfile.ZipFile(self.payload, "w") as archive:
            for name, data in entries.items():
                archive.writestr(name, data)
            archive.writestr("SHA256SUMS", sums)

    def build(self, version="v1.2.3"):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            package.build(self.exe, self.payload, self.out, version)
        return output.getvalue()

    def test_bundle_and_merged_checksums(self):
        self.out.mkdir()
        (self.out / "SHA256SUMS").write_text("a" * 64 + "  retoque-payload-1.2.3-x64.zip\n")
        log = self.build()
        self.assertIn("gh release create retoque-v1.2.3", log)
        archive = self.out / "retoque-app-1.2.3-windows-x64.zip"
        sums = package.sums_parse((self.out / "SHA256SUMS").read_bytes())
        self.assertEqual(sums[archive.name], hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertIn("retoque-payload-1.2.3-x64.zip", sums)
        with zipfile.ZipFile(archive) as zf:
            self.assertEqual(set(zf.namelist()), package.REQUIRED | {"retoque-app.exe", "SHA256SUMS"})
            members = package.sums_parse(zf.read("SHA256SUMS"))
            self.assertTrue(all(hashlib.sha256(zf.read(name)).hexdigest() == digest for name, digest in members.items()))

    def test_mislabeled_version_and_non_windows_exe(self):
        with self.assertRaisesRegex(ValueError, "version"):
            self.build("1.2.30")
        self.exe.write_bytes(b"ELF executable")
        with self.assertRaisesRegex(ValueError, "Windows"):
            self.build()
        self.assertFalse(self.out.exists())

    def test_proprietary_and_unsafe_paths(self):
        for bad in ["nvngx_dlssnr.dll", "../escape", "Licenses/CON.txt", "translations/evil.exe", "OptiScaler/nvngx_dlss.dll", "OptiScaler/cache.spv"]:
            with self.subTest(bad=bad):
                self.write_payload(bad)
                with self.assertRaisesRegex(ValueError, "forbidden"):
                    self.build()
                self.assertFalse(self.out.exists())

    def test_corrupt_payload_checksum(self):
        with zipfile.ZipFile(self.payload, "r") as old:
            entries = {name: old.read(name) for name in old.namelist()}
        entries["OptiScaler.dll"] = b"modified"
        with zipfile.ZipFile(self.payload, "w") as new:
            for name, data in entries.items():
                new.writestr(name, data)
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.build()


if __name__ == "__main__":
    unittest.main()
