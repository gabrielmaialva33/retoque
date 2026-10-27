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



class PlanAndReport(unittest.TestCase):
    def test_vulkan_plan_retains_sidecar_bytes_hashes_and_raw_validation_sources(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            expected = {}
            for name, count in zip(report.VK_CASES, (2, 6, 0, 6)):
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
            for name, count in zip(report.VK_CASES, (2, 6, 0, 6)):
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
            self.assertEqual(len(native_lines), 14)

    def test_plan_is_strict_serial_and_only_legacy_compilers_take_outer_lock(self):
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(report.parse_args(['--fg-steady-seconds', '15']), Path(value),
                                      {'a': {'tier': 'host'}})
        self.assertEqual([step.name for step in plan],
                         ['host', 'build', 'fg-inputs', 'nr-moving', 'nr-replay', 'vulkan', 'fg-provider'])
        self.assertIn('--strict', plan[0].command)
        self.assertEqual(plan[1].command[-1], 'Release')
        self.assertEqual([step.command[0] == 'flock' for step in plan],
                         [False, False, False, False, False, True, False])
        self.assertEqual(plan[-1].command[-2:], ['--steady-seconds', '15'])
        self.assertEqual(plan[-2].command[-2:], ['--only', ','.join(report.VK_CASES)])
        self.assertIn('-o', plan[5].command)

    def test_host_only_and_no_build_select_exact_work(self):
        with tempfile.TemporaryDirectory() as value:
            log_dir = Path(value)
            self.assertEqual([s.name for s in report.build_steps(report.parse_args(['--host-only']),
                                                                log_dir, {'a': {'tier': 'host'}})], ['host'])
            plan = report.build_steps(report.parse_args(['--no-build']), log_dir, {'a': {'tier': 'host'}})
            self.assertNotIn('build', [step.name for step in plan])
            self.assertNotIn('--steady-seconds', plan[-1].command)

    def test_failed_host_blocks_runtime_and_cannot_leave_old_pass_report(self):
        with tempfile.TemporaryDirectory() as value:
            report_path = Path(value)/'summary.json'
            report_path.write_text('{"status":"PASS"}')
            with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', return_value={'name': 'host', 'status': 'FAIL',
                                                                  'hashes_after': {}}) as execute:
                with contextlib.redirect_stdout(io.StringIO()):
                    code = report.main(['--report', str(report_path), '--log-dir', value])
            self.assertEqual(code, 1)
            self.assertEqual(execute.call_count, 1)
            data = json.loads(report_path.read_text())
            self.assertEqual(data['status'], 'FAIL')
            self.assertEqual([row['status'] for row in data['steps']], ['FAIL'] + ['NOT_RUN']*6)

    def test_any_runtime_nonpass_stops_remaining_plan_with_not_run_records(self):
        for failure in ('TIMEOUT', 'FAIL', 'INCONCLUSIVE'):
            with self.subTest(failure=failure):
                with tempfile.TemporaryDirectory() as value:
                    report_path = Path(value)/'summary.json'
                    results = [{'name': 'host', 'kind': 'host', 'status': 'PASS', 'hashes_after': {}},
                               {'name': 'build', 'kind': 'build', 'status': 'PASS', 'hashes_after': {}},
                               {'name': 'fg-inputs', 'kind': 'inputs', 'status': failure, 'hashes_after': {}}]
                    with patch.object(report.runner, 'load_manifest', return_value={'a': {'tier': 'host'}}),                     patch.object(report, 'run_step', side_effect=results) as execute,                     contextlib.redirect_stdout(io.StringIO()):
                        code = report.main(['--report', str(report_path), '--log-dir', value])
                    self.assertEqual(code, 1)
                    self.assertEqual(execute.call_count, 3)
                    self.assertEqual([row['status'] for row in json.loads(report_path.read_text())['steps']],
                                     ['PASS', 'PASS', failure] + ['NOT_RUN']*4)

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
