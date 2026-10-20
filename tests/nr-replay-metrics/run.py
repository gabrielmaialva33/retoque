#!/usr/bin/env python3
"""Independent analytic and end-to-end mutation controls for the replay judge."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tests/_support")]
from replay_fixture import corpus, run, change, plane


class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.capture, self.pixels = corpus(self.root / "clip", moving=True)
        self.reference = run(self.root / "ref", self.capture, self.pixels)
        from tools.replay.judge import judge, calibrate

        self.judge, self.calibrate = judge, calibrate
        self.repeats = [
            run(self.root / f"aa{i}", self.capture, self.pixels, salt=i + 1)
            for i in range(5)
        ]
        self.calibration = self.calibrate(self.capture, self.reference, self.repeats)

    def trial(self, transform=None, **kwargs):
        return run(
            self.root / "candidate",
            self.capture,
            self.pixels,
            "candidate",
            transform,
            **kwargs,
        )

    def test_same_inputs_calibrated_pass_and_missing_calibration_inconclusive(self):
        candidate = self.trial()
        self.assertEqual(
            self.judge(self.capture, self.reference, candidate)["verdict"],
            "INCONCLUSIVE",
        )
        report = self.judge(self.capture, self.reference, candidate, self.calibration)
        self.assertEqual(report["verdict"], "PASS", report)
        self.assertEqual(report["metrics"]["all"]["golden_mae"], 0)
        self.assertGreater(report["metrics"]["all"]["coverage"], 0.8)
        self.assertLess(report["metrics"]["all"]["temporal_mae"], 1e-7)

    def test_pulse_regions_and_exposure_transition(self):
        candidate = self.trial(
            lambda i, rgb: [x * (1.2 if i % 2 else 0.8) for x in rgb]
        )
        report = self.judge(self.capture, self.reference, candidate, self.calibration)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertGreater(report["metrics"]["left"]["pulse_p95"], 0.4)
        from tools.replay.metrics import light_step

        self.assertAlmostEqual(light_step([1, 1], [2, 2], [10, 10], [20, 20]), 0.0)
        self.assertIsNone(light_step([0], [0], [1], [1]))

    def test_blur_and_stale_output_fail(self):
        for transform in (
            lambda i, rgb: [sum(rgb) / len(rgb)] * len(rgb),
            lambda i, rgb: [v * 1.1 for v in self.pixels[0]],
        ):
            report = self.judge(
                self.capture, self.reference, self.trial(transform), self.calibration
            )
            self.assertEqual(report["verdict"], "FAIL")
            self.assertGreater(report["metrics"]["all"]["golden_mae"], 0.01)

    def test_low_coverage_and_no_temporal_pairs_inconclusive(self):
        c = json.loads(self.capture.read_text())
        for f in c["frames"]:
            f["motion"] = plane(
                self.capture.parent, f"{f['id']}.far", [100.0, 100.0] * 48, channels=2
            )
        self.capture.write_text(json.dumps(c))
        ref = run(self.root / "farref", self.capture, self.pixels)
        cand = self.trial()
        report = self.judge(self.capture, ref, cand)
        self.assertEqual(report["verdict"], "INCONCLUSIVE")
        self.assertEqual(report["metrics"]["all"]["coverage"], 0.0)
        change(self.capture, lambda c: [f.update(reset=True) for f in c["frames"]])
        ref = run(self.root / "resetref", self.capture, self.pixels)
        report = self.judge(self.capture, ref, self.trial())
        self.assertEqual(report["verdict"], "INCONCLUSIVE")

    def test_warp_conventions_bilinear_jitter_and_depth(self):
        from tools.replay.metrics import coordinates, sample, corresponds
        from tools.replay.format import Image

        im = Image(2, 2, 1, [0.0, 2.0, 4.0, 6.0])
        self.assertEqual(sample(im, 0.5, 0.5), [3.0])
        m = dict(
            direction="current-to-previous",
            units="uv",
            scale=[2, -1],
            includes_jitter=False,
        )
        self.assertEqual(
            coordinates(2, 3, [0.25, 0.5], m, 8, 6, [0.5, 0], [0, 0.25]), (5.5, 0.25)
        )
        m["includes_jitter"] = True
        self.assertEqual(
            coordinates(2, 3, [0.25, 0.5], m, 8, 6, [0.5, 0], [0, 0.25]), (6.0, 0.0)
        )
        self.assertFalse(corresponds(0.2, 0.8, dict(kind="device", threshold=0.02)))
        self.assertTrue(corresponds(100, 101, dict(kind="linear", threshold=0.02)))

    def test_motion_oracle_mutations(self):
        for mutation in ("zero", "swap", "scale", "jitter"):
            clip, pixels = corpus(self.root / mutation, moving=True)
            c = json.loads(clip.read_text())
            for f in c["frames"]:
                f["motion_oracle"] = plane(
                    clip.parent, f"{f['id']}.oracle", [-1.0, 0.0] * 48, channels=2
                )
                vals = {
                    "zero": [0, 0],
                    "swap": [0, -1],
                    "scale": [-2, 0],
                    "jitter": [-1, 0],
                }[mutation]
                f["motion"] = plane(
                    clip.parent, f"{f['id']}.wrong", vals * 48, channels=2
                )
                if mutation == "jitter":
                    f["jitter"] = [f["id"] * 0.25, 0]
            clip.write_text(json.dumps(c))
            ref = run(self.root / (mutation + "ref"), clip, pixels)
            report = self.judge(clip, ref, ref)
            self.assertEqual(report["verdict"], "INVALID", mutation)

    def test_environment_partial_passes_and_calibration_binding(self):
        candidate = self.trial()
        change(candidate, lambda r: r["environment"].update(model_sha256="3" * 64))
        self.assertEqual(
            self.judge(self.capture, self.reference, candidate, self.calibration)[
                "verdict"
            ],
            "INVALID",
        )
        candidate = self.trial()
        change(candidate, lambda r: r["frames"][2].update(completed_passes=0))
        self.assertEqual(
            self.judge(self.capture, self.reference, candidate, self.calibration)[
                "verdict"
            ],
            "INVALID",
        )
        candidate = self.trial()
        bad = dict(self.calibration, capture_sha256="0" * 64)
        self.assertEqual(
            self.judge(self.capture, self.reference, candidate, bad)["verdict"],
            "INVALID",
        )
        with self.assertRaises(ValueError):
            self.calibrate(self.capture, self.reference, [self.reference] * 5)

    def test_malformed_objects_are_invalid_not_exceptions(self):
        for key in ("environment", "producer", "requirements", "frames"):
            candidate = self.trial()
            change(candidate, lambda r: r.update({key: []}))
            self.assertEqual(
                self.judge(self.capture, self.reference, candidate)["verdict"],
                "INVALID",
            )
        for key in ("depth", "motion", "provenance"):
            clip, pixels = corpus(self.root / key)
            change(clip, lambda r: r.update({key: []}))
            ref = run(self.root / (key + "run"), clip, pixels)
            self.assertEqual(self.judge(clip, ref, ref)["verdict"], "INVALID")

    def test_aa_rejects_carry_even_if_settings_lie(self):
        change(
            self.repeats[0],
            lambda r: r["frames"][2].update(
                kind="carried", interval=2, completed_passes=0
            ),
        )
        with self.assertRaises(ValueError):
            self.calibrate(self.capture, self.reference, self.repeats)

    def test_opposing_region_pulses_do_not_cancel_locally(self):
        clip, pixels = corpus(self.root / "flat")
        pixels = [[0.4] * 144 for _ in pixels]
        data = json.loads(clip.read_text())
        for f, values in zip(data["frames"], pixels):
            f["color"] = plane(clip.parent, f"{f['id']}.flat", values)
        clip.write_text(json.dumps(data))
        ref = run(self.root / "flat-ref", clip, pixels)

        def opposing(i, values):
            return [
                value * (1.2 if (p // 3 % 8 < 4) == (i % 2 == 0) else 0.8)
                for p, value in enumerate(values)
            ]

        cand = run(self.root / "flat-cand", clip, pixels, transform=opposing)
        report = self.judge(clip, ref, cand)
        self.assertLess(report["metrics"]["all"]["pulse_p95"], 1e-6)
        self.assertGreater(report["metrics"]["left"]["pulse_p95"], 0.5)

    def test_golden_bad_fraction_counts_pixels_not_channels(self):
        def red_only(i, values):
            return [v + 0.1 if p % 3 == 0 else v for p, v in enumerate(values)]

        report = self.judge(self.capture, self.reference, self.trial(red_only))
        self.assertEqual(report["metrics"]["all"]["golden_bad_fraction"], 1.0)

    def test_calibration_floors_and_policy_binding(self):
        policy = dict(floors={key: 1.0 for key in self.calibration["bounds"]["all"]})
        cal = self.calibrate(self.capture, self.reference, self.repeats, policy)
        cand = self.trial(lambda i, values: [v + 0.001 for v in values])
        self.assertEqual(
            self.judge(self.capture, self.reference, cand, cal, policy)["verdict"],
            "PASS",
        )
        self.assertEqual(
            self.judge(self.capture, self.reference, cand, cal)["verdict"], "INVALID"
        )
        cal["bounds"]["all"]["golden_max"] += 1
        self.assertEqual(
            self.judge(self.capture, self.reference, cand, cal, policy)["verdict"],
            "INVALID",
        )
        broken = dict(self.calibration, observations=[[]] * 5)
        self.assertEqual(
            self.judge(self.capture, self.reference, cand, broken)["verdict"], "INVALID"
        )

    def test_srgb_decode_and_reference_is_every_frame(self):
        from tools.replay.metrics import rgb
        from tools.replay.format import Image

        result = rgb(Image(1, 1, 3, [0.04045, 0.5, 1]), "srgb-rec709")
        self.assertAlmostEqual(result.data[0], 0.003130805, places=8)
        self.assertAlmostEqual(result.data[1], 0.21404114, places=7)
        change(
            self.reference,
            lambda r: r["frames"][3].update(
                kind="carried", interval=2, completed_passes=0
            ),
        )
        self.assertEqual(
            self.judge(self.capture, self.reference, self.trial())["verdict"], "INVALID"
        )

    def test_pre_exposure_changes_do_not_create_temporal_edit_error(self):
        clip, pixels = corpus(self.root / "exposure", moving=False)
        data = json.loads(clip.read_text())
        for i, (f, values) in enumerate(zip(data["frames"], pixels)):
            f["pre_exposure"] = i + 1
            pixels[i] = [v * (i + 1) for v in values]
            f["color"] = plane(clip.parent, f"{i}.exposed", pixels[i])
        clip.write_text(json.dumps(data))
        ref = run(self.root / "exposure-ref", clip, pixels)
        report = self.judge(clip, ref, ref)
        self.assertLess(report["metrics"]["all"]["temporal_mae"], 1e-7)
        self.assertLess(report["metrics"]["all"]["pulse_p95"], 1e-6)

    def test_cli_outputs_pngs_and_invalid_clears_stale_outputs(self):
        candidate = self.trial()
        out = self.root / "out"
        args = [
            sys.executable,
            "-m",
            "tools.replay",
            "--capture",
            str(self.capture),
            "--reference",
            str(self.reference),
            "--candidate",
            str(candidate),
            "--out",
            str(out),
        ]
        completed = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(completed.returncode, 3, completed.stderr)
        for name in ("contact-sheet.png", "diff-heatmap.png"):
            data = (out / name).read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            offset, compressed, shape = 8, b"", None
            while offset < len(data):
                size = struct.unpack_from(">I", data, offset)[0]
                kind = data[offset + 4 : offset + 8]
                body = data[offset + 8 : offset + 8 + size]
                crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
                self.assertEqual(crc, zlib.crc32(kind + body) & 0xFFFFFFFF)
                if kind == b"IHDR":
                    shape = struct.unpack(">2I5B", body)
                if kind == b"IDAT":
                    compressed += body
                offset += size + 12
            width, height = shape[:2]
            self.assertEqual(shape[2:4], (8, 2))
            decoded = zlib.decompress(compressed)
            self.assertEqual(len(decoded), (width * 3 + 1) * height)
            self.assertTrue(
                all(decoded[y * (width * 3 + 1)] == 0 for y in range(height))
            )
        self.assertEqual(len((out / "frames.jsonl").read_text().splitlines()), 7)
        change(candidate, lambda r: r.update(complete=False))
        completed = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(
            json.loads((out / "result.json").read_text())["verdict"], "INVALID"
        )
        self.assertFalse((out / "contact-sheet.png").exists())

    def test_calibration_cli_and_output_cannot_overwrite_inputs(self):
        calibration = self.root / "cli-calibration.json"
        args = [
            sys.executable,
            "-m",
            "tools.replay",
            "calibrate",
            "--capture",
            str(self.capture),
            "--reference",
            str(self.reference),
        ]
        for repeat in self.repeats:
            args.extend(["--repeat", str(repeat)])
        result = subprocess.run(
            args + ["--out", str(calibration)], cwd=ROOT, capture_output=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(calibration.read_text())["samples"], 5)
        original = self.reference.read_bytes()
        result = subprocess.run(
            args + ["--out", str(self.reference)], cwd=ROOT, capture_output=True
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.reference.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
