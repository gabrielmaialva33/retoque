#!/usr/bin/env python3
"""Host-only matrix, setter and pixel-analysis regression tests."""
import contextlib
import importlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
LOOP = HERE.parent / 'dlssnr-loopback'
sys.path.insert(0, str(LOOP))
import analyze_mask as analyzer
import run as runner


class MatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.matrix = importlib.import_module('mask_matrix') if (LOOP / 'mask_matrix.py').exists() else None

    def require_matrix(self):
        self.assertIsNotNone(self.matrix, 'full matrix module is not implemented')
        return self.matrix

    def test_complete_product_and_cpp_plan_agree(self):
        matrix = self.require_matrix()
        rows = matrix.trials()
        self.assertEqual(len(rows), 184)  # 2 setters * 4 modes * (4 arms * 5 levels + 3 null controls)
        self.assertEqual(len({r['name'] for r in rows}), 184)
        masked = [r for r in rows if r['masked']]
        self.assertEqual(len(masked), 160)
        for setter in ('u64', 'resource'):
            for mode in range(4):
                group = [r for r in rows if r['setter'] == setter and r['blend'] == mode]
                self.assertEqual([r['arm'] for r in group[:2]], ['null-r0', 'null-r1'])
                self.assertEqual(group[-1]['arm'], 'null-after')
                for arm in ('r8', 'r001', 'r111', 'rgb'):
                    values = [r for r in group if r['arm'] == arm]
                    self.assertEqual([r['rgba'][0] for r in values], [0, 64, 128, 191, 255])
                    for row in values:
                        value = row['rgba'][0]
                        expected = [value, 0, 0, 255] if arm in ('r8', 'r001') else (
                            [value, 255, 255, 255] if arm == 'r111' else [value, value, value, 255])
                        self.assertEqual(row['rgba'], expected)
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'matrix'
            subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(HERE / 'stubs'),
                            str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
            actual = subprocess.check_output([str(binary)], text=True).splitlines()
        expected = ['\t'.join(map(str, [r['name'], int(r['masked']), int(r['arm'] == 'r8'),
                                      int(r['setter'] == 'resource'), r['blend'], *r['rgba']])) for r in rows]
        self.assertEqual(actual, expected)

    def test_gpu_orchestration_slice_uses_every_trial_and_32_frames(self):
        self.require_matrix()
        source = (LOOP / 'present_nr.h').read_text()
        start = source.index('        else if (maskMatrix)\n        {')
        end = source.index('        else if (maskAb)\n        {', start)
        body = source[start:end].split('\n', 1)[1]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'full_branch.inc').write_text(body)
            binary = root / 'host'
            subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(HERE / 'stubs'), '-I' + str(root),
                            str(HERE / 'host.cpp'), '-o', str(binary)], check=True)
            self.assertEqual(subprocess.check_output([str(binary)], text=True).strip(), '184 5888')

    def test_runner_legacy_and_full_coverage_are_disjoint(self):
        matrix = self.require_matrix()
        legacy = 'MASK-AB PASS: trials=24 attempts=768 successes=768 presents=768 controls=24 capture_pairs=168'
        full = 'MASK-MATRIX PASS: trials=184 attempts=5888 successes=5888 presents=5888 controls=184 capture_pairs=1288'
        matrix.check_coverage(legacy, full=False)
        matrix.check_coverage(full, full=True)
        for text in (legacy, full.replace('successes=5888', 'successes=5887'), full + '\n' + full, ''):
            with self.assertRaises(ValueError):
                matrix.check_coverage(text, full=True)
        with self.assertRaises(ValueError):
            matrix.check_coverage(full, full=False)
        args = runner.parse_args(['--present-nr', '--mask-matrix', '--skip-build', '--preset', '1'])
        self.assertTrue(args.mask_matrix)
        self.assertFalse(args.mask_ab)
        self.assertTrue(runner.parse_args(['--present-nr', '--mask-ab']).mask_ab)
        for argv in (['--mask-matrix'], ['--present-nr', '--mask-matrix', '--mask-ab']):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                runner.parse_args(argv)

    def fixture(self, root):
        matrix = self.require_matrix()
        analyzer.WIDTH, analyzer.HEIGHT = 2, 1
        self.addCleanup(setattr, analyzer, 'WIDTH', 1280)
        self.addCleanup(setattr, analyzer, 'HEIGHT', 720)
        def write(name, data):
            (root / name).write_bytes(b'P6\n2 1\n255\n' + bytes(data))
        write('mask-hud-cover.ppm', [255, 255, 255, 0, 0, 0])
        (root / 'mask-model.json').write_text(json.dumps({'model': 'synthetic.dll', 'bytes': 1,
                                                        'sha256': 'a' * 64, 'preset': 0}))
        for row in matrix.trials():
            for frame in (0, *analyzer.FRAMES):
                before = [100] * 6
                # Synthetic signal: all unmasked frames change by four; masks scale it by R.
                change = round(4 * row['rgba'][0] / 255) if row['masked'] else 4
                after = [100 + (change if frame else 0)] * 6
                for side, data in (('before', before), ('after', after)):
                    write(f'{row["name"]}-f{frame}-{side}.ppm', data)
        return write

    def test_analyzer_reports_measured_mask_and_setter_pairs(self):
        self.require_matrix()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write = self.fixture(root)
            write('resource-m1-r001-v128-f8-after.ppm', [103] * 6)
            with contextlib.redirect_stdout(io.StringIO()):
                analyzer.main(root, matrix=True)
            report = json.loads((root / 'mask-report.json').read_text())
            self.assertEqual(report['matrix'], 'full-v1')
            self.assertEqual(len(report['source']), 184)
            self.assertTrue(report['acceptance']['controls']['met'])
            self.assertEqual(report['source']['u64-m0-r8-v128']['1']['hud']['mae'], 2)
            self.assertEqual(report['source']['resource-m3-r111-v0']['31']['rest']['max'], 0)
            overload = [p for p in report['pairs'] if p['kind'] == 'setter']
            self.assertEqual(len(overload), 80)
            self.assertTrue(all(p['frames']['1']['hud']['max'] == 0 for p in overload))
            changed = next(p for p in overload if p['b'] == 'resource-m1-r001-v128')
            self.assertEqual(changed['frames']['8']['hud']['mae'], 1)
            self.assertEqual(report['edit_mae_over_unmasked']['resource-m1-r001-v128']['8']['hud'], 0.75)

    def test_legacy_analysis_retains_24_cases(self):
        self.assertEqual(len(analyzer.TRIALS), 24)
        analyzer.WIDTH, analyzer.HEIGHT = 2, 1
        self.addCleanup(setattr, analyzer, 'WIDTH', 1280)
        self.addCleanup(setattr, analyzer, 'HEIGHT', 720)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def write(name, values):
                (root / name).write_bytes(b'P6\n2 1\n255\n' + bytes(values))
            write('mask-hud-cover.ppm', [255, 255, 255, 0, 0, 0])
            for trial in analyzer.TRIALS:
                for frame in analyzer.FRAMES:
                    write(f'{trial}-f{frame}-before.ppm', [100] * 6)
                    write(f'{trial}-f{frame}-after.ppm', [104] * 6)
            with contextlib.redirect_stdout(io.StringIO()):
                analyzer.main(root)
            report = json.loads((root / 'mask-report.json').read_text())
            self.assertEqual(len(report['source']), 24)
            self.assertNotIn('matrix', report)
            self.assertTrue(report['acceptance']['repeat_floor']['met'])
            self.assertFalse(report['acceptance']['r0_restores_source']['met'])

    def test_analyzer_refuses_incomplete_unstable_or_inert_evidence(self):
        self.require_matrix()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write = self.fixture(root)
            mutations = [
                ('u64-m0-r8-v64-f8-after.ppm', None),
                ('u64-m0-r8-v64-f8-after.ppm', [100]),  # truncated capture
                ('u64-m0-r8-v64-f8-before.ppm', [101] * 6),
                ('u64-m0-r8-v64-f0-after.ppm', [101] * 6),
                ('u64-m0-null-r1-f8-after.ppm', [105] * 6),
                ('u64-m0-null-after-f8-after.ppm', [105] * 6),
                ('u64-m0-null-r0-f8-after.ppm', [100] * 6),
                ('mask-hud-cover.ppm', [0] * 6),
                ('mask-model.json', None),
            ]
            for name, data in mutations:
                with self.subTest(name=name):
                    path = root / name
                    original = path.read_bytes()
                    if data is None:
                        path.unlink()
                    else:
                        write(name, data)
                    (root / 'mask-report.json').write_text('{"stale":true}')
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaises((RuntimeError, FileNotFoundError)):
                        analyzer.main(root, matrix=True)
                    self.assertFalse((root / 'mask-report.json').exists())
                    path.write_bytes(original)


if __name__ == '__main__':
    unittest.main()
