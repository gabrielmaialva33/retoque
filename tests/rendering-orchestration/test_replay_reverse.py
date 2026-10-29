"""Reserved reverse trajectory and profile isolation through the real host entrypoints."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / 'tests/dlssnr-loopback'
sys.path[:0] = [str(HERE), str(ROOT), str(ROOT / 'tests/_support')]
import replay_quality as quality
import run_replay
from test_replay_quality import manifest_for

FORWARD = 'temporal-v1'
REVERSE = 'temporal-reverse-v1'
# Frozen from the unmodified temporal-v1 producer, before introducing another profile.
FORWARD_PLAN_SHA256 = '03f4e5ec2feb4b95bfc416ab2b09d0af71e2743b7a1f8eb9564288bfc064bf97'


def capture_for(profile):
    return dict(source='loopback-moving-scene', provenance=dict(profile=profile), frames=[
        dict(id=i + 1, time_ms=round(i * 1000 / 60, 6), reset=i in (0, 360), score=i >= 80,
             params=dict(frame_time_ms=16.6666660)) for i in range(480)])


def entrypoint():
    spec = importlib.util.spec_from_file_location('reverse_loopback_entry', HERE / 'run.py')
    entry = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(entry)
    return entry


class ReverseProfile(unittest.TestCase):
    def rows(self, profile):
        try:
            return quality.temporal_frames(profile)
        except TypeError as error:
            self.fail(f'explicit temporal profile selection is missing: {error}')

    def test_reverse_uses_actual_decreasing_pose_history_and_unchanged_phase_clock(self):
        rows = self.rows(REVERSE)
        self.assertEqual(len(rows), 480)
        self.assertEqual([r['frame'] for r in rows], list(range(480)))
        self.assertEqual([r['score'] for r in rows], [False] * 80 + [True] * 400)
        self.assertEqual([r['frame'] for r in rows if r['reset']], [0, 360])
        for index, current, previous in (
            (0, 704, 704), (159, 704, 704), (160, 702, 704), (239, 544, 546),
            (240, 544, 544), (319, 544, 544), (320, 532, 544), (359, 64, 76),
            (360, 32, 32), (361, 32, 32), (479, 32, 32),
        ):
            with self.subTest(frame=index):
                self.assertEqual((rows[index]['pose_frame'], rows[index]['previous_pose_frame']),
                                 (current, previous))
        for phase, first, stop in (
            ('warmup', 0, 80), ('steady', 80, 160), ('motion', 160, 240),
            ('recovery', 240, 320), ('fast-motion', 320, 360), ('cut', 360, 361),
            ('post-cut-recovery', 361, 480),
        ):
            self.assertEqual([r['phase'] for r in rows[first:stop]], [phase] * (stop - first))
        for first, stop, step in ((160, 240, -2), (320, 360, -12)):
            self.assertEqual([rows[i]['pose_frame'] - rows[i - 1]['pose_frame']
                              for i in range(first, stop)], [step] * (stop - first))
        for i, row in enumerate(rows):
            self.assertAlmostEqual(row['time_ms'], i * 1000 / 60, places=5)
            self.assertAlmostEqual(row['frame_time_ms'], 1000 / 60)
            self.assertEqual(row['previous_pose_frame'],
                             row['pose_frame'] if row['reset'] else rows[i - 1]['pose_frame'])

    def test_forward_plan_keeps_frozen_bytes_after_interleaved_reverse_calls(self):
        before = quality.temporal_frames()
        self.rows(REVERSE)
        after = self.rows(FORWARD)
        self.assertEqual(before, after)
        with tempfile.TemporaryDirectory() as value:
            plan = Path(value) / 'moving-plan.txt'
            quality.write_moving_plan(plan, quality.temporal_frames())
            self.assertEqual(plan.stat().st_size, 20046)
            self.assertEqual(hashlib.sha256(plan.read_bytes()).hexdigest(), FORWARD_PLAN_SHA256)
            quality.write_moving_plan(plan, self.rows(REVERSE))
            self.assertEqual(plan.read_text().splitlines()[0], 'moving-plan 1 480')
            self.assertNotEqual(hashlib.sha256(plan.read_bytes()).hexdigest(), FORWARD_PLAN_SHA256)

    def test_reverse_samples_keep_material_jitter_and_actual_previous_matrices(self):
        rows = self.rows(REVERSE)
        manifest = manifest_for(rows)
        quality.validate_samples(manifest, rows)
        with self.assertRaisesRegex(ValueError, 'pose_frame'):
            quality.validate_samples(manifest, quality.temporal_frames())
        for mutate, reason in (
            (lambda m: m['samples'][160].update(material_frame=160), 'material'),
            (lambda m: m['samples'][160].update(previous_pose_frame=706), 'previous_pose'),
            (lambda m: m['samples'][360].update(previous_pose_frame=64), 'previous_pose'),
            (lambda m: m['samples'][160].update(jitter=m['samples'][161]['jitter']), 'jitter'),
            (lambda m: m['samples'][160]['previous'].__setitem__(12, .702), 'previous matrix'),
        ):
            bad = copy.deepcopy(manifest)
            mutate(bad)
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                quality.validate_samples(bad, rows)

    def test_reverse_corpus_metadata_keeps_schema_counts_and_bound_identity(self):
        capture = capture_for(REVERSE)
        summary = quality.corpus_metadata(capture, REVERSE)
        self.assertEqual((summary['schema'], summary['version']), ('retoque-replay-corpus-summary', 1))
        self.assertEqual(summary['profile'], REVERSE)
        self.assertEqual(summary['source'], 'loopback-moving-scene')
        self.assertEqual((summary['frames'], summary['scored_frames']), (480, 400))
        self.assertAlmostEqual(summary['nominal_duration_ms'], 8000)
        self.assertEqual(summary['reset_frame_ids'], [1, 361])
        self.assertEqual([(p['name'], p['first_frame_id'], p['last_frame_id'], p['scored_frames'])
                          for p in summary['phases']], [
            ('warmup', 1, 80, 0), ('steady', 81, 160, 80), ('motion', 161, 240, 80),
            ('recovery', 241, 320, 80), ('fast-motion', 321, 360, 40), ('cut', 361, 361, 1),
            ('post-cut-recovery', 362, 480, 119)])
        capture['provenance']['corpus'] = summary
        self.assertEqual(quality.corpus_metadata(capture, REVERSE), summary)

    def test_declared_identity_cannot_be_relabelled_between_profiles(self):
        for selected, other in ((REVERSE, FORWARD), (FORWARD, REVERSE)):
            for location in ('capture', 'provenance', 'corpus'):
                capture = capture_for(selected)
                identity = capture if location == 'capture' else capture['provenance']
                if location == 'corpus':
                    identity = capture['provenance'].setdefault('corpus', {})
                identity['profile'] = other
                with self.subTest(selected=selected, location=location), \
                        self.assertRaisesRegex(ValueError, 'profile'):
                    quality.corpus_metadata(capture, selected)

    def test_forward_plan_binding_rejects_reverse_relabel_even_when_all_profile_labels_change(self):
        capture = capture_for(REVERSE)
        capture['profile'] = REVERSE
        capture['provenance']['moving_plan_sha256'] = FORWARD_PLAN_SHA256
        capture['provenance']['corpus'] = dict(profile=REVERSE)
        with self.assertRaisesRegex(ValueError, 'moving.plan'):
            quality.corpus_metadata(capture, REVERSE)

    def test_reverse_metadata_applies_the_existing_clock_and_reset_guards(self):
        for mutate, reason in (
            (lambda c: c['frames'].pop(), 'frame count'),
            (lambda c: c['frames'][0].update(id=True), 'frame id'),
            (lambda c: c['frames'][80].update(score=False), 'scoring'),
            (lambda c: c['frames'][360].update(reset=False), 'reset'),
            (lambda c: c['frames'][320]['params'].update(frame_time_ms=1000 / 120), 'frame_time_ms'),
            (lambda c: c['frames'][320].update(time_ms=float('nan')), 'time_ms'),
        ):
            capture = capture_for(REVERSE)
            mutate(capture)
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                quality.corpus_metadata(capture, REVERSE)

    def test_phase_observations_accept_selected_profile_and_refuse_other_identity(self):
        capture = capture_for(REVERSE)
        entries = [dict(id=i + 1, kind='every', interval=1) for i in range(480)]
        entries[360].update(kind='skipped', interval=0)
        entries[400].update(kind='carried', interval=2)
        runs = dict(candidate=dict(frames=entries))
        try:
            phases = quality.phase_observations(capture, runs, REVERSE)['candidate']
        except TypeError as error:
            self.fail(f'phase observations do not accept explicit profile: {error}')
        self.assertEqual(phases['cut']['counts'], dict(model=0, carried=0, skipped=1))
        self.assertEqual(phases['post-cut-recovery']['counts'], dict(model=118, carried=1, skipped=0))
        self.assertEqual(phases['post-cut-recovery']['intervals'], {'1': 118, '2': 1})
        with self.assertRaisesRegex(ValueError, 'profile'):
            quality.phase_observations(capture, runs)

    def test_corpus_builder_transports_reverse_plan_and_bound_identity(self):
        rows = self.rows(REVERSE)
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            for name in run_replay.STAGED:
                (root / name).write_bytes(b'host fixture')
            def recorded(command, *, cwd, env, log, timeout):
                self.assertEqual(command[-4:], ['--moving-corpus', '480', '--moving-plan', 'moving-plan.txt'])
                lines = (cwd / 'moving-plan.txt').read_text().splitlines()
                self.assertEqual(lines[1].split()[:4], ['0', '704', '704', '1'])
                self.assertEqual(lines[161].split()[:4], ['160', '702', '704', '0'])
                self.assertEqual(lines[360].split()[:4], ['359', '64', '76', '0'])
                (cwd / 'moving.json').write_text(json.dumps(manifest_for(rows)))
                (cwd / 'dlssnr-loopback.log').write_text('MOVING-SCENE PASS: evaluations=480 captures=480\n')
                for suffix, size in (('output', 640 * 360 * 8), ('depth', 640 * 360 * 4),
                                     ('motion', 640 * 360 * 4)):
                    shared = cwd / ('shared.' + suffix)
                    shared.write_bytes(bytes(size))
                    for i in range(480):
                        os.link(shared, cwd / f'f{i}.{suffix}')
                return 0
            with patch.object(run_replay, 'execute', side_effect=recorded), \
                    patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
                path = run_replay.make_corpus(types.SimpleNamespace(HERE=HERE), Path('/host/proton'), {},
                                              root, dict(producer='host-only'), profile=REVERSE)
            capture = run_replay.load_capture(path)
            self.assertEqual(capture['provenance']['profile'], REVERSE)
            self.assertEqual(capture['provenance']['corpus']['profile'], REVERSE)
            self.assertEqual(capture['provenance']['moving_plan_sha256'],
                             hashlib.sha256((path.parent / 'moving-plan.txt').read_bytes()).hexdigest())
            self.assertNotEqual(capture['provenance']['moving_plan_sha256'], FORWARD_PLAN_SHA256)
            quality.validate_temporal_capture(capture, REVERSE)
            with self.assertRaisesRegex(ValueError, 'profile'):
                quality.validate_temporal_capture(capture)
            replay = root / 'replay-plan.txt'
            run_replay.write_plan(capture, path.parent, replay)
            self.assertEqual(replay.read_text().splitlines()[364].split()[:5],
                             ['361', '361', '6000.0', repr(1000 / 60), '1'])


class ReverseEntryPoint(unittest.TestCase):
    def test_main_selects_profile_directory_before_the_dll_check(self):
        entry = entrypoint()
        for args, directory in (([], 'replay-quality-run'),
                                (['--replay-profile', FORWARD], 'replay-quality-run'),
                                (['--replay-profile', REVERSE], 'replay-quality-reverse-run'),
                                ([f'--replay-profile={REVERSE}'], 'replay-quality-reverse-run')):
            with self.subTest(args=args), tempfile.TemporaryDirectory() as value:
                root = Path(value)
                with patch.object(entry, 'OUT', root), patch.dict(os.environ, {}, clear=True), \
                        patch.object(sys, 'argv', ['run.py', '--replay-quality', *args,
                                                  '--dll', str(root / 'absent.dll')]):
                    with self.assertRaisesRegex(SystemExit, 'no OptiScaler build'):
                        entry.main()
                self.assertEqual(entry.RUN, root / directory)
                self.assertEqual(list(root.iterdir()), [])

    def test_cli_conflicts_are_rejected_before_staging_or_runtime(self):
        entry = entrypoint()
        cases = [(['--replay-profile', REVERSE], {}, 'requires --replay-quality'),
                 (['--replay-nr', '--replay-profile', REVERSE], {}, 'requires --replay-quality'),
                 (['--moving-scene', '--replay-profile', REVERSE], {}, 'requires --replay-quality'),
                 (['--replay-quality', '--replay-profile', 'unknown'], {}, 'invalid choice'),
                 (['--replay-quality', '--replay-prof', REVERSE], {}, 'unrecognized arguments'),
                 (['--replay-quality', '--replay-profile', REVERSE, '--runtime', 'wine'], {},
                  'requires --runtime proton')]
        cases += [(['--replay-quality', '--replay-profile', REVERSE], {name: value}, name)
                  for name, value in (('LOOPBACK_REPLAY_CAPTURE', 'capture.json'),
                                      ('LOOPBACK_REPLAY_FRAMES', '256'), ('LOOPBACK_REPLAY_WARMUP', '160'),
                                      ('LOOPBACK_REPLAY_RECORD_CHECK', '1'))]
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            for args, env, reason in cases:
                error = io.StringIO()
                with self.subTest(args=args, env=env), patch.object(entry, 'OUT', root), \
                        patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', ['run.py', *args]), \
                        contextlib.redirect_stderr(error):
                    with self.assertRaises(SystemExit) as caught:
                        entry.main()
                    self.assertEqual(caught.exception.code, 2)
                    self.assertIn(reason, error.getvalue())
            self.assertEqual(list(root.iterdir()), [])

    def test_actual_entrypoint_lease_matches_selected_profile_and_preflight(self):
        for args, directory, code in (
            (['--replay-quality'], 'replay-quality-run', 0),
            (['--replay-quality', '--replay-profile', FORWARD], 'replay-quality-run', 0),
            (['--replay-quality', '--replay-profile', REVERSE], 'replay-quality-reverse-run', 0),
            (['--replay-quality', f'--replay-profile={REVERSE}'], 'replay-quality-reverse-run', 0),
            (['--replay-nr'], 'replay-run', 0),
            (['--replay-nr', '--replay-profile', REVERSE], '', 2),
            (['--replay-quality', '--replay-prof', REVERSE], '', 2),
            (['--replay-quality', '--replay-profile', REVERSE, '--runtime', 'wine'], '', 2),
        ):
            script = ('import contextlib,runpy,sys\n'
                      f'sys.path.insert(0,{str(HERE)!r})\nimport run_moving\n'
                      '@contextlib.contextmanager\ndef lease(work):\n'
                      '    print(work.name,flush=True)\n    raise SystemExit(0)\n    yield\n'
                      'run_moving.lease=lease\n'
                      f'sys.argv={[str(HERE / "run.py"), *args]!r}\n'
                      f'runpy.run_path({str(HERE / "run.py")!r},run_name="__main__")\n')
            result = subprocess.run([sys.executable, '-c', script], capture_output=True, text=True,
                                    timeout=3, env={})
            with self.subTest(args=args):
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), directory)

    def test_main_transports_selected_profile_through_proton_to_production_fixture(self):
        entry = entrypoint()
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            fake_home = root / 'home'
            kit = fake_home / '.local/opt/dlssnr-kit'
            kit.mkdir(parents=True)
            for name in ('nvngx.dll_dlssnr.dll', 'nvngx_dlssnr.dll', '_nvngx.dll', 'nvngx_dlss.dll'):
                (kit / name).write_bytes(b'host-only dependency')
            proton = fake_home / '.local/share/Steam/steamapps/common/Proton - Experimental/proton'
            proton.parent.mkdir(parents=True)
            proton.write_bytes(b'not executed')
            dll = root / 'OptiScaler.dll'
            dll.write_bytes(b'host-only build')
            for profile, args, directory in (
                (FORWARD, [], 'replay-quality-run'),
                (REVERSE, ['--replay-profile', REVERSE], 'replay-quality-reverse-run'),
            ):
                work = root / 'artifacts' / directory
                work.mkdir(parents=True)
                (work / 'dlssnr-loopback.exe').write_bytes(b'not executed')
                def fixture(runner, runtime, env, selected_dll, profile=None):
                    return profile, runner.RUN.name, runtime, selected_dll
                with self.subTest(profile=profile), patch.object(entry, 'OUT', root / 'artifacts'), \
                        patch.object(Path, 'home', return_value=fake_home), \
                        patch.dict(os.environ, {}, clear=True), \
                        patch.dict(sys.modules, {entry.__name__: entry}), \
                        patch.object(run_replay, 'run', side_effect=fixture), \
                        patch.object(sys, 'argv', ['run.py', '--replay-quality', *args,
                                                  '--skip-build', '--dll', str(dll)]):
                    try:
                        result = entry.main()
                    except SystemExit as error:
                        self.fail(f'valid profile must reach the production fixture: {error}')
                    self.assertEqual(result, (profile, directory, proton, dll))
                    self.assertEqual((work / 'OptiScaler.dll').read_bytes(), b'host-only build')


if __name__ == '__main__':
    unittest.main()
