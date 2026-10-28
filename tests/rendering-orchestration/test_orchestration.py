"""Host-only orchestration and evidence controls; no compiler prefix or GPU."""
import contextlib
import copy
import importlib.util
import hashlib
import json
import io
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('rendering_report', ROOT/'tools/test-rendering.py')
report = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = report
if spec.loader is not None and (ROOT/'tools/test-rendering.py').exists():
    spec.loader.exec_module(report)
spec_runner = importlib.util.spec_from_file_location('rendering_runner', ROOT/'tests/run_all.py')
runner = importlib.util.module_from_spec(spec_runner)
sys.modules[spec_runner.name] = runner
spec_runner.loader.exec_module(runner)


class CommandOverride(unittest.TestCase):
    def test_nonzero_override_is_supervised_and_retained(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            (root/'tests/fixture').mkdir(parents=True)
            with patch.object(runner, 'ROOT', root), patch.object(runner, 'TESTS', root/'tests'):
                result = runner.run_suite('fixture', {'tier': 'host', 'requires': [], 'timeout': 5},
                                          root, False, command=[sys.executable, '-c',
                                                               'print("override executed"); raise SystemExit(7)'])
            self.assertEqual(result.status, 'FAIL')
            self.assertEqual(result.returncode, 7)
            self.assertIn('override executed', (root/'fixture.log').read_text())

    def test_timeout_receipt_failure_preserves_supervision_outcome(self):
        for error in (ValueError('controlled invalid receipt'), OSError('controlled write failure')):
            with self.subTest(error=error), tempfile.TemporaryDirectory() as value:
                root = Path(value)
                (root/'tests/fixture').mkdir(parents=True)
                entry = {'tier': 'host', 'requires': [], 'timeout': .05, 'result': 'artifacts/result.json'}
                with patch.object(runner, 'ROOT', root), patch.object(runner, 'TESTS', root/'tests'),                         patch.object(runner, 'write_result', side_effect=error):
                    result = runner.run_suite('fixture', entry, root, False,
                                              command=[sys.executable, '-c', 'import time;time.sleep(60)'])
                self.assertEqual(result.status, 'TIMEOUT')
                self.assertIsInstance(result.returncode, int)
                self.assertLess(result.returncode, 0)
                self.assertIn('receipt', result.note)

    def test_abort_receipt_failure_preserves_supervision_outcome(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            (root/'tests/fixture').mkdir(parents=True)
            ready = root/'ready'
            stop = threading.Event()
            def request_stop():
                deadline = time.monotonic()+5
                while not ready.exists() and time.monotonic()<deadline:
                    time.sleep(.01)
                stop.set()
            watcher = threading.Thread(target=request_stop)
            watcher.start()
            try:
                with patch.object(runner, 'ROOT', root), patch.object(runner, 'TESTS', root/'tests'),                         patch.object(runner, 'write_result', side_effect=OSError('controlled abort receipt failure')):
                    result = runner.run_suite('fixture', {'tier': 'host', 'requires': [], 'timeout': 10,
                                                          'result': 'artifacts/result.json'}, root, False, stop=stop,
                                              command=[sys.executable, '-c', 'import time;from pathlib import Path;'
                                                       f'Path({str(ready)!r}).write_text("ready");time.sleep(60)'])
            finally:
                watcher.join(timeout=6)
            self.assertEqual(result.status, 'ABORTED')
            self.assertIsInstance(result.returncode, int)
            self.assertLess(result.returncode, 0)
            self.assertIn('receipt', result.note)

    def test_invalid_override_fails_before_launch(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            (root/'tests/fixture').mkdir(parents=True)
            for command in ([], [''], ['python', 7], ['python', '\0'], 'python -c pass'):
                with self.subTest(command=command), patch.object(runner, 'ROOT', root), \
                        patch.object(runner, 'TESTS', root/'tests'), \
                        patch.object(runner.subprocess, 'Popen', side_effect=AssertionError('must not launch')):
                    result = runner.run_suite('fixture', {'tier': 'host', 'requires': [], 'timeout': 5},
                                              root, False, command=command)
                    self.assertEqual(result.status, 'FAIL')
                    self.assertIsNone(result.returncode)


class Evidence(unittest.TestCase):
    def test_cli_duration_is_opt_in_and_finite_bounded(self):
        self.assertEqual(report.parse_args([]).fg_steady_seconds, 0)
        self.assertEqual(report.parse_args(['--fg-steady-seconds', '15']).fg_steady_seconds, 15)
        for value in ('0', '4', '121', 'nan', 'inf', '-1', '15.5'):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    report.parse_args(['--fg-steady-seconds', value])

    def test_exact_vulkan_cases_and_dll_identity_required(self):
        cases = {name: {'status': 'PASS', 'problems': []} for name in report.VK_CASES}
        valid = {'status': 'PASS', 'dll_sha256': 'a'*64, 'cases': cases}
        self.assertEqual(report.validate_vulkan(valid, 'a'*64), [])
        for changed in ({**valid, 'dll_sha256': 'b'*64},
                        {**valid, 'cases': {k:v for k,v in cases.items() if k != 'nr'}},
                        {**valid, 'cases': {**cases, 'nr': {'status': 'SKIP', 'problems': []}}},
                        {**valid, 'cases': {**cases, 'nr': {'status': 'PASS', 'problems': ['validation failure']}}}):
            with self.subTest(changed=changed):
                self.assertTrue(report.validate_vulkan(changed, 'a'*64))

    def test_strict_host_aggregate_rejects_missing_or_skipped_suites(self):
        valid = {'schema': 'retoque-test-run', 'version': 1, 'run_id': 'fresh', 'status': 'PASS',
                 'selected': ['a', 'b'], 'results': [{'name': 'a', 'status': 'PASS'},
                                                   {'name': 'b', 'status': 'PASS'}]}
        self.assertEqual(report.validate_host(valid, ('a', 'b')), [])
        for changed in ({**valid, 'results': valid['results'][:1]},
                        {**valid, 'selected': ['a']},
                        {**valid, 'results': [{'name': 'a', 'status': 'PASS'}, {'name': 'b', 'status': 'SKIP'}]}):
            with self.subTest(changed=changed):
                self.assertTrue(report.validate_host(changed, ('a', 'b')))

    def test_suite_status_and_requested_long_qualification_are_independent(self):
        data = {'status': 'PASS', 'dll_sha256': 'a'*64, 'runtime_coverage': True,
                'qualification': {'status': 'INCONCLUSIVE'}, 'requested_steady_seconds': 15}
        self.assertTrue(report.validate_suite(data, 'a'*64, steady_seconds=15))
        data['qualification']['status'] = 'PASS'
        self.assertEqual(report.validate_suite(data, 'a'*64, steady_seconds=15), [])
        data['dll_sha256'] = 'b'*64
        self.assertTrue(report.validate_suite(data, 'a'*64, steady_seconds=15))

    def test_moving_report_requires_all_trials_controls_and_build_provenance(self):
        valid = {'schema': 1, 'timing': {name: {'n': 16, 'samples_ms': [1.0]*16,
                                                     'p50_ms': 1.0, 'p95_ms': 1.0}
                                         for name in ('off-a', 'on-a', 'on-b', 'off-b')},
                 'negative_controls': [{'control': 'zero-motion', 'reason': 'motion rejected'},
                                       {'control': 'disabled-processing', 'reason': 'processing rejected'}],
                 'guide_oracle': [{'samples': 100, 'max_error_px': .01, 'mean_motion_px': .2}]*16,
                 'processing': [{'mae': .1, 'repeat_mae': .001}]*16,
                 'motion_compensated_edit_temporal': [{'frame': frame, 'channels': 300, 'residual_mae': .01}
                                                      for frame in range(65, 80)],
                 'input_sha256': {str(i): {side: f'{i:064x}' for side in ('source', 'depth', 'motion')}
                                  for i in range(64, 80)},
                 'provenance': {'files': {'OptiScaler.dll': {'sha256': 'a'*64},
                                          'nvngx.dll_dlssnr.dll': {'sha256': 'b'*64}}}}
        self.assertEqual(report.validate_moving(valid, {'dll': 'a'*64, 'forwarder': 'b'*64}), [])
        for changed in ({**valid, 'timing': {}}, {**valid, 'negative_controls': []},
                        {**valid, 'processing': []}, {**valid, 'provenance': {}}):
            self.assertTrue(report.validate_moving(changed, {'dll': 'a'*64, 'forwarder': 'b'*64}))
        # Shape alone cannot attest to real samples, processing, motion or input identity.
        for field, replacement in (('guide_oracle', [None]*16), ('processing', [{}]*16),
                                   ('motion_compensated_edit_temporal', [None]*15),
                                   ('input_sha256', {str(i): {} for i in range(64, 80)})):
            with self.subTest(field=field):
                self.assertTrue(report.validate_moving({**valid, field: replacement},
                                                       {'dll': 'a'*64, 'forwarder': 'b'*64}))
        for change in ({'samples_ms': [None]*16}, {'p50_ms': None}, {'p95_ms': -1},
                       {'samples_ms': [0]*16}, {'samples_ms': [float('nan')]*16},
                       {'p50_ms': 2.0}, {'n': True}):
            broken = copy.deepcopy(valid)
            broken['timing']['on-a'].update(change)
            with self.subTest(change=change):
                self.assertTrue(report.validate_moving(broken, {'dll': 'a'*64, 'forwarder': 'b'*64}))
        for field, row in (('guide_oracle', {'samples': 0, 'max_error_px': 0, 'mean_motion_px': .2}),
                           ('guide_oracle', {'samples': 100, 'max_error_px': .2, 'mean_motion_px': .2}),
                           ('processing', {'mae': 0, 'repeat_mae': 0}),
                           ('processing', {'mae': .1, 'repeat_mae': .2}),
                           ('motion_compensated_edit_temporal', {'frame': 65, 'channels': 0, 'residual_mae': 0})):
            broken = copy.deepcopy(valid)
            broken[field][0] = row
            with self.subTest(field=field, row=row):
                self.assertTrue(report.validate_moving(broken, {'dll': 'a'*64, 'forwarder': 'b'*64}))


    def test_replay_requires_default_seam_gates_without_claiming_pixel_approval(self):
        required = ('off-leaves-every-byte', 'off-records-nothing',
                    'every-a-no-scored-frame-skipped', 'every-b-no-scored-frame-skipped',
                    'cadence2-no-scored-frame-skipped', 'cadence4-no-scored-frame-skipped',
                    'adaptive4-no-scored-frame-skipped', 'every-a-model-every-scored-frame',
                    'every-b-model-every-scored-frame', 'cadence2-carries', 'cadence4-carries',
                    'adaptive-reports-intervals', 'repeat-decisions-identical')
        gates = {name: {'ok': True} for name in required}
        gates['repeat-pixels-identical-frames'] = {'ok': None}
        valid = {'schema': 'retoque-replay-seam-report', 'version': 1,
                 'provenance': {'dll_sha256': 'a'*64}, 'gates': gates,
                 'judge': {'available': True, 'verdicts': {'cadence2': {'verdict': 'FAIL'}}}}
        self.assertEqual(report.validate_replay(valid, 'a'*64), [])
        for changed in ({**valid, 'gates': {}},
                        {**valid, 'gates': {**gates, required[0]: {'ok': None}}},
                        {**valid, 'gates': {**gates, required[0]: {'ok': False}}},
                        {**valid, 'provenance': {'dll_sha256': 'c'*64}}):
            self.assertTrue(report.validate_replay(changed, 'a'*64))

    def test_runtime_hash_changes_cannot_pass(self):
        expected = {'dll': 'a'*64, 'forwarder': 'b'*64}
        self.assertEqual(report.validate_hashes(expected, expected, expected), [])
        self.assertTrue(report.validate_hashes(expected, expected, {**expected, 'forwarder': 'c'*64}))
        self.assertTrue(report.validate_hashes(expected, {**expected, 'dll': None}, expected))



class StepExecution(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'tests/fixture/artifacts').mkdir(parents=True)
        (self.root/'x64/out').mkdir(parents=True)
        (self.root/'x64/out/OptiScaler.dll').write_bytes(b'controlled dll')
        (self.root/'x64/out/nvngx.dll_dlssnr.dll').write_bytes(b'controlled forwarder')
        self.receipt = self.root/'tests/fixture/artifacts/result.json'
        self.logs = self.root/'logs'
        self.logs.mkdir()
        for target, attribute, value in ((report, 'ROOT', self.root), (report.runner, 'ROOT', self.root),
                                          (report.runner, 'TESTS', self.root/'tests')):
            self.enterContext(patch.object(target, attribute, value))
        self.expected = report.runtime_hashes()

    def step(self, code, *, kind='provider', logs=()):
        entry = {'path': 'fixture', 'tier': 'host', 'requires': [], 'timeout': 5}
        if kind == 'provider':
            entry['result'] = 'artifacts/result.json'
        return report.Step('controlled', [sys.executable, '-c', code], entry, kind,
                           self.receipt, log_paths=logs, steady_seconds=15 if kind == 'provider' else 0)

    def emitter(self, status='PASS', exit_code=0, *, mutate=False):
        payload = {'schema': 'retoque-suite-result', 'version': 1, 'status': status,
                   'dll_sha256': self.expected['dll'], 'runtime_coverage': True,
                   'requested_steady_seconds': 15, 'qualification': {'status': status}}
        return ('import json,os;from pathlib import Path;'
                f'data={payload!r};data["run_id"]=os.environ["RETOQUE_TEST_RUN_ID"];'
                f'Path({str(self.receipt)!r}).write_text(json.dumps(data));'
                + (f'Path({str(self.root/"x64/out/OptiScaler.dll")!r}).write_bytes(b"changed");' if mutate else '')
                + f'raise SystemExit({exit_code})')

    def test_nonzero_does_not_erase_fixture_inconclusive_or_receipt(self):
        result = report.run_step(self.step(self.emitter('INCONCLUSIVE', 1)), self.logs, self.expected)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['runner_status'], 'FAIL')
        self.assertEqual(result['fixture_status'], 'INCONCLUSIVE')
        self.assertEqual(result['returncode'], 1)
        self.assertEqual(result['evidence']['qualification']['status'], 'INCONCLUSIVE')
        snapshot = Path(result['receipt_snapshot']['path'])
        self.assertEqual(json.loads(snapshot.read_text())['status'], 'INCONCLUSIVE')

    def test_successful_process_cannot_reuse_stale_legacy_pass(self):
        self.receipt.write_text(json.dumps({'status': 'PASS', 'dll_sha256': self.expected['dll'],
                                           'cases': {name: {'status': 'PASS', 'problems': []}
                                                     for name in report.VK_CASES}}))
        result = report.run_step(self.step('pass', kind='vulkan'), self.logs, self.expected)
        self.assertEqual(result['returncode'], 0)
        self.assertEqual(result['status'], 'FAIL')
        self.assertTrue(result['findings'])

    def test_versioned_receipt_and_runtime_hash_must_both_match(self):
        for code in (self.emitter(mutate=True), self.emitter().replace('os.environ["RETOQUE_TEST_RUN_ID"]', '"stale"')):
            with self.subTest(code=code):
                result = report.run_step(self.step(code), self.logs, self.expected)
                self.assertEqual(result['status'], 'FAIL')
                self.assertTrue(result['findings'])
                (self.root/'x64/out/OptiScaler.dll').write_bytes(b'controlled dll')

    def test_malformed_current_metadata_is_a_reported_step_failure(self):
        code = self.emitter().replace("'qualification': {'status': 'PASS'}", "'qualification': ['PASS']")
        result = report.run_step(self.step(code), self.logs, self.expected)
        self.assertEqual(result['status'], 'FAIL')
        self.assertTrue(result['findings'])

    def test_vulkan_validation_errors_remain_visible_under_fixture_pass(self):
        data = {'status': 'PASS', 'dll_sha256': self.expected['dll'],
                'cases': {name: {'status': 'PASS', 'problems': []} for name in report.VK_CASES}}
        code = ('from pathlib import Path;'
                f'Path({str(self.receipt)!r}).write_text({json.dumps(data)!r});'
                'print("Validation Error: controlled raw Vulkan finding")')
        result = report.run_step(self.step(code, kind='vulkan'), self.logs, self.expected)
        self.assertEqual(result['fixture_status'], 'PASS')
        self.assertEqual(result['validation_findings']['matched_line_count'], 1)
        self.assertIn('raw Vulkan finding', result['validation_findings']['lines'][0]['text'])

    def test_logs_and_validation_findings_survive_later_overwrite(self):
        log = self.root/'tests/fixture/artifacts/current.log'
        code = self.emitter() + ''
        log.write_text('Validation Error: controlled finding\n')
        result = report.run_step(self.step(code, logs=(log,)), self.logs, self.expected)
        self.assertEqual(result['status'], 'PASS')
        self.assertTrue(result['validation_findings']['lines'])
        self.assertEqual(result['validation_findings']['matched_line_count'], 1)
        saved = Path(next(row['path'] for row in result['retained_logs'] if row['source'] == str(log)))
        log.write_text('overwritten')
        self.assertIn('controlled finding', saved.read_text())

    def replay_step(self, *, emit_artifacts):
        work = self.root/'tests/fixture/artifacts/replay-run'
        required = ('off-leaves-every-byte', 'off-records-nothing',
                    'every-a-no-scored-frame-skipped', 'every-b-no-scored-frame-skipped',
                    'cadence2-no-scored-frame-skipped', 'cadence4-no-scored-frame-skipped',
                    'adaptive4-no-scored-frame-skipped', 'every-a-model-every-scored-frame',
                    'every-b-model-every-scored-frame', 'cadence2-carries', 'cadence4-carries',
                    'adaptive-reports-intervals', 'repeat-decisions-identical')
        data = {'schema': 'retoque-replay-seam-report', 'version': 1,
                'provenance': {'dll_sha256': self.expected['dll']},
                'gates': {name: {'ok': True} for name in required},
                'judge': {'available': True, 'verdicts': {'cadence2': {'verdict': 'FAIL'}}}}
        files = {'judge/calibration.json': b'{"bounds":{}}', 'judge/policy.json': b'{"floors":{}}',
                 'judge/corpus.json': b'{"profile":"controlled"}',
                 'judge/cadence2/result.json': b'{"verdict":"FAIL"}',
                 'judge/cadence2/frames.jsonl': b'{"id":80}\n',
                 'judge/cadence2/contact-sheet.png': b'controlled contact bytes',
                 'judge/cadence2/diff-heatmap.png': b'controlled heatmap bytes'}
        code = 'from pathlib import Path\n'
        if emit_artifacts:
            for relative, content in files.items():
                code += (f'p=Path({str(work/relative)!r});p.parent.mkdir(parents=True,exist_ok=True);'
                         f'p.write_bytes({content!r})\n')
        code += f'Path({str(self.receipt)!r}).write_text({json.dumps(data)!r})\n'
        step = self.step(code, kind='replay')
        step.artifact_root = work
        return step, files

    def test_replay_retains_current_judge_bytes_without_erasing_quality_failure(self):
        step, files = self.replay_step(emit_artifacts=True)
        result = report.run_step(step, self.logs, self.expected)
        self.assertIn('artifact_retention', result, 'the replay step must retain current judge artifacts')
        self.assertEqual(result['artifact_retention']['status'], 'PASS')
        self.assertEqual(result['functional_status'], 'PASS')
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['quality_qualification']['original_judge']['verdicts']['cadence2']['verdict'], 'FAIL')
        retained = {row['relative_path']: row for row in result['retained_artifacts']}
        self.assertEqual(set(retained), set(files))
        for relative, content in files.items():
            self.assertEqual(Path(retained[relative]['path']).read_bytes(), content)
            self.assertEqual(retained[relative]['sha256'], hashlib.sha256(content).hexdigest())

    def test_replay_missing_current_artifacts_fails_qualification_but_preserves_functional_pass(self):
        step, files = self.replay_step(emit_artifacts=False)
        for relative, content in files.items():
            path = step.artifact_root/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        result = report.run_step(step, self.logs, self.expected)
        self.assertEqual(result['status'], 'FAIL', 'old judge files cannot attest to this invocation')
        self.assertEqual(result['functional_status'], 'PASS')
        self.assertEqual(result['artifact_retention']['status'], 'FAIL')
        self.assertEqual(result['retained_artifacts'], [])
        self.assertEqual(result['quality_qualification']['original_judge']['verdicts']['cadence2']['verdict'], 'FAIL')
        self.assertTrue(all((step.artifact_root/relative).is_file() for relative in files))


class ReplayRetention(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root/'replay-run'
        self.destination = self.root/'retained'

    def inventory(self):
        self.assertTrue(callable(getattr(report, 'replay_artifact_inventory', None)),
                        'replay freshness inventory is missing')
        return report.replay_artifact_inventory(self.source)

    def evidence(self, variants=('every-b',)):
        return {'judge': {'available': True,
                          'verdicts': {name: {'verdict': 'PASS' if name == 'every-b' else 'FAIL'}
                                       for name in variants}}}

    def write_files(self, variants=('every-b',)):
        files = {'judge/calibration.json': b'{"bounds":{}}', 'judge/policy.json': b'{"floors":{}}',
                 'judge/corpus.json': b'{"profile":"controlled"}'}
        for name in variants:
            files.update({f'judge/{name}/result.json': b'{"verdict":"PASS"}',
                          f'judge/{name}/frames.jsonl': b'{"id":80}\n',
                          f'judge/{name}/contact-sheet.png': b'controlled contact bytes',
                          f'judge/{name}/diff-heatmap.png': b'controlled heatmap bytes'})
        for relative, content in files.items():
            path = self.source/relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        return files

    def retain(self, before, variants=('every-b',)):
        self.assertTrue(callable(getattr(report, 'retain_replay_artifacts', None)),
                        'bounded replay artifact retention is missing')
        return report.retain_replay_artifacts(self.source, self.destination, before, self.evidence(variants))

    def test_fresh_evidence_has_actual_bytes_hashes_and_survives_source_overwrite(self):
        before = self.inventory()
        files = self.write_files(('every-b', 'cadence2'))
        records, outcome = self.retain(before, ('every-b', 'cadence2'))
        self.assertEqual(outcome['status'], 'PASS', outcome)
        self.assertEqual(outcome['bytes'], sum(map(len, files.values())))
        by_name = {row['relative_path']: row for row in records}
        self.assertEqual(set(by_name), set(files))
        for relative, content in files.items():
            (self.source/relative).write_bytes(b'later overwrite')
            row = by_name[relative]
            self.assertEqual(Path(row['path']).read_bytes(), content)
            self.assertEqual(row['bytes'], len(content))
            self.assertEqual(row['sha256'], hashlib.sha256(content).hexdigest())
        self.assertEqual(outcome['limits'], {'variants': 32, 'files': 132, 'file_bytes': 16*1024*1024,
                                             'summary_bytes': 2*1024*1024, 'total_bytes': 128*1024*1024})

    def test_unchanged_old_artifacts_are_stale_and_never_deleted(self):
        files = self.write_files()
        before = self.inventory()
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertEqual(records, [])
        self.assertEqual({row['relative_path'] for row in outcome['excluded'] if row['reason'] == 'STALE'}, set(files))
        self.assertTrue(all((self.source/relative).read_bytes() == content for relative, content in files.items()))

    def test_identical_content_rewritten_by_current_invocation_is_fresh(self):
        files = self.write_files()
        before = self.inventory()
        for relative, content in files.items():
            path = self.source/relative
            replacement = path.with_name(path.name+'.replacement')
            replacement.write_bytes(content)
            replacement.replace(path)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'PASS', outcome)
        self.assertEqual(len(records), len(files))

    def test_identical_in_place_rewrite_is_fresh_without_touching_old_diagnostics(self):
        files = self.write_files()
        before = self.inventory()
        self.write_files()
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'PASS', outcome)
        self.assertEqual(len(records), len(files))

    def test_raw_trees_and_unreported_variants_are_not_copied(self):
        before = self.inventory()
        expected = self.write_files()
        raw = self.source/'runs/every-b/raw/frame.bin'
        raw.parent.mkdir(parents=True)
        with raw.open('wb') as handle:
            handle.truncate(1024*1024*1024)
        nested = self.source/'judge/every-b/raw/frame.bin'
        nested.parent.mkdir()
        nested.write_bytes(b'not a retained artifact')
        other = self.source/'judge/unreported/result.json'
        other.parent.mkdir()
        other.write_bytes(b'not current reported evidence')
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'PASS', outcome)
        self.assertEqual({row['relative_path'] for row in records}, set(expected))
        self.assertFalse(any(path.suffix == '.bin' for path in self.destination.rglob('*')))

    def test_optional_moving_plan_is_retained_when_fresh(self):
        before = self.inventory()
        expected = self.write_files()
        content = b'controlled versioned moving plan\n'
        (self.source/'judge/moving-plan.txt').write_bytes(content)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'PASS', outcome)
        self.assertEqual(len(records), len(expected)+1)
        plan = next(row for row in records if row['relative_path'] == 'judge/moving-plan.txt')
        self.assertEqual(Path(plan['path']).read_bytes(), content)

    def test_file_symlink_is_not_followed_or_copied(self):
        before = self.inventory()
        self.write_files()
        outside = self.root/'outside.json'
        outside.write_bytes(b'outside bytes must not enter evidence')
        source = self.source/'judge/every-b/result.json'
        source.unlink()
        source.symlink_to(outside)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertNotIn(str(source), {row['source'] for row in records})
        self.assertTrue(any('SYMLINK' in row['reason'] for row in outcome['excluded']))

    def test_parent_directory_symlink_is_not_followed(self):
        before = self.inventory()
        self.write_files()
        directory = self.source/'judge/every-b'
        outside = self.root/'outside'
        directory.rename(outside)
        directory.symlink_to(outside, target_is_directory=True)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertFalse(any(row['relative_path'].startswith('judge/every-b/') for row in records))

    def test_source_root_symlink_is_not_followed(self):
        before = self.inventory()
        self.write_files()
        outside = self.root/'outside'
        self.source.rename(outside)
        self.source.symlink_to(outside, target_is_directory=True)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertEqual(records, [])

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires a native FIFO')
    def test_special_file_is_rejected_without_blocking(self):
        before = self.inventory()
        self.write_files()
        path = self.source/'judge/every-b/frames.jsonl'
        path.unlink()
        os.mkfifo(path)
        started = time.monotonic()
        records, outcome = self.retain(before)
        self.assertLess(time.monotonic()-started, 2)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertNotIn(str(path), {row['source'] for row in records})

    def test_missing_required_current_file_is_an_explicit_retention_failure(self):
        before = self.inventory()
        self.write_files()
        (self.source/'judge/policy.json').unlink()
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertTrue(any(row['relative_path'] == 'judge/policy.json' and row['reason'] == 'MISSING'
                            for row in outcome['excluded']))

    def test_per_file_and_summary_limits_reject_before_copy(self):
        before = self.inventory()
        self.write_files()
        for relative, length in (('judge/calibration.json', 2*1024*1024+1),
                                 ('judge/every-b/contact-sheet.png', 16*1024*1024+1)):
            with (self.source/relative).open('wb') as handle:
                handle.truncate(length)
        records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertFalse(any(row['relative_path'] in ('judge/calibration.json', 'judge/every-b/contact-sheet.png')
                             for row in records))
        self.assertEqual(len([row for row in outcome['excluded'] if row['reason'] == 'FILE_LIMIT']), 2)

    def test_file_count_and_total_byte_limits_bound_actual_copies(self):
        before = self.inventory()
        self.write_files()
        with patch.dict(report.REPLAY_ARTIFACT_LIMITS, {'files': 2}):
            records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertEqual(len(records), 2)
        self.assertTrue(any(row['reason'] == 'COUNT_LIMIT' for row in outcome['excluded']))
        self.destination = self.root/'byte-limited'
        with patch.dict(report.REPLAY_ARTIFACT_LIMITS, {'total_bytes': 30}):
            records, outcome = self.retain(before)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertLessEqual(sum(row['bytes'] for row in records), 30)
        self.assertTrue(any(row['reason'] == 'TOTAL_LIMIT' for row in outcome['excluded']))

    def test_too_many_reported_variants_and_unsafe_names_cannot_escape_bounds(self):
        before = self.inventory()
        self.write_files()
        for variants in (tuple(f'v{i}' for i in range(33)), ('../outside',)):
            with self.subTest(variants=variants):
                records, outcome = self.retain(before, variants)
                self.assertEqual(outcome['status'], 'FAIL')
                self.assertLessEqual(len(records), 132)
                self.assertFalse(any('outside' in row['relative_path'] for row in records))

    def test_incomplete_previous_inventory_never_treats_unknown_files_as_fresh(self):
        self.write_files()
        for number in range(33):
            (self.source/'judge'/f'old-{number}').mkdir()
        before = self.inventory()
        self.assertFalse(before['complete'])
        self.write_files()
        records, outcome = self.retain(before)
        self.assertEqual(records, [])
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertTrue(any(row['reason'] == 'INVENTORY_INCOMPLETE' for row in outcome['excluded']))

    def test_copy_destination_failure_is_explicit_and_publishes_no_partial_artifacts(self):
        before = self.inventory()
        self.write_files()
        self.destination.write_bytes(b'not a directory')
        records, outcome = self.retain(before)
        self.assertEqual(records, [])
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertEqual(self.destination.read_bytes(), b'not a directory')
        self.assertTrue(outcome['findings'])

    def test_destination_directory_swap_at_publication_cannot_publish_a_successful_record(self):
        before = self.inventory()
        files = self.write_files()
        replace = os.replace
        for replacement in ('directory', 'symlink', 'missing'):
            with self.subTest(replacement=replacement):
                self.destination = self.root/f'retained-{replacement}'
                target = self.destination/'judge/calibration.json'
                detached = self.root/f'detached-{replacement}'
                changed = False
                def swap_parent(source, destination, **kwargs):
                    nonlocal changed
                    if not changed and destination == target.name and kwargs.get('dst_dir_fd') is not None:
                        changed = True
                        target.parent.rename(detached)
                        if replacement == 'directory':
                            target.parent.mkdir()
                            target.write_bytes(b'counterfeit bytes at reported path')
                        elif replacement == 'symlink':
                            target.parent.symlink_to(detached, target_is_directory=True)
                    return replace(source, destination, **kwargs)
                with patch.object(report.os, 'replace', side_effect=swap_parent):
                    records, outcome = self.retain(before)
                self.assertTrue(changed)
                self.assertEqual(outcome['status'], 'FAIL')
                self.assertFalse(any(row['relative_path'] == 'judge/calibration.json' for row in records))
                self.assertTrue(any(row['relative_path'] == 'judge/calibration.json'
                                    and row['reason'] == 'DESTINATION_CHANGED' for row in outcome['excluded']))
                self.assertEqual((detached/'calibration.json').read_bytes(), files['judge/calibration.json'])

    def test_replaced_or_modified_published_file_cannot_keep_the_original_hash_receipt(self):
        before = self.inventory()
        self.write_files()
        replace = os.replace
        for mutation in ('replacement', 'in-place'):
            with self.subTest(mutation=mutation):
                self.destination = self.root/f'retained-{mutation}'
                target = self.destination/'judge/calibration.json'
                changed = False
                def replace_then_mutate(source, destination, **kwargs):
                    nonlocal changed
                    result = replace(source, destination, **kwargs)
                    if not changed and destination == target.name and kwargs.get('dst_dir_fd') is not None:
                        changed = True
                        if mutation == 'replacement':
                            other = self.root/'replacement-file'
                            other.write_bytes(b'counterfeit published file')
                            replace(other, target)
                        else:
                            target.write_bytes(b'X'*target.stat().st_size)
                    return result
                with patch.object(report.os, 'replace', side_effect=replace_then_mutate):
                    records, outcome = self.retain(before)
                self.assertTrue(changed)
                self.assertEqual(outcome['status'], 'FAIL')
                self.assertFalse(any(row['relative_path'] == 'judge/calibration.json' for row in records))
                self.assertTrue(any(row['relative_path'] == 'judge/calibration.json'
                                    and row['reason'] == 'DESTINATION_CHANGED' for row in outcome['excluded']))

    def test_failed_publication_cannot_accumulate_copies_beyond_remaining_total_budget(self):
        before = self.inventory()
        files = self.write_files()
        replace = os.replace
        for prior_successes in (0, 1):
            with self.subTest(prior_successes=prior_successes):
                self.destination = self.root/f'bounded-publication-{prior_successes}'
                for relative in files:
                    length = 10 if prior_successes and relative == 'judge/calibration.json' else 20
                    (self.source/relative).write_bytes(b'A'*length)
                publications = 0
                def publish_then_change_one_byte(source, destination, **kwargs):
                    nonlocal publications
                    result = replace(source, destination, **kwargs)
                    directory = kwargs.get('dst_dir_fd')
                    if directory is not None:
                        publications += 1
                        if publications > prior_successes:
                            descriptor = os.open(destination, os.O_WRONLY, dir_fd=directory)
                            try:
                                os.write(descriptor, b'X')
                            finally:
                                os.close(descriptor)
                    return result
                with patch.dict(report.REPLAY_ARTIFACT_LIMITS, {'total_bytes': 30}), \
                        patch.object(report.os, 'replace', side_effect=publish_then_change_one_byte):
                    records, outcome = self.retain(before)
                physical_files = [path for path in self.destination.rglob('*') if path.is_file()]
                self.assertEqual(outcome['status'], 'FAIL')
                self.assertEqual(len(records), prior_successes)
                self.assertEqual(outcome['bytes'], 10*prior_successes)
                self.assertTrue(any(row['reason'] == 'DESTINATION_CHANGED' for row in outcome['excluded']))
                self.assertLessEqual(sum(path.stat().st_size for path in physical_files), 30)
                self.assertEqual(publications, prior_successes+1)

    def test_changed_source_during_copy_is_discarded(self):
        before = self.inventory()
        self.write_files()
        source = self.source/'judge/calibration.json'
        identity = source.stat().st_ino
        read = os.read
        changed = False
        def mutate(fd, count):
            nonlocal changed
            content = read(fd, count)
            if not changed and os.fstat(fd).st_ino == identity:
                changed = True
                source.write_bytes(b'changed during copy')
            return content
        with patch.object(report.os, 'read', side_effect=mutate):
            records, outcome = self.retain(before)
        self.assertTrue(changed)
        self.assertEqual(outcome['status'], 'FAIL')
        self.assertNotIn(str(source), {row['source'] for row in records})
        self.assertFalse((self.destination/'judge/calibration.json').exists())



