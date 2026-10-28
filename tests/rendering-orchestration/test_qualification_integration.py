"""Selected qualification modes must bind the invoked corpus and evidence paths."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('qualification_integration_report', ROOT/'tools/test-rendering.py')
report = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = report
spec.loader.exec_module(report)


def valid_replay():
    required = ('off-leaves-every-byte', 'off-records-nothing',
                'every-a-model-every-scored-frame', 'every-b-model-every-scored-frame',
                'cadence2-carries', 'cadence4-carries', 'adaptive-reports-intervals',
                'repeat-decisions-identical')
    variants = ('every-a', 'every-b', 'cadence2', 'cadence4', 'adaptive4')
    gates = {name: {'ok': True} for name in required}
    gates.update({f'{name}-no-scored-frame-skipped': {'ok': True} for name in variants})
    gates.update({f'{name}-scored-reset-runs-model': {'ok': True} for name in variants})
    return {'schema': 'retoque-replay-seam-report', 'version': 1,
            'provenance': {'dll_sha256': 'a'*64}, 'gates': gates,
            'corpus': {'schema': 'retoque-replay-corpus-summary', 'version': 1,
                       'profile': 'temporal-v1', 'frames': 480, 'scored_frames': 400,
                       'reset_frame_ids': [1, 361]},
            'judge': {'available': True, 'verdicts': {'cadence2': {'verdict': 'FAIL'}}}}


class QualificationIntegration(unittest.TestCase):
    def test_default_still_uses_short_replay_and_its_own_evidence(self):
        args = report.parse_args(['--no-build'])
        self.assertFalse(getattr(args, 'replay_quality', None))
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(args, Path(value), {'host-check': {'tier': 'host'}})
        replay = next(step for step in plan if step.kind == 'replay')
        self.assertIn('--replay-nr', replay.command)
        self.assertNotIn('--replay-quality', replay.command)
        self.assertEqual(replay.artifact_root, ROOT/'tests/dlssnr-loopback/artifacts/replay-run')
        self.assertEqual(replay.receipt.parent, replay.artifact_root)
        self.assertEqual(replay.entry['timeout'], 3600)
        self.assertEqual(getattr(replay, 'replay_profile', None), 'legacy')

    def test_temporal_opt_in_binds_command_receipt_logs_and_retention(self):
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(report.parse_args(['--no-build', '--replay-quality']),
                                      Path(value), {'host-check': {'tier': 'host'}})
        replay = next(step for step in plan if step.kind == 'replay')
        work = ROOT/'tests/dlssnr-loopback/artifacts/replay-quality-run'
        self.assertIn('--replay-quality', replay.command)
        self.assertNotIn('--replay-nr', replay.command)
        self.assertEqual(replay.artifact_root, work)
        self.assertEqual(replay.receipt, work/'replay-report.json')
        self.assertEqual(replay.log_paths, (work,))
        self.assertEqual(getattr(replay, 'replay_profile', None), 'temporal-v1')
        self.assertEqual(replay.entry['timeout'], 5400)
        self.assertTrue(all(step.artifact_root is None for step in plan if step is not replay))
        self.assertEqual(plan[0].name, 'graphics-preflight')

    def test_vulkan_scale125_is_required_even_if_all_other_cases_pass(self):
        self.assertEqual(set(report.VK_CASES),
                         {'nr', 'nr-route-quiet', 'nr-route-quiet-wdx12', 'nr-route-guides', 'nr-route-scale125'})
        data = {'status': 'PASS', 'dll_sha256': 'a'*64,
                'cases': {name: {'status': 'PASS', 'problems': []} for name in report.VK_CASES}}
        self.assertEqual(report.validate_vulkan(data, 'a'*64), [])
        data['cases'].pop('nr-route-scale125')
        self.assertTrue(report.validate_vulkan(data, 'a'*64))

    def test_temporal_receipt_requires_selected_profile_and_scored_cut(self):
        valid = valid_replay()
        self.assertEqual(report.validate_replay(valid, 'a'*64, profile='temporal-v1'), [])
        for updates in ({'profile': 'legacy'}, {'frames': 256}, {'scored_frames': 399},
                        {'reset_frame_ids': [1]}, {'version': True}):
            with self.subTest(updates=updates):
                data = copy.deepcopy(valid)
                data['corpus'].update(updates)
                self.assertTrue(report.validate_replay(data, 'a'*64, profile='temporal-v1'))
        data = copy.deepcopy(valid)
        del data['gates']['cadence4-scored-reset-runs-model']
        self.assertTrue(report.validate_replay(data, 'a'*64, profile='temporal-v1'))
        self.assertTrue(report.validate_replay(valid, 'a'*64, profile='legacy'))

    def test_requested_profile_is_recorded_even_when_preflight_blocks_runtime(self):
        with tempfile.TemporaryDirectory() as value:
            path = Path(value)/'summary.json'
            first = {'name': 'graphics-preflight', 'kind': 'preflight', 'status': 'FAIL', 'hashes_after': {}}
            with patch.object(report.runner, 'load_manifest', return_value={'host-check': {'tier': 'host'}}), \
                    patch.object(report, 'run_step', return_value=first) as run, \
                    patch.object(report, 'runtime_hashes', return_value={}), \
                    contextlib.redirect_stdout(io.StringIO()):
                code = report.main(['--no-build', '--replay-quality', '--report', str(path), '--log-dir', value])
            data = json.loads(path.read_text())
            self.assertEqual(code, 1)
            self.assertEqual(run.call_count, 1)
            self.assertEqual(data.get('requested_replay_profile'), 'temporal-v1')
            self.assertFalse(data['runtime_coverage'])
            self.assertEqual([row['status'] for row in data['steps']], ['FAIL'] + ['NOT_RUN']*6)


if __name__ == '__main__':
    unittest.main()
