#!/usr/bin/env python3
"""Wire-format rejection tests; no GPU or third-party Python modules."""

import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tests/_support")]
from replay_fixture import corpus, run, change, plane


class FormatTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.capture, self.pixels = corpus(self.root / "clip")
        self.result = run(self.root / "run", self.capture, self.pixels)
        from tools.replay import format as wire

        self.wire = wire

    def test_valid_capture_and_run(self):
        c = self.wire.load_capture(self.capture)
        r = self.wire.load_run(self.result, c)
        self.assertEqual(len(r["frames"]), 8)
        image = self.wire.read_plane(c["_root"], c["frames"][0]["color"])
        self.assertAlmostEqual(image.data[0], 0.2)

    def test_bad_manifests(self):
        original = self.capture.read_bytes()
        mutations = [
            lambda x: x.update(version=2),
            lambda x: x.update(complete=False),
            lambda x: x["frames"].pop(3),
            lambda x: x["frames"][2].update(id=2),
            lambda x: x["frames"][2].pop("exposure_value"),
            lambda x: x["frames"][2].update(time_ms=0),
            lambda x: x["frames"][0].update(reset=False),
            lambda x: x["motion"].update(direction="previous-to-current"),
            lambda x: x["frames"][1].update(completion="assumed-8-frames"),
            lambda x: x["frames"][2].update(pre_exposure=0),
            lambda x: x["regions"].update(bad=[7, 0, 2, 6]),
            lambda x: x["frames"][0]["color"].update(width=1000000),
            lambda x: x["frames"][0]["motion"].update(width=4),
            lambda x: x.update(version=True),
            lambda x: x.pop("color_jitter"),
            lambda x: x.update(color_jitter="unknown"),
            lambda x: x.update(color_jitter=True),
        ]
        for mutate in mutations:
            self.capture.write_bytes(original)
            change(self.capture, mutate)
            with (
                self.subTest(mutation=mutations.index(mutate)),
                self.assertRaises(ValueError),
            ):
                self.wire.load_capture(self.capture)

    def test_duplicate_json_and_nonfinite(self):
        for text in ['{"schema":1,"schema":2}', '{"value":NaN}', '{"value":1e999}']:
            self.capture.write_text(text)
            with self.assertRaises(ValueError):
                self.wire.load_capture(self.capture)

    def test_hash_length_nonfinite_padding_and_path(self):
        c = self.wire.load_capture(self.capture)
        d = c["frames"][0]["color"].copy()
        for key, val in [
            ("path", "../outside"),
            ("path", "/etc/passwd"),
            ("path", "C:\\bad"),
            ("row_pitch", 1),
            ("sha256", "0" * 64),
        ]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.wire.read_plane(c["_root"], dict(d, **{key: val}))
        target = c["_root"] / d["path"]
        original = target.read_bytes()
        for data in (original[:-1], struct.pack("<f", float("nan")) + original[4:]):
            target.write_bytes(data)
            d["sha256"] = hashlib.sha256(data).hexdigest()
            with self.assertRaises(ValueError):
                self.wire.read_plane(c["_root"], d)
        outside = self.root / "outside"
        outside.write_bytes(original)
        target.unlink()
        target.symlink_to(outside)
        d["sha256"] = hashlib.sha256(original).hexdigest()
        with self.assertRaises(ValueError):
            self.wire.read_plane(c["_root"], d)

    def test_raw_half_u8_and_padded_rows(self):
        for dtype, vals, want in [
            ("f16", [0.5, 1.0, 2.0, 3.0], [0.5, 1.0, 2.0, 3.0]),
            ("u8", [0, 255, 128, 64], [0, 1, 128 / 255, 64 / 255]),
        ]:
            desc = plane(self.root, "tiny", vals, 2, 2, 1, dtype)
            im = self.wire.read_plane(self.root, desc)
            for a, b in zip(im.data, want):
                self.assertAlmostEqual(a, b)
        data = struct.pack("<2f", 1, 2) + b"pad!" + struct.pack("<2f", 3, 4) + b"pad!"
        (self.root / "pad").write_bytes(data)
        d = dict(
            path="pad",
            encoding="raw",
            dtype="f32",
            width=2,
            height=2,
            channels=1,
            row_pitch=12,
            sha256=hashlib.sha256(data).hexdigest(),
        )
        self.assertEqual(list(self.wire.read_plane(self.root, d).data), [1, 2, 3, 4])

    def test_pfm_orientation_scale_endian_and_truncation(self):
        for endian, scale in [("<", -2), (">", 2)]:
            data = f"Pf\n2 2\n{scale}\n".encode() + struct.pack(
                endian + "4f", 3, 4, 1, 2
            )
            (self.root / "a.pfm").write_bytes(data)
            d = dict(
                path="a.pfm",
                encoding="pfm",
                dtype="f32",
                width=2,
                height=2,
                channels=1,
                row_pitch=8,
                sha256=hashlib.sha256(data).hexdigest(),
            )
            self.assertEqual(
                list(self.wire.read_plane(self.root, d).data), [2, 4, 6, 8]
            )
            (self.root / "a.pfm").write_bytes(data[:-1])
            d["sha256"] = hashlib.sha256(data[:-1]).hexdigest()
            with self.assertRaises(ValueError):
                self.wire.read_plane(self.root, d)

    def test_run_completeness_and_coverage(self):
        c = self.wire.load_capture(self.capture)
        original = self.result.read_bytes()
        mutations = [
            lambda x: x.update(capture_sha256="0" * 64),
            lambda x: x["frames"].pop(),
            lambda x: x["frames"][1].update(completed_passes=0),
            lambda x: x["frames"][1].update(gpu_ms=-1),
            lambda x: x["frames"][1].update(completion="pending"),
            lambda x: x["requirements"].update(min_carried=1),
            lambda x: x["producer"].pop("trial"),
            lambda x: x["producer"].update(trial=" "),
            lambda x: x["producer"].update(trial=3),
        ]
        for mutate in mutations:
            self.result.write_bytes(original)
            change(self.result, mutate)
            with self.assertRaises(ValueError):
                self.wire.load_run(self.result, c)

    def test_skips_only_unscored_reasoned_and_fenced(self):
        c = self.wire.load_capture(self.capture)
        change(
            self.result,
            lambda r: r["frames"][0].update(
                kind="skipped",
                interval=0,
                requested_passes=0,
                completed_passes=0,
                skip="extent settling",
            ),
        )
        accepted = self.result.read_bytes()
        r = self.wire.load_run(self.result, c)
        self.assertEqual(r["_counts"], dict(model=7, carried=0))
        for changes in (
            {"skip": None},
            {"skip": ""},
            {"skip": " "},
            {"skip": 12},
            {"completed_passes": 1},
            {"requested_passes": 4},
            {"interval": 5},
            {"completion": "pending"},
            {"gpu_ms": None},
        ):
            self.result.write_bytes(accepted)
            change(self.result, lambda r: r["frames"][0].update(changes))
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.wire.load_run(self.result, c)
        self.result.write_bytes(accepted)
        change(
            self.result,
            lambda r: r["frames"][1].update(
                kind="skipped",
                interval=0,
                requested_passes=0,
                completed_passes=0,
                skip="model refused",
            ),
        )
        with self.assertRaisesRegex(ValueError, "model refused"):
            self.wire.load_run(self.result, c)

    def test_explicit_registry_paths_and_requirements(self):
        import importlib.util
        from unittest.mock import patch

        spec = importlib.util.spec_from_file_location(
            "replay_registry_test", ROOT / "tests/run_all.py"
        )
        registry = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = registry
        spec.loader.exec_module(registry)
        directory = self.root / "tests"
        (directory / "nr" / "suite").mkdir(parents=True)
        (directory / "nr" / "suite" / "run.py").write_text("pass\n")
        entry = dict(
            path="nr/suite", domain="nr", requires=["python"], tier="host", timeout=10
        )
        with patch.object(registry, "TESTS", directory):
            self.assertEqual(registry.check_registry({"stable-id": entry}), [])
            self.assertTrue(registry.check_registry({"a": entry, "b": entry}))
            self.assertTrue(
                registry.check_registry({"a": dict(entry, path="../outside")})
            )
            self.assertTrue(
                registry.check_registry({"a": dict(entry, requires=["misspelled"])})
            )
            (directory / "orphan").mkdir()
            self.assertTrue(registry.check_registry({"stable-id": entry}))
        with patch.object(registry.shutil, "which", return_value=None):
            self.assertEqual(registry.entry_blocker(entry), "")
            self.assertTrue(registry.entry_blocker(dict(entry, requires=["cxx"])))


if __name__ == "__main__":
    unittest.main()