class PlanAndReport(unittest.TestCase):
    def test_vulkan_plan_retains_sidecar_bytes_hashes_and_raw_validation_sources(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            expected = {}
            for name, count in zip(report.VK_CASES, (2, 6, 0, 6, 6)):
                work = root/'tests/vulkan-present-host/artifacts/cases'/name
                work.mkdir(parents=True)
                validation = ''.join(f'Validation Error: VUID-{name}-controlled-{i} native diagnostic\n'
                                     for i in range(count)).encode()
                attribution = json.dumps({'model_messages': count,
                                          'model_vuids': ','.join(f'VUID-{name}-controlled-{i}' for i in range(count))}).encode()
                for filename, data in (('validation.txt', validation), ('result.json', attribution)):
                    if filename == 'validation.txt' and count == 0:
                        continue  # Native producer emits no raw file when no validation message occurred.
                    path = work/filename
                    path.write_bytes(data)
                    expected[str(path)] = data
            with patch.object(report, 'ROOT', root):
                plan = report.build_steps(report.parse_args(['--no-build']), root/'logs', {'a': {'tier': 'host'}})
            vulkan = next(step for step in plan if step.kind == 'vulkan')
            records, findings = report.retain_logs(vulkan.log_paths, root/'retained')
            by_source = {row['source']: row for row in records}
            self.assertEqual(set(by_source), set(expected))
            for source, data in expected.items():
                saved = by_source[source]
                self.assertEqual(Path(saved['path']).read_bytes(), data)
                self.assertEqual(saved['sha256'], hashlib.sha256(data).hexdigest())
            native_lines = []
            for name, count in zip(report.VK_CASES, (2, 6, 0, 6, 6)):
                source = str(root/'tests/vulkan-present-host/artifacts/cases'/name/'validation.txt')
                if count == 0:
                    self.assertNotIn(source, by_source)
                    facts_source = str(root/'tests/vulkan-present-host/artifacts/cases'/name/'result.json')
                    facts = json.loads(Path(by_source[facts_source]['path']).read_text())
                    self.assertEqual(facts['model_messages'], 0)
                    continue
                retained = by_source[source]['path']
                lines = [row for row in findings['lines'] if row['log'] == retained]
                self.assertEqual(len(lines), count)
                native_lines.extend(lines)
                self.assertEqual(lines[0]['line'], 1)
                self.assertIn(f'VUID-{name}-controlled-0', lines[0]['text'])
            self.assertEqual(len(native_lines), 20)

    def test_plan_is_strict_serial_and_only_legacy_compilers_take_outer_lock(self):
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(report.parse_args(['--fg-steady-seconds', '15']), Path(value),
                                      {'a': {'tier': 'host'}})
        self.assertEqual([step.name for step in plan],
                         ['graphics-preflight', 'host', 'build', 'fg-inputs', 'nr-moving', 'nr-replay', 'vulkan', 'fg-provider'])
        self.assertIn('--strict', plan[1].command)
        self.assertEqual(plan[2].command[-1], 'Release')
        self.assertEqual([step.command[0] == 'flock' for step in plan],
                         [False, False, False, False, False, False, True, False])
        self.assertEqual(plan[-1].command[-2:], ['--steady-seconds', '15'])
        self.assertEqual(plan[-2].command[-2:], ['--only', ','.join(report.VK_CASES)])
        self.assertIn('-o', plan[6].command)

    def test_host_only_and_no_build_select_exact_work(self):
        with tempfile.TemporaryDirectory() as value:
            log_dir = Path(value)
            self.assertEqual([s.name for s in report.build_steps(report.parse_args(['--host-only']),
                                                                log_dir, {'a': {'tier': 'host'}})], ['host'])
            plan = report.build_steps(report.parse_args(['--no-build']), log_dir, {'a': {'tier': 'host'}})
            self.assertNotIn('build', [step.name for step in plan])
            self.assertNotIn('--steady-seconds', plan[-1].command)

    def test_replay_plan_selects_only_its_known_artifact_root(self):
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(report.parse_args(['--no-build']), Path(value), {'a': {'tier': 'host'}})
        replay = next(step for step in plan if step.kind == 'replay')
        self.assertEqual(getattr(replay, 'artifact_root', None), ROOT/'tests/dlssnr-loopback/artifacts/replay-run')
        self.assertTrue(all(getattr(step, 'artifact_root', None) is None for step in plan if step is not replay))

    def test_failed_host_blocks_runtime_and_cannot_leave_old_pass_report(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            report_path.write_text('{"status":"PASS"}')
            results = [{'name': 'graphics-preflight', 'kind': 'preflight', 'status': 'PASS', 'hashes_after': {}},
                       {'name': 'host', 'kind': 'host', 'status': 'FAIL', 'hashes_after': {}}]
            with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', side_effect=results) as execute:
                with contextlib.redirect_stdout(io.StringIO()):
                    code = report.main(['--report', str(report_path), '--log-dir', value])
            self.assertEqual(code, 1)
            self.assertEqual(execute.call_count, 2)
            data = json.loads(report_path.read_text())
            self.assertEqual(data['status'], 'FAIL')
            self.assertEqual([row['status'] for row in data['steps']], ['PASS', 'FAIL'] + ['NOT_RUN']*6)

    def test_any_runtime_nonpass_stops_remaining_plan_with_not_run_records(self):
        for failure in ('TIMEOUT', 'FAIL', 'INCONCLUSIVE'):
            with self.subTest(failure=failure):
                with tempfile.TemporaryDirectory() as value:
                    report_path = Path(value)/'summary.json'
                    results = [{'name': 'graphics-preflight', 'kind': 'preflight', 'status': 'PASS', 'hashes_after': {}},
                               {'name': 'host', 'kind': 'host', 'status': 'PASS', 'hashes_after': {}},
                               {'name': 'build', 'kind': 'build', 'status': 'PASS', 'hashes_after': {}},
                               {'name': 'fg-inputs', 'kind': 'inputs', 'status': failure, 'hashes_after': {}}]
                    with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', side_effect=results) as execute,                     contextlib.redirect_stdout(io.StringIO()):
                        code = report.main(['--report', str(report_path), '--log-dir', value])
                    self.assertEqual(code, 1)
                    self.assertEqual(execute.call_count, 4)
                    self.assertEqual([row['status'] for row in json.loads(report_path.read_text())['steps']],
                                     ['PASS', 'PASS', 'PASS', failure] + ['NOT_RUN']*4)

    def test_host_only_report_explicitly_disclaims_runtime_coverage(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', return_value={'name': 'host', 'kind': 'host',
                                                                  'status': 'PASS', 'hashes_after': {}}),                     contextlib.redirect_stdout(io.StringIO()):
                code = report.main(['--host-only', '--report', str(report_path), '--log-dir', value])
            data = json.loads(report_path.read_text())
            self.assertEqual(code, 0)
            self.assertFalse(data['runtime_coverage'])
            self.assertIn('host checks only', data['evidence_scope']['overall'])

    def test_initial_hash_error_terminalizes_current_report(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'runtime_hashes', side_effect=OSError('controlled hash read failure')):
                with self.assertRaises(OSError):
                    report.main(['--report', str(report_path), '--log-dir', value])
            data = json.loads(report_path.read_text())
            self.assertEqual(data['status'], 'FAIL')
            self.assertIn('controlled hash read failure', data['error'])

    def test_manifest_error_replaces_previous_pass_report(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            report_path.write_text('{"status":"PASS"}')
            with patch.object(report.runner, 'load_manifest', side_effect=ValueError('controlled manifest')):
                with self.assertRaises(ValueError):
                    report.main(['--report', str(report_path), '--log-dir', value])
            self.assertEqual(json.loads(report_path.read_text())['status'], 'FAIL')

    def test_interrupt_during_terminal_write_replaces_pass_and_changes_exit(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            real_write = report.atomic_json
            fired = False
            def write(path, payload):
                nonlocal fired
                real_write(path, payload)
                if payload['status'] == 'PASS' and not fired:
                    fired = True
                    os.kill(os.getpid(), signal.SIGTERM)
            with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', return_value={'name': 'host', 'status': 'PASS',
                                                                  'hashes_after': {}}),                     patch.object(report, 'atomic_json', side_effect=write):
                with contextlib.redirect_stdout(io.StringIO()):
                    code = report.main(['--host-only', '--report', str(report_path), '--log-dir', value])
            self.assertEqual(code, 143)
            self.assertEqual(json.loads(report_path.read_text())['status'], 'ABORTED')


if __name__ == '__main__':
    unittest.main()
