"""Temporal replay plans, actual portable parser and numerical-policy transport; no GPU."""
import copy
import contextlib
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / 'tests/dlssnr-loopback'
sys.path[:0] = [str(HERE), str(ROOT), str(ROOT / 'tests/_support')]
import run_replay
from replay_fixture import corpus, run as fixture_run


def quality(test):
    path = HERE / 'replay_quality.py'
    test.assertTrue(path.is_file(), 'temporal profile helper is not implemented')
    spec = importlib.util.spec_from_file_location('quality_under_test', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def manifest_for(rows):
    identity = [float(i % 5 == 0) for i in range(16)]
    def translation(x):
        matrix = list(identity)
        matrix[12] = x
        return matrix
    def halton(index, base):
        result, factor = 0., 1.
        while index:
            factor /= base
            result += factor * (index % base)
            index //= base
        return result
    return dict(width=640, height=360, frames=480, measured=480, moving_plan_version=1,
                samples=[dict(frame=f['frame'], pose_frame=f['pose_frame'],
                              previous_pose_frame=f['previous_pose_frame'], reset=f['reset'],
                              time_ms=f['time_ms'], frame_time_ms=f['frame_time_ms'],
                              material_frame=float(f['pose_frame']), gpu_ms=1.,
                              jitter=[halton(f['frame'] % 32 + 1, b) - .5 for b in (2, 3)],
                              inverse=translation(-f['pose_frame'] * .001),
                              previous=translation(f['previous_pose_frame'] * .001)) for f in rows])


class TemporalProfile(unittest.TestCase):
    def test_default_and_explicit_selection_do_not_relabel_short_or_custom_captures(self):
        q = quality(self)
        self.assertIsNone(q.select_profile(None, {}))
        self.assertIsNone(q.select_profile(None, {'LOOPBACK_REPLAY_CAPTURE': 'short.json'}))
        self.assertEqual(q.select_profile('temporal-v1', {}), 'temporal-v1')
        self.assertEqual((run_replay.CORPUS_FRAMES, run_replay.CORPUS_WARMUP), (256, 160))
        self.assertAlmostEqual(run_replay.FRAME_MS, 1000 / 120)
        for profile, env, reason in (
            ('unknown', {}, 'profile'),
            ('temporal-v1', {'LOOPBACK_REPLAY_CAPTURE': 'short.json'}, 'CAPTURE'),
            ('temporal-v1', {'LOOPBACK_REPLAY_FRAMES': '256'}, 'FRAMES'),
            ('temporal-v1', {'LOOPBACK_REPLAY_WARMUP': '160'}, 'WARMUP'),
            ('temporal-v1', {'LOOPBACK_REPLAY_FRAMES': 'not-a-number'}, 'FRAMES'),
        ):
            with self.subTest(profile=profile, env=env), self.assertRaisesRegex(ValueError, reason):
                q.select_profile(profile, env)
        self.assertEqual(q.select_profile('temporal-v1', {
            'LOOPBACK_REPLAY_FRAMES': '480', 'LOOPBACK_REPLAY_WARMUP': '80'}), 'temporal-v1')

    def test_recorder_check_is_rejected_before_any_temporal_run_mutation(self):
        q = quality(self)
        self.assertIsNone(q.select_profile(None, {'LOOPBACK_REPLAY_RECORD_CHECK': '1'}))
        self.assertEqual(q.select_profile('temporal-v1', {'LOOPBACK_REPLAY_RECORD_CHECK': '0'}), 'temporal-v1')
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            runner = types.SimpleNamespace(RUN=root, HERE=HERE, ROOT=ROOT)
            with patch.dict(os.environ, {'LOOPBACK_REPLAY_RECORD_CHECK': '1'}, clear=True), \
                    patch.object(run_replay, 'producer', side_effect=AssertionError('must reject before producer')):
                with self.assertRaisesRegex(ValueError, 'LOOPBACK_REPLAY_RECORD_CHECK'):
                    run_replay.run(runner, Path('/not-executed/proton'), {}, None, profile='temporal-v1')
            self.assertEqual(list(root.iterdir()), [])

    def test_continuous_real_pose_schedule_and_scored_cut(self):
        rows = quality(self).temporal_frames()
        self.assertEqual([r['frame'] for r in rows], list(range(480)))
        self.assertEqual([r['score'] for r in rows], [False] * 80 + [True] * 400)
        self.assertEqual([r['frame'] for r in rows if r['reset']], [0, 360])
        for phase, first, stop in (
            ('warmup', 0, 80), ('steady', 80, 160), ('motion', 160, 240),
            ('recovery', 240, 320), ('fast-motion', 320, 360), ('cut', 360, 361),
            ('post-cut-recovery', 361, 480),
        ):
            self.assertEqual([r['phase'] for r in rows[first:stop]], [phase] * (stop-first))
        for row in rows:
            i = row['frame']
            self.assertAlmostEqual(row['time_ms'], i * 1000 / 60, places=5)
            self.assertAlmostEqual(row['frame_time_ms'], 1000 / 60)
            self.assertEqual(row['previous_pose_frame'],
                             row['pose_frame'] if row['reset'] else rows[i-1]['pose_frame'])
        for first, stop in ((0, 160), (240, 320), (361, 480)):
            self.assertEqual(len({r['pose_frame'] for r in rows[first:stop]}), 1)
        for first, stop, increment in ((160, 240, 2), (320, 360, 12)):
            self.assertEqual([rows[i]['pose_frame'] - rows[i-1]['pose_frame']
                              for i in range(first, stop)], [increment] * (stop-first))
        self.assertEqual(rows[240]['pose_frame'], rows[239]['pose_frame'])
        self.assertNotEqual(rows[360]['pose_frame'], rows[359]['pose_frame'])
        self.assertEqual(rows[361]['pose_frame'], rows[360]['pose_frame'])
        self.assertTrue(rows[360]['score'])

    def test_metadata_cannot_mask_wrong_geometry_material_timing_or_resets(self):
        q = quality(self)
        rows = q.temporal_frames()
        good = manifest_for(rows)
        q.validate_samples(good, rows)
        cases = (
            (lambda m: m.update(width=320), 'extent'),
            (lambda m: m['samples'].pop(), 'frames'),
            (lambda m: m['samples'][160].update(previous_pose_frame=159), 'previous_pose'),
            (lambda m: m['samples'][80].update(material_frame=80), 'material'),
            (lambda m: m['samples'][360].update(reset=False), 'reset'),
            (lambda m: m['samples'][360].update(time_ms=0), 'time_ms'),
            (lambda m: m['samples'][360].update(frame_time_ms=math.nan), 'frame_time'),
            (lambda m: m['samples'][80].update(jitter=[0, 0]), 'jitter'),
            (lambda m: m['samples'][160]['previous'].__setitem__(12, .5), 'previous matrix'),
            (lambda m: m['samples'][360]['previous'].__setitem__(12, .5), 'previous matrix'),
            (lambda m: m['samples'][80]['inverse'].__setitem__(0, math.inf), 'finite'),
            (lambda m: [s.update(inverse=[float(i % 5 == 0) for i in range(16)],
                                 previous=[float(i % 5 == 0) for i in range(16)])
                        for s in m['samples']], 'moving pose'),
        )
        for mutate, reason in cases:
            bad = copy.deepcopy(good)
            mutate(bad)
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                q.validate_samples(bad, rows)

    def test_phase_evidence_preserves_skip_and_does_not_invent_recovery(self):
        q = quality(self)
        rows = q.temporal_frames()
        capture = dict(frames=[dict(r, id=r['frame'] + 1) for r in rows])
        entries = [dict(id=i+1, kind='every', interval=1) for i in range(480)]
        entries[360].update(kind='skipped', interval=0)
        entries[400].update(kind='carried', interval=4)
        observations = q.phase_observations(capture, {'candidate': dict(frames=entries)})
        cut = observations['candidate']['cut']
        self.assertEqual(cut['counts'], dict(model=0, carried=0, skipped=1))
        self.assertEqual(cut['intervals'], {'0': 1})
        self.assertEqual(observations['candidate']['post-cut-recovery']['counts'],
                         dict(model=118, carried=1, skipped=0))
        self.assertEqual(observations['candidate']['post-cut-recovery']['intervals'], {'1': 118, '4': 1})

    def test_scored_reset_requires_a_complete_model_evaluation_for_cadence_too(self):
        q = quality(self)
        self.assertTrue(callable(getattr(q, 'reset_gates', None)), 'scored reset coverage gate is missing')
        capture = dict(frames=[dict(r, id=r['frame'] + 1) for r in q.temporal_frames()])
        entries = [dict(id=i+1, kind='model', model_result=1, requested_passes=1, completed_passes=1)
                   for i in range(480)]
        runs = {'cadence4': dict(settings={'Enabled': True}, frames=entries)}
        self.assertTrue(all(r['ok'] for r in q.reset_gates(capture, runs).values()))
        for update in (dict(kind='carried'), dict(kind='skipped'), dict(completed_passes=0), dict(model_result=0)):
            altered = copy.deepcopy(runs)
            altered['cadence4']['frames'][360].update(update)
            with self.subTest(update=update):
                gate = q.reset_gates(capture, altered)['cadence4-scored-reset-runs-model']
                self.assertFalse(gate['ok'])
                self.assertIn('361', gate['detail'])

    def test_corpus_builder_transports_actual_reset_clock_and_plan_to_replay(self):
        q = quality(self)
        # Only GPU execution is replaced. Real-sized files share inodes to keep this host check small;
        # the real descriptor hashing, capture loader and replay-plan writer consume them.
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            for name in run_replay.STAGED:
                (root / name).write_bytes(b'host fixture')
            def recorded(command, *, cwd, env, log, timeout):
                self.assertEqual(command[-4:], ['--moving-corpus', '480', '--moving-plan', 'moving-plan.txt'])
                self.assertEqual((cwd / 'moving-plan.txt').read_text().splitlines()[361].split()[:4],
                                 ['360', '32', '32', '1'])
                (cwd / 'moving.json').write_text(json.dumps(manifest_for(q.temporal_frames())))
                (cwd / 'dlssnr-loopback.log').write_text('MOVING-SCENE PASS: evaluations=480 captures=480\n')
                for suffix, size in (('output', 640*360*8), ('depth', 640*360*4), ('motion', 640*360*4)):
                    shared = cwd / ('shared.' + suffix)
                    shared.write_bytes(bytes(size))
                    for i in range(480):
                        os.link(shared, cwd / f'f{i}.{suffix}')
                return 0
            with patch.object(run_replay, 'execute', side_effect=recorded), contextlib.redirect_stdout(io.StringIO()):
                path = run_replay.make_corpus(types.SimpleNamespace(HERE=HERE), Path('/host/proton'), {}, root,
                                              dict(producer='host-only'), profile='temporal-v1')
            capture = run_replay.load_capture(path)
            self.assertEqual(sum(f['score'] for f in capture['frames']), 400)
            self.assertEqual([f['id'] for f in capture['frames'] if f['reset']], [1, 361])
            cut = capture['frames'][360]
            self.assertTrue(cut['score'])
            self.assertEqual(cut['time_ms'], 6000)
            self.assertAlmostEqual(cut['params']['frame_time_ms'], 1000 / 60)
            self.assertEqual(capture['provenance']['profile'], 'temporal-v1')
            self.assertEqual(capture['provenance']['moving_plan_sha256'],
                             hashlib.sha256((path.parent / 'moving-plan.txt').read_bytes()).hexdigest())
            replay = root / 'replay-plan.txt'
            run_replay.write_plan(capture, path.parent, replay)
            tokens = replay.read_text().splitlines()[364].split()
            self.assertEqual(tokens[:5], ['361', '361', '6000.0', repr(1000 / 60), '1'])


class PortablePlan(unittest.TestCase):
    def test_real_cpp_reader_rejects_invalid_count_timing_history_and_bounds(self):
        q = quality(self)
        header = HERE / 'moving_plan.h'
        self.assertTrue(header.is_file(), 'portable moving-plan reader is not implemented')
        compiler = shutil.which('c++')
        if compiler is None:
            self.skipTest('portable parser needs the host C++ compiler; no GPU/compiler-prefix use')
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            source = root / 'reader.cpp'
            source.write_text('''#include "moving_plan.h"
#include <iostream>
#include <iomanip>
int main(int argc, char** argv) {
    if (argc != 2) return 3;
    try {
        auto rows = MovingPlan::Load(argv[1], 480);
        std::cout << std::setprecision(17);
        for (const auto& r : rows)
            std::cout << r.frame << " " << r.poseFrame << " " << r.previousPoseFrame
                      << " " << r.reset << " " << r.timeMs << " " << r.frameTimeMs << "\\n";
    } catch (const std::exception& e) { std::cerr << e.what(); return 2; }
}
''')
            executable = root / 'reader'
            compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                       '-I', str(HERE), str(source), '-o', str(executable)],
                                      capture_output=True, text=True, timeout=20)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            plan = root / 'moving-plan.txt'
            q.write_moving_plan(plan, q.temporal_frames())
            original = plan.read_text()
            passed = subprocess.run([str(executable), str(plan)], capture_output=True, text=True, timeout=3)
            self.assertEqual(passed.returncode, 0, passed.stderr)
            parsed = [line.split() for line in passed.stdout.splitlines()]
            self.assertEqual(len(parsed), 480)
            self.assertEqual(parsed[360][:4], ['360', '32', '32', '1'])
            self.assertAlmostEqual(float(parsed[479][4]), 479 * 1000 / 60, places=5)
            lines = original.splitlines()
            def changed(index, token, value):
                altered = list(lines)
                fields = altered[index].split()
                fields[token] = value
                altered[index] = ' '.join(fields)
                return '\n'.join(altered) + '\n'
            bad_plans = [
                original.replace('moving-plan 1 480', 'moving-plan 2 480', 1),
                original.replace('moving-plan 1 480', 'moving-plan 1 481', 1),
                original.replace('moving-plan 1 480', 'moving-plan 1 10001', 1),
                '\n'.join(lines[:-1]) + '\n', original + 'extra\n',
                changed(1, 3, '0'), changed(1, 0, '-1'), changed(1, 1, '64.5'),
                changed(2, 0, '5'), changed(2, 2, '159'), changed(2, 4, 'nan'),
                changed(2, 5, '0'), changed(2, 4, '17'), changed(361, 2, '704'),
                changed(2, 3, '2'), changed(2, 1, '1000001'), original + ' ' * (2*1024*1024),
            ]
            for i, bad in enumerate(bad_plans):
                with self.subTest(mutation=i):
                    plan.write_text(bad)
                    result = subprocess.run([str(executable), str(plan)], capture_output=True,
                                            text=True, timeout=3)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertTrue(result.stderr)


