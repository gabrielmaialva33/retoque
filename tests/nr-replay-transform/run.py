#!/usr/bin/env python3
"""tools/replay/transform.py on a small synthetic capture: the derived captures are the same scene in another
representation, byte for byte where they claim to be, and the seam's own validator (run_replay.load_capture) takes the
ones it can replay. Stdlib (NumPy optional, checked equal to the scalar path). No GPU, DLL or model."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tests/dlssnr-loopback')]
from tools.replay import transform  # noqa: E402
import run_replay  # noqa: E402

W, H, N = 20, 16, 6


def synthetic(directory):
    """A v1 capture shaped like the loopback corpus: f16 RGBA colour (R high, B low), f32 depth, f16 motion."""
    directory.mkdir(parents=True)
    frames = []
    for i in range(N):
        colour = struct.pack(f'<{W * H * 4}e', *[v for p in range(W * H)
                                                for v in (0.25 + (p % W) / 40.0, 0.5, 0.02 * (i + 1), 1.0)])
        depth = struct.pack(f'<{W * H}f', *[(p % W) / W * 0.5 + (p // W) / H * 0.25 + 0.01 * i for p in range(W * H)])
        motion = struct.pack(f'<{W * H * 2}e', *[v for p in range(W * H)
                                                for v in (0.25 * (p % W) - 1.5 + i, -0.125 * (p // W))])
        entry = dict(id=i, present=i, time_ms=i * 8.0, reset=i == 0, score=i >= 2, jitter=[0, 0], pre_exposure=1.0,
                     exposure=None, exposure_value=1.0, completion='fence-complete',
                     params=dict(create_flags=65, mv_scale=[1.0, 1.0], frame_time_ms=8.0))
        for key, data, dtype, channels in (('color', colour, 'f16', 4), ('depth', depth, 'f32', 1),
                                           ('motion', motion, 'f16', 2)):
            name = f'f{i}.{key}'
            (directory / name).write_bytes(data)
            item = 4 if dtype == 'f32' else 2
            entry[key] = dict(path=name, encoding='raw', dtype=dtype, width=W, height=H, channels=channels,
                              row_pitch=W * channels * item, sha256=transform.digest(directory / name))
        frames.append(entry)
    capture = dict(schema='retoque-replay-capture', version=1, complete=True, mode='nr-boundary', source='synthetic',
                   clock='recorded-evaluation', color_space='linear-rec709', color_jitter='removed',
                   depth=dict(kind='device', inverted=False, threshold=0.02),
                   motion=dict(direction='current-to-previous', units='pixels', scale=[1.0, 1.0],
                               includes_jitter=False),
                   regions={}, provenance={}, frames=frames)
    path = directory / 'capture.json'
    path.write_text(json.dumps(capture))
    return path


def run_tool(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = transform.main([str(a) for a in argv])
    assert code == 0
    return Path(out.getvalue().strip().splitlines()[-1])


def plane_bytes(capture_path, frame, key):
    return (capture_path.parent / frame[key]['path']).read_bytes()


class Pad(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nr-replay-transform-')
        self.root = Path(self.tmp.name)
        self.source = synthetic(self.root / 'source')
        self.capture = json.loads(self.source.read_text())

    def tearDown(self):
        self.tmp.cleanup()

    def derive(self, *extra):
        return run_tool('pad', self.source, '--out', self.root / 'pad', '--depth-base', 3, 2, '--motion-base', 5, 1,
                        '--pad', 4, 3, *extra)

    def test_region_moved_exactly_and_margin_is_sentinel(self):
        path = self.derive()
        derived = json.loads(path.read_text())
        for src, dst in zip(self.capture['frames'], derived['frames']):
            self.assertEqual(dst['params']['render_subrect'], [W, H])
            self.assertEqual((dst['params']['depth_base'], dst['params']['motion_base']), ([3, 2], [5, 1]))
            self.assertEqual(dst['params']['mv_scale'], [1.0, 1.0])
            self.assertEqual(plane_bytes(path, dst, 'color'), plane_bytes(self.source, src, 'color'))
            for key, (bx, by), sentinel in (('depth', (3, 2), struct.pack('<f', 3.0e38)),
                                            ('motion', (5, 1), struct.pack('<HH', 0x7BFF, 0x7BFF))):
                p = dst[key]
                self.assertEqual((p['width'], p['height']), (bx + W + 4, by + H + 3))
                data, original = plane_bytes(path, dst, key), plane_bytes(self.source, src, key)
                pixel = p['pixel_bytes']
                inside = 0
                for y in range(p['height']):
                    for x in range(p['width']):
                        got = data[(y * p['width'] + x) * pixel:(y * p['width'] + x + 1) * pixel]
                        if bx <= x < bx + W and by <= y < by + H:
                            want = original[((y - by) * W + x - bx) * pixel:((y - by) * W + x - bx + 1) * pixel]
                            self.assertEqual(got, want, f'{key} ({x},{y})')
                            inside += 1
                        else:
                            self.assertEqual(got, sentinel, f'{key} margin ({x},{y})')
                self.assertEqual(inside, W * H)

    def test_the_seam_takes_it_and_plans_the_bases(self):
        path = self.derive('--sentinel', 'nan', '--units', '2')
        capture = run_replay.load_capture(path)
        plan = self.root / 'plan.txt'
        run_replay.write_plan(capture, path.parent, plan)
        lines = plan.read_text().splitlines()
        self.assertTrue(lines[0].startswith('replay-plan 3 '))
        self.assertIn(f'plane depth 41 41 0 {3 + W + 4} {2 + H + 3} 4', lines)
        frame = lines[4].split()
        self.assertEqual(frame[10:18], ['0.5', '0.5', str(W), str(H), '3', '2', '5', '1'])

    def test_units_compensated_exactly(self):
        for units in ('2', '-1', '0.25'):
            target = self.root / f'pad{units}'
            path = run_tool('pad', self.source, '--out', target, '--depth-base', 0, 0, '--motion-base', 0, 0, '--pad',
                            0, 0, '--units', units)
            derived = json.loads(path.read_text())
            k = float(units)
            for src, dst in zip(self.capture['frames'], derived['frames']):
                self.assertEqual(dst['params']['mv_scale'], [1.0 / k, 1.0 / k])
                stored = struct.unpack(f'<{W * H * 2}e', plane_bytes(path, dst, 'motion'))
                original = struct.unpack(f'<{W * H * 2}e', plane_bytes(self.source, src, 'motion'))
                # What the model computes, stored x scale, is the source vector bit for bit.
                self.assertEqual([struct.pack('<f', s * (1.0 / k)) for s in stored],
                                 [struct.pack('<f', o) for o in original])

    def test_inexact_units_are_refused(self):
        with self.assertRaises(SystemExit):
            self.derive('--units', '3')
        # A subnormal halved loses its last bit: the tool must refuse rather than write a lossy "exact" capture.
        frame = self.capture['frames'][0]
        motion = bytearray(plane_bytes(self.source, frame, 'motion'))
        motion[0:2] = struct.pack('<H', 0x0001)
        (self.source.parent / frame['motion']['path']).write_bytes(bytes(motion))
        for backend in ('python', 'auto'):
            transform.BACKEND = backend
            with self.assertRaises(SystemExit):
                transform.scale_motion(bytes(motion), 34, 0.5)
        transform.BACKEND = 'auto'

    def test_mv_low_res_toggle_at_one_to_one(self):
        path = run_tool('pad', self.source, '--out', self.root / 'low', '--mv-low-res', 'on')
        derived = json.loads(path.read_text())
        self.assertTrue(all(f['params']['create_flags'] & 2 for f in derived['frames']))
        run_replay.load_capture(path)


class Colour8(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nr-replay-transform-')
        self.root = Path(self.tmp.name)
        self.source = synthetic(self.root / 'source')

    def tearDown(self):
        self.tmp.cleanup()

    def derive(self, order, *extra):
        return run_tool('colour8', self.source, '--out', self.root / order, '--order', order, '--swap-every', 2,
                        '--swap-start', 1, '--reset-on-swap', 1, *extra)

    def test_twins_carry_the_same_colour(self):
        rgba, bgra = self.derive('rgba'), self.derive('bgra')
        a, b = json.loads(rgba.read_text()), json.loads(bgra.read_text())
        for x, y in zip(a['frames'], b['frames']):
            self.assertEqual((x['color']['dxgi_format'], y['color']['dxgi_format']), (28, 87))
            self.assertEqual((x['color']['source_format'], y['color']['source_format']), (28, 87))
            ca, cb = plane_bytes(rgba, x, 'color'), plane_bytes(bgra, y, 'color')
            self.assertNotEqual(ca, cb)
            self.assertEqual(transform.reorder(cb), ca)
            self.assertEqual(x['reset'], y['reset'])
            self.assertEqual(plane_bytes(rgba, x, 'motion'), plane_bytes(bgra, y, 'motion'))
            alpha = set(ca[3::4])
            self.assertGreater(len(alpha), 10, 'alpha must vary')
        self.assertGreater(json.loads((rgba.parent / 'transform.json').read_text())['mean_abs_r_minus_b'], 40)
        # The seam replays a whole capture in either order today.
        run_replay.load_capture(rgba)
        run_replay.load_capture(bgra)

    def test_alternating_schedule_and_reset_on_a_swap(self):
        alt = self.derive('alternate')
        t = json.loads((alt.parent / 'transform.json').read_text())
        frames = json.loads(alt.read_text())['frames']
        formats = [f['color']['dxgi_format'] for f in frames]
        self.assertEqual(t['swap_frames'], [1, 3, 5])
        self.assertEqual(formats, [28, 87, 87, 28, 28, 87])
        self.assertEqual(t['reset_frames'], [3])  # frame 1 is not scored, frame 3 is the first scored swap
        self.assertEqual([f['id'] for f in frames if f['reset']], [0, 3])
        self.assertFalse(t['seam_supported'])
        # The all-RGBA reference of the same logical sequence has its reset on the same frame.
        rgba = json.loads(self.derive('rgba').read_text())['frames']
        self.assertEqual([f['id'] for f in rgba if f['reset']], [0, 3])
        for f, g in zip(frames, rgba):
            c = plane_bytes(alt, f, 'color')
            self.assertEqual(transform.reorder(c) if f['color']['dxgi_format'] == 87 else c,
                             plane_bytes(alt.parent.parent / 'rgba' / 'capture.json', g, 'color'))
        # What the seam lacks today: one colour format per capture (README, "colour8"). Reported, not pinned.
        try:
            run_replay.load_capture(alt)
            print('NOTE: run_replay.load_capture now accepts a per-frame colour format')
        except SystemExit as refusal:
            print(f'NOTE: the seam refuses the alternating capture: {refusal}')

    def test_backends_agree(self):
        if transform.numpy is None:
            self.skipTest('NumPy absent')
        frame = json.loads(self.source.read_text())['frames'][2]
        data = plane_bytes(self.source, frame, 'color')
        motion = plane_bytes(self.source, frame, 'motion')
        results = {}
        for backend in ('python', 'numpy'):
            transform.BACKEND = backend
            results[backend] = (transform.quantise(data, 10, W, H, (1.0, 0.75, 0.2), 'pattern', 2),
                                transform.quantise(data, 10, W, H, (1.0, 0.75, 0.2), 'source', 2),
                                transform.scale_motion(motion, 34, 2.0), transform.scale_motion(motion, 34, -1.0))
        transform.BACKEND = 'auto'
        self.assertEqual(results['python'], results['numpy'])


class Compare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nr-replay-transform-')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_dir(self, name, outputs, fmt, decisions=None):
        directory = self.root / name
        directory.mkdir()
        frames = []
        for i, data in enumerate(outputs):
            path = directory / f'f{i}.output'
            path.write_bytes(data)
            frames.append(dict(id=i, kind='every', interval=1, requested_passes=1, completed_passes=1,
                               model_result=1, skip=None,
                               output=dict(path=path.name, encoding='raw-dxgi', dxgi_format=fmt, width=2, height=1,
                                           pixel_bytes=4, row_pitch=8, sha256=transform.digest(path))))
        if decisions:
            frames[decisions]['interval'] = 2
        (directory / 'run.json').write_text(json.dumps(dict(settings={'Cadence': '1'}, frames=frames)))
        return directory

    def test_channel_order_is_normalised_and_a_missed_reorder_is_caught(self):
        rgba = [bytes([200, 10, 30, 255, 180, 20, 5, 128]), bytes([90, 90, 10, 1, 250, 0, 0, 0])]
        a = self.run_dir('a', rgba, 28)
        good = self.run_dir('good', [transform.reorder(x) for x in rgba], 87)
        missed = self.run_dir('missed', rgba, 87)  # RGBA bytes labelled BGRA: R and B exchanged as read
        decided = self.run_dir('decided', rgba, 28, decisions=1)
        self.assertTrue(transform.compare_runs(a, good)['ok'])
        bad = transform.compare_runs(a, missed)
        self.assertFalse(bad['ok'])
        self.assertEqual(bad['pixels_differ'], [0, 1])
        self.assertEqual(bad['worst_unorm8_step'], 250)
        self.assertEqual(transform.compare_runs(a, decided)['decisions_differ'], [1])


if __name__ == '__main__':
    result = unittest.main(verbosity=1, exit=False).result
    if result.wasSuccessful():
        print(f'PASS: nr-replay-transform, {result.testsRun} cases')
    sys.exit(0 if result.wasSuccessful() else 1)
