#!/usr/bin/env python3
"""Host checks for run_replay.py's capture loading and plan v3 (docs/dlssnr/design/offline-replay-seam.md, Plan v3).

The harness parses the plan with fscanf, so the plan's shape is checked here token for token: a v1 capture must give
the v1 surfaces and a full-extent, unscaled geometry (which is what keeps the baselines bit-exact), and a v2 capture
must carry its formats, source formats, flags, extents and per-frame geometry. Every refusal names its reason."""
import hashlib
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_replay  # noqa: E402

FRAME_TOKENS = 21  # replay_nr.h LoadPlan: 18 numbers and three paths


def write(directory, name, size):
    path = directory / name
    path.write_bytes(bytes((i * 7) & 0xFF for i in range(size)))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def v1_capture(directory, frames=3, w=32, h=16):
    out = []
    for i in range(1, frames + 1):
        planes = {}
        for key, dtype, channels, item in (('color', 'f16', 4, 2), ('depth', 'f32', 1, 4), ('motion', 'f16', 2, 2)):
            name = f'f{i}.{key}'
            planes[key] = dict(path=name, encoding='raw', dtype=dtype, width=w, height=h, channels=channels,
                               row_pitch=w * channels * item, sha256=write(directory, name, w * h * channels * item))
        out.append(dict(id=i, present=100 + i, time_ms=i * 10.0, reset=i == 1, score=i > 1, jitter=[0.25, -0.25],
                        pre_exposure=1.0, exposure=None, params=dict(create_flags=run_replay.CREATE_FLAGS,
                                                                     mv_scale=[1.0, 1.0], frame_time_ms=10.0),
                        **planes))
    capture = dict(schema='retoque-replay-capture', version=1, complete=True, mode='nr-boundary',
                   clock='recorded-evaluation', color_jitter='removed',
                   motion=dict(direction='current-to-previous', units='pixels', scale=[1.0, 1.0],
                               includes_jitter=False), frames=out)
    return save(directory, capture)


def v2_capture(directory, frames=3):
    shapes = dict(color=(10, 9, 4, 48, 32, 8), depth=(41, 39, 0, 32, 24, 4), motion=(34, 34, 0, 32, 24, 4))
    out = []
    for i in range(1, frames + 1):
        planes = {}
        for key, (fmt, source, flags, w, h, item) in shapes.items():
            name = f'f{i}.{key}'
            planes[key] = dict(path=name, encoding='raw-dxgi', dxgi_format=fmt, width=w, height=h, pixel_bytes=item,
                               row_pitch=w * item, source_format=source, source_flags=flags,
                               sha256=write(directory, name, w * h * item))
        out.append(dict(id=i, present=200 + i * 2, time_ms=i * 16.5, reset=i == 1, score=i > 1, jitter=[0.5, 0.0],
                        pre_exposure=2.0, exposure=None, exposure_value=1, completion='fence-complete',
                        params=dict(create_flags=1 | run_replay.MV_LOW_RES, ray_reconstruction=True,
                                    mv_scale=[0.5, 0.5], frame_time_ms=16.5, render_subrect=[24, 16],
                                    depth_base=[2, 3], motion_base=[4, 4]), **planes))
    capture = dict(schema='retoque-replay-capture', version=2, complete=True, finalized=True, mode='nr-boundary',
                   source='game:Test', clock='recorded-evaluation', color_space='linear-rec709',
                   color_jitter='removed', depth=dict(kind='device', inverted=False, threshold=0.02),
                   motion=dict(direction='current-to-previous', units='pixels', scale='per-frame',
                               includes_jitter=False), regions={}, provenance={}, frames=out)
    return save(directory, capture)


def save(directory, capture):
    path = directory / 'capture.json'
    path.write_text(json.dumps(capture))
    return path


def plan_of(capture_path):
    capture = run_replay.load_capture(capture_path)
    plan = capture_path.parent / 'plan.txt'
    run_replay.write_plan(capture, capture_path.parent, plan)
    return capture, plan.read_text().splitlines()