class PolicyAndReport(unittest.TestCase):
    def test_actual_calibrator_and_judge_receive_the_same_unchanged_policy(self):
        q = quality(self)
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            policy = q.write_policy(root / 'judge')
            expected = HERE / 'replay-quality-policy-v1.json'
            self.assertEqual(policy.read_bytes(), expected.read_bytes())
            values = json.loads(policy.read_text())
            self.assertEqual(values['version'], 1)
            self.assertEqual(values['min_coverage'], .25)
            self.assertEqual(values['pixel_tolerance'], .01)
            self.assertEqual(set(values['floors']), {'golden_mae', 'golden_max', 'golden_bad_fraction',
                                                     'ghost_mae', 'disocclusion_mae', 'detail_error',
                                                     'pulse_p95', 'temporal_mae'})
            self.assertEqual(set(values['floors'].values()), {0})
            clip, pixels = corpus(root / 'corpus', moving=True)
            fixture_run(root / 'runs/every-a', clip, pixels, suffix='run', salt=0)
            fixture_run(root / 'runs/every-b', clip, pixels, suffix='run', salt=10)
            repeats = [f'aa-{i}' for i in range(1, 6)]
            for i, name in enumerate(repeats, 1):
                fixture_run(root / 'runs' / name, clip, pixels, suffix='run', salt=i)
            runner = types.SimpleNamespace(ROOT=ROOT)
            with patch.object(run_replay, 'judge_python', return_value=[sys.executable]), \
                    patch.dict(os.environ, {'LOOPBACK_REPLAY_JUDGE': '1', 'RETOQUE_REPLAY_BACKEND': 'python'}):
                calibration, report = run_replay.calibrate(runner, root, clip, repeats, policy)
                self.assertIsNotNone(calibration, report)
                runs = {name: json.loads((root / 'runs' / name / 'run.json').read_text())
                        for name in ('every-a', 'every-b')}
                result = run_replay.judge(runner, root, clip, runs, calibration, policy)
            self.assertEqual(result['verdicts']['every-b']['verdict'], 'PASS', result)
            self.assertEqual(json.loads(calibration.read_text())['policy'], values)
            judged = json.loads((root / 'judge/every-b/result.json').read_text())
            self.assertEqual(judged['policy'], values)

    def test_corpus_metadata_exposes_duration_phases_reset_and_scoring(self):
        q = quality(self)
        frames = [dict(r, id=r['frame'] + 1) for r in q.temporal_frames()]
        # NGX receives float32 deltas. The nominal profile duration must still be exactly eight seconds.
        for frame in frames:
            frame['params'] = dict(frame_time_ms=16.6666660)
        capture = dict(source='loopback-moving-scene', frames=frames,
                       provenance=dict(profile='temporal-v1'))
        report = q.corpus_metadata(capture, 'temporal-v1')
        self.assertEqual(report['profile'], 'temporal-v1')
        self.assertEqual(report['frames'], 480)
        self.assertEqual(report['scored_frames'], 400)
        self.assertAlmostEqual(report['nominal_duration_ms'], 8000)
        self.assertAlmostEqual(report['timestamp_span_ms'], 479 * 1000 / 60, places=5)
        self.assertAlmostEqual(report['scored_duration_ms'], 400 * 1000 / 60)
        self.assertEqual(report['reset_frame_ids'], [1, 361])
        cut = next(p for p in report['phases'] if p['name'] == 'cut')
        self.assertEqual((cut['first_frame_id'], cut['last_frame_id'], cut['scored_frames']), (361, 361, 1))

    def test_temporal_summary_rejects_contradictory_identity_and_schedule(self):
        q = quality(self)
        frames = [dict(id=i+1, time_ms=i*1000/60, score=i >= 80, reset=i in (0, 360),
                       params={'frame_time_ms': 1000/60}) for i in range(480)]
        good = dict(source='loopback-moving-scene', provenance={'profile': 'temporal-v1'}, frames=frames)
        cases = (
            ('profile', lambda c: c['provenance'].update(profile='another-profile')),
            ('profile', lambda c: c.update(profile='another-profile')),
            ('source', lambda c: c.update(source='custom-source')),
            ('profile', lambda c: c['provenance'].update(corpus={'profile': 'another-profile'})),
            ('source', lambda c: c['provenance'].update(corpus={'source': 'custom-source'})),
            ('frame count', lambda c: c['frames'].pop()),
            ('frame id', lambda c: c['frames'][360].update(id=360)),
            ('scoring', lambda c: [f.update(score=False) for f in c['frames']]),
            ('scoring', lambda c: (c['frames'][79].update(score=True), c['frames'][80].update(score=False))),
            ('scoring', lambda c: c['frames'][100].update(score=1)),
            ('reset', lambda c: [f.update(reset=False) for f in c['frames']]),
            ('reset', lambda c: (c['frames'][360].update(reset=False), c['frames'][361].update(reset=True))),
            ('reset', lambda c: c['frames'][100].update(reset=True)),
            ('time_ms', lambda c: [f.update(time_ms=i*1000/120) for i, f in enumerate(c['frames'])]),
            ('frame_time_ms', lambda c: [f['params'].update(frame_time_ms=1000/120) for f in c['frames']]),
            ('time_ms', lambda c: c['frames'][100].update(time_ms=100*1000/60 + .01)),
            ('time_ms', lambda c: c['frames'][100].update(time_ms=math.nan)),
            ('frame_time_ms', lambda c: c['frames'][100]['params'].update(frame_time_ms=math.inf)),
            ('frame_time_ms', lambda c: c['frames'][100]['params'].update(frame_time_ms=0)),
            ('frame_time_ms', lambda c: c['frames'][100]['params'].clear()),
        )
        for reason, mutate in cases:
            bad = copy.deepcopy(good)
            mutate(bad)
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                q.corpus_metadata(bad, 'temporal-v1')
        # Legacy summaries remain descriptive; the fixed-profile contract is opt-in.
        legacy = copy.deepcopy(good)
        legacy['source'] = 'custom-source'
        self.assertEqual(q.corpus_metadata(legacy)['profile'], 'legacy')

    def test_temporal_summary_accepts_serialized_float_clocks_before_nested_metadata_exists(self):
        q = quality(self)
        capture = dict(source='loopback-moving-scene', provenance={}, frames=[
            dict(id=i+1, time_ms=float(f'{i*1000/60:.9g}'), score=i >= 80, reset=i in (0, 360),
                 params={'frame_time_ms': 16.6666660}) for i in range(480)])
        summary = q.corpus_metadata(capture, 'temporal-v1')
        self.assertEqual(summary['scored_frames'], 400)
        self.assertEqual(summary['reset_frame_ids'], [1, 361])
        self.assertAlmostEqual(summary['nominal_duration_ms'], 8000)
        capture['provenance'].update(profile='temporal-v1', corpus=summary)
        self.assertEqual(q.corpus_metadata(capture, 'temporal-v1'), summary)

    def test_functional_pass_keeps_numerical_failure_and_perceptual_unassessed(self):
        q = quality(self)
        report = q.assessments({'transport': dict(ok=True)}, dict(
            available=True, calibration=dict(verdict='CALIBRATED'),
            verdicts={'every-b': dict(verdict='PASS'), 'cadence4': dict(verdict='FAIL')}))
        self.assertEqual(report['functional'], 'PASS')
        self.assertEqual(report['numerical'], 'FAIL')
        self.assertEqual(report['perceptual'], 'NOT_ASSESSED')
        self.assertEqual(q.assessments({}, {'available': False})['numerical'], 'NOT_RUN')


