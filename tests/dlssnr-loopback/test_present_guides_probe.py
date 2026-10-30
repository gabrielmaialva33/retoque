#!/usr/bin/env python3
"""Host-only regression tests for fail-closed present-guide evidence."""
import importlib.util
import tempfile
import argparse
import json
import shutil
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('present_guides_probe', Path(__file__).with_name('present_guides_probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Evidence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dll = self.root / 'OptiScaler.dll'
        self.dll.write_bytes(b'candidate')
        self.harness = self.root / 'dlssnr-loopback.log'
        self.log = self.root / 'OptiScaler.log'
        self.harness.write_text('MOVING-PRESENT delay_ms=30\nMOVING-SCENE PASS: evaluations=80 captures=16\n')
        self.log.write_text('DLSS-NR present guides: guide snapshot admitted via same-queue execution order (zero CPU wait)\n'
                            "DLSS-NR present guides, identity applied, D3D12 present: 4 presents in 1.0 s, lag limit enforced on 0 | 3 used the game's depth and motion, 0 refused\n")
        self.trace = self.root / 'steam-0.log'
        self.trace.write_text('trace:loaddll:build_module Loaded L\"Z:' + str(self.dll).replace('/', '\\') + '\" at 0x1: native\n')
        self.started_ns = 1
        self.expected = probe.hash_file(self.dll)

    def verify(self):
        return probe.validate_evidence(self.root, self.expected, self.started_ns)

    def test_probe_uses_bounded_present_delay(self):
        command = getattr(probe, 'harness_command', lambda *_: [])('/proton', self.root)
        self.assertEqual(command[-4:], ['--moving-scene', '--moving-present', '--moving-present-delay-ms', '30'])

    def test_old_harness_without_delay_evidence_rejected(self):
        self.harness.write_text('MOVING-SCENE PASS: evaluations=80 captures=16\n')
        with self.assertRaises(ValueError): self.verify()

    def test_harness_delay_argument_contract(self):
        source = Path(__file__).with_name('harness.cpp').read_text()
        moving = Path(__file__).with_name('moving_scene.h').read_text()
        self.assertIn('"--moving-present-delay-ms"', source)
        self.assertIn('movingPresentDelayMs = 0', source)
        self.assertIn('movingPresentDelayMs <= 100', source)
        self.assertIn('Require(!sawPresentDelay && i + 1 < argc', source)
        self.assertIn("Require(*value != '\\0'", source)
        self.assertIn("Require(*digit >= '0' && *digit <= '9'", source)
        self.assertIn('movingPresentDelayMs <= (100 - n) / 10', source)
        self.assertIn('movingPresentDelayMs >= 1', source)
        self.assertIn('hasFlag("--moving-scene") && hasFlag("--moving-present")', source)
        self.assertIn('argument == "--moving-present-delay-ms")', source)
        self.assertIn('hasFlag("--moving-present")', source)
        self.assertIn('Sleep(presentDelayMs);', moving)
        self.assertLess(moving.index('swapchain->Present(0, 0)'), moving.index('Sleep(presentDelayMs);'))
        self.assertIn('constexpr unsigned Width = 640, Height = 360, Warmup = 64, Count = 80;', moving)

    def test_live_present_accepted(self):
        self.assertGreater(self.verify()['guides_used'], 0)

    def test_run_refuses_missing_dll_before_creating_output(self):
        out = self.root / 'fresh'
        args = argparse.Namespace(dll=self.root / 'missing.dll', forwarder=self.dll, harness=self.dll,
                                  model=self.dll, sr=self.dll, core=self.dll, proton=self.dll,
                                  steam=self.root, out=out)
        with self.assertRaises(ValueError): probe.run_probe(args)
        self.assertFalse(out.exists())

    def test_missing_and_wrong_dll_rejected(self):
        self.dll.unlink()
        with self.assertRaises(ValueError): self.verify()
        self.dll.write_bytes(b'other')
        with self.assertRaises(ValueError): self.verify()

    def test_zero_capture_and_cold_pass_rejected(self):
        self.log.write_text('PRESENT-NR PASS: attempts=48 successes=48 presents=48\n')
        with self.assertRaises(ValueError): self.verify()
        self.log.write_text('DLSS-NR present guides: guide snapshot admitted via same-queue execution order (zero CPU wait)\n'
                            "DLSS-NR present guides, identity applied, D3D12 present: 4 presents in 1.0 s, lag limit enforced on 0 | 0 used the game's depth and motion, 0 refused\n")
        with self.assertRaises(ValueError): self.verify()

    def test_missing_or_wrong_loaded_module_rejected(self):
        self.trace.write_text('trace:loaddll:build_module Loaded L"Z:\\wrong\\OptiScaler.dll" at 0x1: native\n')
        with self.assertRaises(ValueError): self.verify()
        self.trace.write_text('trace:loaddll:build_module Loaded L"Z:' +
                              str(self.dll).replace('/', '\\') + '.other" at 0x1: native\n')
        with self.assertRaises(ValueError): self.verify()
        self.trace.unlink()
        with self.assertRaises(ValueError): self.verify()

    def test_comparison_rejects_input_changed_after_run(self):
        names = ('dlssnr-loopback.exe', 'nvngx_dlssnr.dll', 'nvngx_dlss.dll', '_nvngx.dll', 'scene.hlsl')
        for name in names:
            (self.root / name).write_bytes(name.encode())
        hashes = {name: probe.hash_file(self.root / name) for name in names}
        hashes.update({'OptiScaler.dll': self.expected, 'proton': 'proton-hash'})
        record = {'schema': 'retoque-present-guides-probe', 'version': 1, 'status': 'PASS',
                  'run_id': 'first', 'started_ns': 1, 'guides_used': 3, 'inputs_sha256': hashes,
                  'harness_args': ['OptiScaler.dll', '--moving-scene', '--moving-present',
                                   '--moving-present-delay-ms', '30']}
        (self.root / 'result.json').write_text(json.dumps(record))
        other = self.root / 'second'
        other.mkdir()
        for item in list(self.root.iterdir()):
            if item.is_file(): shutil.copyfile(item, other / item.name)
        (other / 'steam-0.log').write_text('trace:loaddll:build_module Loaded L"Z:' +
                                           str(other / 'OptiScaler.dll').replace('/', '\\') + '" at 0x1: native\n')
        record['run_id'] = 'second'
        (other / 'result.json').write_text(json.dumps(record))
        (other / 'nvngx_dlss.dll').write_bytes(b'changed')
        with self.assertRaises(ValueError): probe.compare(self.root, other)

    def test_old_and_missing_results_rejected(self):
        self.started_ns = max(self.log.stat().st_mtime_ns, self.harness.stat().st_mtime_ns) + 10**9
        with self.assertRaises(ValueError): self.verify()
        self.started_ns = 1
        self.harness.unlink()
        with self.assertRaises(ValueError): self.verify()


if __name__ == '__main__':
    unittest.main()
