#!/usr/bin/env python3
"""Adversarial report and ZIP fixtures; no DLL execution or release publication."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT/'tools/release_manifest.py'
release = None
if TOOL.exists():
    spec = importlib.util.spec_from_file_location('retoque_release_under_test', TOOL)
    release = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(release)

COMMIT = 'a' * 40
RUN = 'b' * 32
REQUIRED = {'alpha', 'beta'}


def report():
    return dict(schema='retoque-test-run', version=1, run_id=RUN, status='PASS', head=COMMIT,
                dirty=False, started_at='2026-10-29T12:00:00+00:00', finished_at='2026-10-29T12:01:00+00:00',
                selected=['alpha', 'beta'],
                counts=dict(PASS=2, FAIL=0, SKIP=0, TIMEOUT=0, ABORTED=0, INCONCLUSIVE=0),
                results=[dict(name=name, tier='host', status='PASS', returncode=0,
                              log='/private-machine/log.txt', evidence={'private': 'do-not-export'})
                         for name in ('alpha', 'beta')])


class ReleaseEvidence(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(release, 'release evidence tool is not implemented')
        temporary = tempfile.TemporaryDirectory(prefix='retoque-release-')
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.archive = self.tmp/'Retoque-v0.3.0.zip'

    def make_zip(self, extra=(), payload=b'dll-fixture'):
        with zipfile.ZipFile(self.archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for name, data in [('OptiScaler.dll', payload), ('nvngx.dll_dlssnr.dll', b'forwarder'),
                               ('OptiScaler.ini', b'[DlssNr]\nEnabled=auto\n'),
                               ('Licenses/RenoDX_ATTRIBUTION.txt', b'attribution'), *extra]:
                z.writestr(name, data)

    def validate(self, value):
        return release.validate_host_report(value, COMMIT, REQUIRED, RUN)

    def test_complete_report_sanitizes_machine_paths_and_raw_evidence(self):
        result = self.validate(report())
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['selected'], ['alpha', 'beta'])
        public = json.dumps(result)
        self.assertNotIn('/private-machine', public)
        self.assertNotIn('do-not-export', public)
        self.assertIn('source', result['scope'])

    def test_wrong_identity_dirty_unknown_or_nonterminal_reports_fail(self):
        mutations = [dict(schema='other'), dict(version=True), dict(head='c'*40), dict(dirty=True),
                     dict(dirty=None), dict(run_id=''), dict(run_id='c'*32), dict(status='RUNNING'),
                     dict(status='INCONCLUSIVE'), dict(finished_at=None),
                     dict(finished_at='2026-10-29T11:59:00+00:00')]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.validate(report() | mutation)

    def test_partial_duplicate_or_extra_selection_fails(self):
        for selected in (['alpha'], ['alpha', 'alpha'], ['alpha', 'beta', 'gamma'], []):
            with self.subTest(selected=selected), self.assertRaises(ValueError):
                self.validate(report() | {'selected': selected})

    def test_aggregate_pass_cannot_hide_bad_or_duplicate_suite_results(self):
        changes = [dict(status='SKIP'), dict(tier='wine'), dict(returncode=7), dict(returncode=None),
                   dict(name='beta')]
        for change in changes:
            value = report()
            value['results'][0].update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate(value)
        value = report()
        value['results'].pop()
        with self.assertRaises(ValueError):
            self.validate(value)

    def test_counts_are_recomputed_not_trusted(self):
        for counts in ({'PASS': 2}, dict(PASS=1, FAIL=0, SKIP=1, TIMEOUT=0, ABORTED=0, INCONCLUSIVE=0),
                       dict(PASS=True, FAIL=0, SKIP=0, TIMEOUT=0, ABORTED=0, INCONCLUSIVE=0)):
            with self.subTest(counts=counts), self.assertRaises(ValueError):
                self.validate(report() | {'counts': counts})

    def test_archive_identity_hashes_real_members(self):
        self.make_zip()
        inventory = release.archive_inventory(self.archive)
        self.assertEqual(inventory['sha256'], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        files = {item['path']: item for item in inventory['files']}
        self.assertEqual(files['OptiScaler.dll']['sha256'], hashlib.sha256(b'dll-fixture').hexdigest())
        self.assertEqual(files['OptiScaler.dll']['size'], len(b'dll-fixture'))
        old = inventory['sha256']
        self.make_zip(payload=b'changed')
        self.assertNotEqual(release.archive_inventory(self.archive)['sha256'], old)

    def test_unsafe_paths_collisions_and_model_redistribution_fail(self):
        bad_names = ('../outside.dll', '/absolute.dll', 'C:/bad.dll', 'a\\bad.dll', 'a//bad.dll',
                     'a/./bad.dll', 'optiscaler.DLL', 'nested/NVNGX_DLSSNR.DLL')
        for name in bad_names:
            self.make_zip([(name, b'bad')])
            with self.subTest(name=name), self.assertRaises(ValueError):
                release.archive_inventory(self.archive)

    def test_zip_symlink_and_empty_required_binary_fail(self):
        link = zipfile.ZipInfo('linked.dll')
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        self.make_zip([(link, b'outside')])
        with self.assertRaises(ValueError):
            release.archive_inventory(self.archive)
        self.make_zip(payload=b'')
        with self.assertRaises(ValueError):
            release.archive_inventory(self.archive)

    def test_missing_required_file_fails(self):
        with zipfile.ZipFile(self.archive, 'w') as z:
            z.writestr('OptiScaler.dll', b'not-enough')
        with self.assertRaises(ValueError):
            release.archive_inventory(self.archive)

    def test_size_limit_refuses_before_hashing_oversized_member(self):
        self.make_zip()
        from unittest.mock import patch
        with patch.object(release, 'MAX_MEMBER_BYTES', 4), self.assertRaises(ValueError):
            release.archive_inventory(self.archive)

    def cli(self, *args):
        return subprocess.run([sys.executable, str(TOOL), '--root', str(ROOT),
                               '--archive', str(self.archive), '--version', 'retoque-v0.3.0', *args],
                              capture_output=True, text=True, timeout=20)

    def test_local_manifest_marks_missing_host_and_runtime_explicitly(self):
        self.make_zip()
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        path = Path(str(self.archive) + '.manifest.json')
        manifest = json.loads(path.read_text())
        self.assertEqual(manifest['schema'], 'retoque-release-manifest')
        self.assertEqual(manifest['host_tests']['status'], 'NOT_SUPPLIED')
        self.assertEqual(manifest['runtime']['status'], 'NOT_RUN')
        self.assertEqual(manifest['build']['mode'], 'existing-output')
        self.assertEqual(manifest['source']['identity_scope'], 'packaging checkout, not inferred from DLLs')
        self.assertNotIn(str(ROOT), path.read_text())

    def test_required_host_missing_refuses_without_overwriting_output(self):
        self.make_zip()
        path = Path(str(self.archive) + '.manifest.json')
        path.write_text('keep previous evidence')
        result = self.cli('--require-host')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('host', result.stderr)
        self.assertEqual(path.read_text(), 'keep previous evidence')

    def test_supplied_invalid_report_is_never_ignored(self):
        self.make_zip()
        path = self.tmp/'report.json'
        path.write_text(json.dumps(report()))
        result = self.cli('--host-report', str(path))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(Path(str(self.archive) + '.manifest.json').exists())

    def test_output_cannot_overwrite_input_archive(self):
        self.make_zip()
        before = self.archive.read_bytes()
        result = self.cli('--output', str(self.archive))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.archive.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