class QualityEntryPoint(unittest.TestCase):
    def test_help_accepts_quality_and_explains_the_opt_in_profile(self):
        result = subprocess.run([sys.executable, str(HERE / 'run.py'), '--replay-quality', '--help'],
                                capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('temporal-v1', result.stdout)

    def test_quality_and_legacy_use_separate_lease_and_output_directories(self):
        for flag, directory in (('--replay-quality', 'replay-quality-run'), ('--replay-nr', 'replay-run')):
            code = ('import contextlib,runpy,sys\n'
                    f'sys.path.insert(0,{str(HERE)!r})\nimport run_moving\n'
                    '@contextlib.contextmanager\ndef lease(work):\n'
                    '    print(work.name, flush=True)\n    raise SystemExit(0)\n    yield\n'
                    'run_moving.lease=lease\n'
                    f'sys.argv=[{str(HERE / "run.py")!r},{flag!r}]\n'
                    f'runpy.run_path({str(HERE / "run.py")!r},run_name="__main__")\n')
            result = subprocess.run([sys.executable, '-c', code], capture_output=True, text=True, timeout=3)
            with self.subTest(flag=flag):
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), directory)

    def test_main_selects_separate_directory_and_refuses_conflicts_before_staging(self):
        spec = importlib.util.spec_from_file_location('quality_loopback_entry', HERE / 'run.py')
        entry = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(entry)
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            for flag, directory in (('--replay-quality', 'replay-quality-run'), ('--replay-nr', 'replay-run')):
                with patch.object(entry, 'OUT', root), patch.dict(os.environ, {}, clear=True), \
                        patch.object(sys, 'argv', ['run.py', flag, '--dll', str(root / 'absent.dll')]):
                    with self.assertRaisesRegex(SystemExit, 'no OptiScaler build'):
                        entry.main()
                    self.assertEqual(entry.RUN, root / directory)
            for args, env, reason in ((['--runtime', 'wine'], {}, 'requires --runtime proton'),
                                      ([], {'LOOPBACK_REPLAY_CAPTURE': 'old.json'}, 'CAPTURE'),
                                      ([], {'LOOPBACK_REPLAY_FRAMES': '256'}, 'FRAMES'),
                                      ([], {'LOOPBACK_REPLAY_RECORD_CHECK': '1'}, 'RECORD_CHECK')):
                stderr = io.StringIO()
                with patch.object(entry, 'OUT', root), patch.dict(os.environ, env, clear=True), \
                        patch.object(sys, 'argv', ['run.py', '--replay-quality', *args]), \
                        contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as error:
                        entry.main()
                self.assertEqual(error.exception.code, 2)
                self.assertIn(reason, stderr.getvalue())
            self.assertEqual(list(root.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