class PlanV3(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.dir = Path(self.scratch.name)

    def tearDown(self):
        self.scratch.cleanup()

    def test_v1_capture_gives_the_v1_surfaces_and_full_extent_geometry(self):
        _, lines = plan_of(v1_capture(self.dir))
        self.assertEqual(lines[0], 'replay-plan 3 3')
        self.assertEqual(lines[1:4], ['plane color 10 10 4 32 16 8', 'plane depth 41 41 0 32 16 4',
                                      'plane motion 34 34 0 32 16 4'])
        for line in lines[4:]:
            tokens = line.split()
            self.assertEqual(len(tokens), FRAME_TOKENS)
            # create flags, RR, MV scale, render subrect, depth base, motion base: as the v1 harness set them.
            self.assertEqual(tokens[8:18], [str(run_replay.CREATE_FLAGS), '0', '1.0', '1.0', '32', '16', '0', '0',
                                            '0', '0'])
        self.assertEqual(lines[4].split()[:8], ['1', '101', '10.0', '10.0', '1', '0.25', '-0.25', '1.0'])

    def test_v2_capture_carries_formats_flags_and_geometry(self):
        _, lines = plan_of(v2_capture(self.dir))
        self.assertEqual(lines[1:4], ['plane color 10 9 4 48 32 8', 'plane depth 41 39 0 32 24 4',
                                      'plane motion 34 34 0 32 24 4'])
        tokens = lines[5].split()
        self.assertEqual(len(tokens), FRAME_TOKENS)
        self.assertEqual(tokens[:18], ['2', '204', '33.0', '16.5', '0', '0.5', '0.0', '2.0', '3', '1', '0.5', '0.5',
                                       '24', '16', '2', '3', '4', '4'])
        self.assertEqual(Path(tokens[18].replace('\\', '/')).name, 'f2.color')

    def test_v2_without_source_fields_uses_the_typed_format(self):
        path = v2_capture(self.dir)
        capture = json.loads(path.read_text())
        for f in capture['frames']:
            for key in ('color', 'depth', 'motion'):
                del f[key]['source_format'], f[key]['source_flags']
        save(self.dir, capture)
        _, lines = plan_of(path)
        self.assertEqual(lines[1:4], ['plane color 10 10 4 48 32 8', 'plane depth 41 41 0 32 24 4',
                                      'plane motion 34 34 0 32 24 4'])

    def refused(self, mutate, reason):
        path = v2_capture(self.dir)
        capture = json.loads(path.read_text())
        mutate(capture)
        save(self.dir, capture)
        with self.assertRaises(SystemExit) as caught:
            run_replay.load_capture(path)
        self.assertIn(reason, str(caught.exception))

    def test_refusals(self):
        cases = [
            (lambda c: c.update(finalized=False), 'not finalized'),
            (lambda c: [f['color'].update(source_flags=0) for f in c['frames']], 'unordered access'),
            (lambda c: [f['depth'].update(source_flags=8) for f in c['frames']], 'denies shader resource'),
            (lambda c: c['frames'][1]['depth'].update(width=40, row_pitch=160), 'shape changes'),
            (lambda c: [f['params'].update(render_subrect=[24, 0]) for f in c['frames']], 'half a render subrect'),
            (lambda c: [f['params'].update(depth_base=[-1, 0]) for f in c['frames']], 'integers'),
            (lambda c: c['frames'][2]['params'].update(create_flags=1), 'create flags change'),
            (lambda c: c['frames'][0]['motion'].update(sha256='0' * 64), 'digest mismatch'),
            (lambda c: [f['color'].update(pixel_bytes=4, row_pitch=192) for f in c['frames']],
             'not one the seam transports'),
            (lambda c: [f['depth'].update(dxgi_format=99) for f in c['frames']], 'not one the seam transports'),
            (lambda c: c['frames'][1].update(exposure='f2.exposure'), 'exposure textures'),
            (lambda c: c['frames'][1]['params'].update(mv_scale=[math.nan, 1.0]), 'finite'),
            (lambda c: c['frames'][1].update(present=1), 'strictly increase'),
            (lambda c: c.update(color_jitter='present'), 'color_jitter'),
        ]
        for mutate, reason in cases:
            with self.subTest(reason=reason):
                self.refused(mutate, reason)

    def test_geometry_the_pass_clamps_is_carried_as_recorded(self):
        # A base or subrect past the plane, a missing subrect, and output-size motion without MVLowRes in a smaller
        # plane: the pass bounds them all against the allocation, so the plan carries them unchanged.
        path = v2_capture(self.dir)
        capture = json.loads(path.read_text())
        for f in capture['frames']:
            f['params'].update(create_flags=1, depth_base=[30, 30])
        capture['frames'][1]['params']['render_subrect'] = [0, 0]
        save(self.dir, capture)
        _, lines = plan_of(path)
        self.assertEqual(lines[4].split()[8:18], ['1', '1', '0.5', '0.5', '24', '16', '30', '30', '4', '4'])
        self.assertEqual(lines[5].split()[12:14], ['0', '0'])

    def test_halfres_capture_keeps_the_corpus_provenance(self):
        corpus = v1_capture(self.dir, w=64, h=32)
        capture = json.loads(corpus.read_text())
        capture['provenance'] = dict(producer='abc+def', shader_sha256='0' * 64)
        save(self.dir, capture)
        derived = run_replay.halfres_capture(self.dir / 'root', corpus)
        loaded = run_replay.load_capture(derived)
        self.assertEqual(loaded['provenance']['producer'], 'abc+def')
        self.assertEqual(loaded['provenance']['shader_sha256'], '0' * 64)
        self.assertEqual(loaded['provenance']['derived_from'], str(corpus))
        self.assertEqual(loaded['frames'][0]['params']['render_subrect'], [32, 16])

    def test_run_output_descriptor(self):
        shape = dict(format=10, width=32, height=16, pixel_bytes=8)
        (self.dir / 'f1.output').write_bytes(bytes(32 * 16 * 8))
        out = run_replay.raw_dxgi_plane(self.dir, 'f1.output', shape)
        self.assertEqual({k: out[k] for k in ('encoding', 'dxgi_format', 'width', 'height', 'pixel_bytes',
                                              'row_pitch')},
                         dict(encoding='raw-dxgi', dxgi_format=10, width=32, height=16, pixel_bytes=8, row_pitch=256))
        (self.dir / 'f2.output').write_bytes(bytes(10))
        with self.assertRaises(SystemExit):
            run_replay.raw_dxgi_plane(self.dir, 'f2.output', shape)


if __name__ == '__main__':
    unittest.main()
