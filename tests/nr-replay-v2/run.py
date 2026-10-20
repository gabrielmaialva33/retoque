#!/usr/bin/env python3
import copy
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from array import array

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tests/_support")]
from replay_fixture import corpus, run, save_json
from tools.replay.format import load_capture, load_run, read_plane, digest, Image
from tools.replay.dxgi import FORMATS, decode
from tools.replay.geometry import aligned, prepare
from tools.replay.conformance import capture_v2, run_v2, projection
from tools.replay.judge import judge, calibrate
from tools.replay.metrics import rgb, coordinates
from tools.replay.backend import select

try:
    import numpy

    BACKENDS = ["python", "numpy"]
except ImportError:
    BACKENDS = ["python"]
    print("SKIP NumPy equivalence: optional NumPy absent")


class V2(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        old = os.environ.get("RETOQUE_REPLAY_BACKEND")
        self.addCleanup(
            lambda: (
                os.environ.pop("RETOQUE_REPLAY_BACKEND", None)
                if old is None
                else os.environ.__setitem__("RETOQUE_REPLAY_BACKEND", old)
            )
        )
        self.cap, pixels = corpus(self.root / "clip", moving=True)
        self.ref = run(self.root / "ref", self.cap, pixels)
        self.cand = run(
            self.root / "cand",
            self.cap,
            pixels,
            transform=lambda i, p: [v * (1 + 0.01 * i) for v in p],
        )
        # Whitelist has RGBA32, not RGB32. Add alpha without changing RGB values.
        for path in (self.cap, self.ref, self.cand):
            d = json.loads(path.read_text())
            frames = d["frames"]
            for f in frames:
                p = f["color"] if "color" in f else f["output"]
                file = path.parent / p["path"]
                vals = struct.unpack(
                    "<" + str(p["width"] * p["height"] * 3) + "f", file.read_bytes()
                )
                rgba = [
                    v for i in range(0, len(vals), 3) for v in (*vals[i : i + 3], 1)
                ]
                file.write_bytes(struct.pack("<" + str(len(rgba)) + "f", *rgba))
                p.update(channels=4, row_pitch=p["width"] * 16, sha256=digest(file))
            save_json(path, d)
        for p in (self.ref, self.cand):
            d = json.loads(p.read_text())
            d["capture_sha256"] = digest(self.cap)
            save_json(p, d)
        self.c2 = self.cap.with_name("v2.json")
        save_json(self.c2, capture_v2(json.loads(self.cap.read_text())))
        self.r2 = self.ref.with_name("v2.json")
        save_json(self.r2, run_v2(json.loads(self.ref.read_text()), digest(self.c2)))
        self.t2 = self.cand.with_name("v2.json")
        save_json(self.t2, run_v2(json.loads(self.cand.read_text()), digest(self.c2)))

    def test_conformance(self):
        for b in BACKENDS:
            os.environ["RETOQUE_REPLAY_BACKEND"] = b
            a = judge(self.cap, self.ref, self.cand)
            v = judge(self.c2, self.r2, self.t2)
            self.assertNotEqual(a["verdict"], "INVALID", a.get("reasons"))
            self.assertEqual(projection(a), projection(v))

    def test_calibrated_verdict_conformance(self):
        for backend in BACKENDS:
            os.environ["RETOQUE_REPLAY_BACKEND"] = backend
            repeats, repeats2 = [], []
            for i in range(5):
                r = json.loads(self.ref.read_text())
                r["producer"]["trial"] = f"fixture-repeat-{i}"
                p = self.ref.with_name(f"aa{i}.json")
                save_json(p, r)
                q = self.ref.with_name(f"aa{i}-v2.json")
                save_json(q, run_v2(r, digest(self.c2)))
                repeats.append(p)
                repeats2.append(q)
            a = calibrate(self.cap, self.ref, repeats)
            b = calibrate(self.c2, self.r2, repeats2)
            self.assertEqual(a["bounds"], b["bounds"])
            for old, new, expected in (
                (self.ref, self.r2, "PASS"),
                (self.cand, self.t2, "FAIL"),
            ):
                v1 = judge(self.cap, self.ref, old, a)
                v2 = judge(self.c2, self.r2, new, b)
                self.assertEqual(v1["verdict"], expected)
                self.assertEqual(projection(v1), projection(v2))

    def test_formats(self):
        cases = {
            2: (struct.pack("<4f", 0.25, 0.5, 1, 1), [0.25, 0.5, 1, 1]),
            10: (struct.pack("<4e", 0.25, 0.5, 1, 1), [0.25, 0.5, 1, 1]),
            16: (struct.pack("<2f", -2, 3), [-2, 3]),
            34: (struct.pack("<2e", -2, 3), [-2, 3]),
            41: (struct.pack("<f", 0.5), [0.5]),
            40: (struct.pack("<f", 0.5), [0.5]),
            26: (
                struct.pack("<I", (15 << 6) | ((16 << 6) << 11) | ((14 << 5) << 22)),
                [1, 2, 0.5],
            ),
            24: (
                struct.pack("<I", 1023 | (512 << 10) | (1 << 20) | (2 << 30)),
                [1, 512 / 1023, 1 / 1023, 2 / 3],
            ),
        }
        for fmt in (28, 29, 87, 91):
            cases[fmt] = (
                bytes([0, 128, 255, 255]),
                [1, 128 / 255, 0, 1] if fmt in (87, 91) else [0, 128 / 255, 1, 1],
            )
        self.assertEqual(set(cases), set(FORMATS))
        for fmt, (raw, expected) in cases.items():
            for b in BACKENDS:
                actual = decode(raw, fmt, b == "numpy")
                self.assertEqual(
                    actual.tobytes(), array("f", expected).tobytes(), (fmt, b)
                )
                file = self.root / "native"
                file.write_bytes(raw)
                desc = dict(
                    path="native",
                    encoding="raw-dxgi",
                    dxgi_format=fmt,
                    width=1,
                    height=1,
                    pixel_bytes=len(raw),
                    row_pitch=len(raw),
                    sha256=digest(file),
                )
                os.environ["RETOQUE_REPLAY_BACKEND"] = b
                image = read_plane(self.root, desc)
                self.assertEqual(image.data, actual)
                if fmt in (29, 91):
                    linear = select()[1].rgb(image, "linear-rec709")
                    self.assertEqual(
                        linear.data[1],
                        array(
                            "f", [((array("f", [128 / 255])[0] + 0.055) / 1.055) ** 2.4]
                        )[0],
                    )
        # Unsigned mini-floats: smallest subnormal and maximum finite, plus nonfinite rejection.
        self.assertEqual(decode(struct.pack("<I", 1), 26)[0], 2**-20)
        self.assertEqual(decode(struct.pack("<I", (30 << 6) | 63), 26)[0], 65024)

    def test_all_native_color_formats_through_judge(self):
        original = json.loads(self.c2.read_text())
        for fmt, (size, channels, code) in FORMATS.items():
            if code:
                pixel = struct.pack(
                    "<" + str(channels) + code,
                    *([128] * channels if code == "B" else [0.5] * channels),
                )
            else:
                pixel = struct.pack(
                    "<I",
                    (
                        (15 << 6) | ((15 << 6) << 11) | ((15 << 5) << 22)
                        if fmt == 26
                        else 0xFFFFFFFF
                    ),
                )
            raw = pixel * 48
            c = copy.deepcopy(original)
            for f in c["frames"]:
                file = self.c2.parent / f"native-color-{f['id']}"
                file.write_bytes(raw)
                f["color"] = dict(
                    path=file.name,
                    encoding="raw-dxgi",
                    dxgi_format=fmt,
                    width=8,
                    height=6,
                    pixel_bytes=size,
                    row_pitch=8 * size,
                    sha256=digest(file),
                )
                f["depth"]["dxgi_format"] = 40
            save_json(self.c2, c)
            for path in (self.r2, self.t2):
                r = json.loads(path.read_text())
                r["capture_sha256"] = digest(self.c2)
                for f, source in zip(r["frames"], c["frames"]):
                    f["output"] = dict(source["color"])
                    (path.parent / f["output"]["path"]).write_bytes(raw)
                save_json(path, r)
            for backend in BACKENDS:
                os.environ["RETOQUE_REPLAY_BACKEND"] = backend
                report = judge(self.c2, self.r2, self.t2)
                self.assertNotEqual(
                    report["verdict"], "INVALID", (fmt, report.get("reasons"))
                )
                self.assertEqual(report["metrics"]["all"]["golden_max"], 0)

    def test_mutations(self):
        original = json.loads(self.c2.read_text())
        mutations = [
            lambda c: c.update(finalized=False),
            lambda c: c["frames"][0]["color"].update(sha256="0" * 64),
            lambda c: c["frames"][0]["color"].update(row_pitch=1),
            lambda c: c["frames"][0]["color"].update(pixel_bytes=8),
            lambda c: c["frames"][0]["color"].update(dxgi_format=999),
            lambda c: c["frames"][0]["params"].update(mv_scale=[float("nan"), 1]),
            lambda c: c["frames"][0]["params"].update(render_subrect=[9, 6]),
            lambda c: c["frames"][0]["params"].update(depth_base=[1, 0]),
            lambda c: c["frames"][0]["params"].update(motion_base=[1, 0]),
            lambda c: c["frames"][0]["color"].update(source_flags=-1),
            lambda c: c["frames"][0]["color"].update(source_format=True),
            lambda c: c["frames"][1]["color"].update(width=4, row_pitch=64, height=12),
        ]
        for mutate in mutations:
            d = copy.deepcopy(original)
            mutate(d)
            self.c2.write_text(json.dumps(d))
            with self.assertRaises((ValueError, OSError)):
                load_capture(self.c2)
        save_json(self.c2, original)
        file = self.c2.parent / original["frames"][0]["depth"]["path"]
        file.write_bytes(file.read_bytes() + b"X")
        with self.assertRaisesRegex(ValueError, "byte count"):
            load_capture(self.c2)

    def test_native_roles_and_geometry_metrics(self):
        c = json.loads(self.c2.read_text())
        for i, f in enumerate(c["frames"]):
            # Native RGBA vectors, render-size region in a larger allocation; .zw are irrelevant.
            raw = struct.pack("<" + str(24 * 4) + "e", *([-0.25, 0, 77, 88] * 24))
            file = self.c2.parent / f"mv4-{i}"
            file.write_bytes(raw)
            f["motion"] = dict(
                path=file.name,
                encoding="raw-dxgi",
                dxgi_format=10,
                width=6,
                height=4,
                pixel_bytes=8,
                row_pitch=48,
                sha256=digest(file),
            )
            file = self.c2.parent / f"depth32-{i}"
            file.write_bytes(
                struct.pack(
                    "<24f", *[0.1 + 0.03 * x for y in range(4) for x in range(6)]
                )
            )
            f["depth"] = dict(
                path=file.name,
                encoding="raw-dxgi",
                dxgi_format=40,
                width=6,
                height=4,
                pixel_bytes=4,
                row_pitch=24,
                sha256=digest(file),
            )
            f["params"].update(
                create_flags=3,
                render_subrect=[4, 3],
                depth_base=[i % 2, 1],
                motion_base=[2, 1],
                mv_scale=[2, 1],
            )
        save_json(self.c2, c)
        for path in (self.r2, self.t2):
            r = json.loads(path.read_text())
            r["capture_sha256"] = digest(self.c2)
            save_json(path, r)
        results = []
        for b in BACKENDS:
            os.environ["RETOQUE_REPLAY_BACKEND"] = b
            result = judge(self.c2, self.r2, self.t2)
            self.assertNotEqual(result["verdict"], "INVALID", result.get("reasons"))
            results.append(result)
        if len(results) == 2:
            self.assertEqual(results[0]["verdict"], results[1]["verdict"])
            for region, metrics in results[0]["metrics"].items():
                for key, value in metrics.items():
                    other = results[1]["metrics"][region][key]
                    if isinstance(value, (int, float)):
                        self.assertAlmostEqual(value, other, delta=1e-6)
                    else:
                        self.assertEqual(value, other)
        # A tiny oracle would broadcast in NumPy but be out of bounds in scalar; reject at the reader.
        f = c["frames"][0]
        file = self.c2.parent / "oracle"
        file.write_bytes(struct.pack("<2f", 0, 0))
        f["motion_oracle"] = dict(
            path="oracle",
            encoding="raw-dxgi",
            dxgi_format=16,
            width=1,
            height=1,
            pixel_bytes=8,
            row_pitch=8,
            sha256=digest(file),
        )
        save_json(self.c2, c)
        with self.assertRaisesRegex(ValueError, "output-sized"):
            load_capture(self.c2)

    def test_nonfinite_pixels_and_run_mutations(self):
        file = self.root / "bad"
        file.write_bytes(struct.pack("<I", 31 << 6))
        desc = dict(
            path="bad",
            encoding="raw-dxgi",
            dxgi_format=26,
            width=1,
            height=1,
            pixel_bytes=4,
            row_pitch=4,
            sha256=digest(file),
        )
        for b in BACKENDS:
            os.environ["RETOQUE_REPLAY_BACKEND"] = b
            with self.assertRaisesRegex(ValueError, "nonfinite"):
                read_plane(self.root, desc)
        c = load_capture(self.c2)
        r = json.loads(self.r2.read_text())
        r["frames"][0]["output"]["sha256"] = "0" * 64
        save_json(self.r2, r)
        with self.assertRaisesRegex(ValueError, "checksum"):
            load_run(self.r2, c)

    def test_v1_does_not_silently_accept_v2_planes(self):
        c = json.loads(self.cap.read_text())
        c["frames"][0]["color"] = json.loads(self.c2.read_text())["frames"][0]["color"]
        save_json(self.cap, c)
        with self.assertRaisesRegex(ValueError, "v1 plane encoding"):
            load_capture(self.cap)

    def test_optional_metadata(self):
        d = json.loads(self.c2.read_text())
        d["frames"][0]["color"].update(source_flags=128, source_format=39)
        save_json(self.c2, d)
        load_capture(self.c2)

    def test_geometry(self):
        capture = load_capture(self.c2)
        f = capture["frames"][1]
        f["params"].update(
            render_subrect=[4, 3],
            depth_base=[1, 1],
            motion_base=[2, 1],
            create_flags=3,
            mv_scale=[2, 3],
        )
        depth = Image(8, 6, 1, array("f", range(48)))
        motion = Image(8, 6, 2, array("f", [1, 2] * 48))
        for b in BACKENDS:
            os.environ["RETOQUE_REPLAY_BACKEND"] = b
            c, ff, d, m = prepare(capture, f, depth, motion)
            self.assertEqual(list(d.data[:8]), [9, 9, 10, 10, 11, 11, 12, 12])
            self.assertEqual(c["motion"]["scale"], [4, 6])
            self.assertEqual(
                coordinates(
                    0, 0, m.data[:2], c["motion"], 8, 6, [0, 0], [0, 0], "removed"
                ),
                (4, 12),
            )
            f["params"]["create_flags"] = 1
            f["params"]["motion_base"] = [0, 0]
            c, ff, d, m = prepare(capture, f, depth, motion)
            self.assertEqual(c["motion"]["scale"], [2, 3])
            f["params"].update(create_flags=3, motion_base=[2, 1])
        f["params"]["create_flags"] = 7
        capture["motion"]["includes_jitter"] = True
        c, ff, d, m = prepare(capture, f, depth, motion)
        self.assertEqual(
            coordinates(0, 0, [0, 0], c["motion"], 8, 6, [1, 2], [3, 5], "removed"),
            (-2, -3),
        )
        if "numpy" in BACKENDS:
            from tools.replay.numpy_metrics import coordinates as numpy_coordinates

            zero = Image(8, 6, 2, array("f", [0, 0] * 48))
            dx, dy = numpy_coordinates(
                c, dict(ff, jitter=[1, 2]), dict(ff, jitter=[3, 5]), zero
            )
            self.assertTrue((dx == -2).all() and (dy == -3).all())


if __name__ == "__main__":
    unittest.main()
