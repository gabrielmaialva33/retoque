#!/usr/bin/env python3
"""Host-only negative controls for native D3D11 DLL-backed evidence."""
import tempfile
import unittest
from pathlib import Path
import dx11_present_probe as probe

class Evidence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dll = self.root / 'OptiScaler.dll'
        self.dll.write_bytes(b'dll')
        self.digest = probe.hash_file(self.dll)
        self.harness = self.root / 'dx11-present.log'
        self.log = self.root / 'OptiScaler.log'
        self.trace = self.root / 'steam-0.log'
        self.harness.write_text('D3D11-PRESENT LOOP-COMPLETE: evaluations=200 presents=200 elapsed_ms=6300\n'
                                'D3D11-PRESENT PASS: evaluations=200 presents=200 elapsed_ms=6300\n')
        self.log.write_text("Creating new DLSS feature\nDLSS-NR native D3D11 guides: 100 evaluates in 2.0 s | 98 carried to the present pass\n"
                            "DLSS-NR present guides, identity applied, D3D11 bridge: 100 presents in 2.0 s, lag limit enforced on 0 | 90 used the game's depth and motion, 0 refused\n")
        self.trace.write_text('trace:loaddll:build_module Loaded L"Z:' + str(self.dll).replace('/', '\\') + '" at 0x1: native\n')
        self.started = 1

    def check(self):
        return probe.validate_evidence(self.root, self.digest, self.started)

    def test_harness_drives_public_dx11_and_exported_factory(self):
        source = Path(__file__).with_name('dx11_present_harness.cpp').read_text()
        for name in ('NVSDK_NGX_D3D11_Init', 'NVSDK_NGX_D3D11_AllocateParameters',
                     'NVSDK_NGX_D3D11_CreateFeature', 'NVSDK_NGX_D3D11_EvaluateFeature',
                     'NVSDK_NGX_D3D11_ReleaseFeature', 'NVSDK_NGX_D3D11_DestroyParameters',
                     'NVSDK_NGX_D3D11_Shutdown1'):
            self.assertIn('decltype(&' + name + ')', source)
            self.assertIn('"' + name + '"', source)
        for part in ('LoadLibraryA(argv[1])', '"CreateDXGIFactory2"',
                     'CreateSwapChainForHwnd(device, window', 'frame < 200', 'Sleep(30)',
                     'elapsed < 6000', 'swapchain->Present(0, 0)',
                     'NVSDK_NGX_Parameter_Color', 'NVSDK_NGX_Parameter_Output',
                     'NVSDK_NGX_Parameter_Depth', 'NVSDK_NGX_Parameter_MotionVectors'):
            self.assertIn(part, source)
        self.assertLess(source.index('CheckNgx(evaluate('), source.index('swapchain->Present(0, 0)'))

    def test_good(self):
        result = self.check()
        self.assertEqual((result['carried'], result['guides_used']), (98, 90))

    def test_missing_or_changed_dll(self):
        self.dll.unlink()
        with self.assertRaises(ValueError): self.check()
        self.dll.write_bytes(b'wrong')
        with self.assertRaises(ValueError): self.check()

    def test_no_carried_or_no_bridge_use(self):
        for text in ("DLSS-NR native D3D11 guides: carrying the game's depth and motion\n",
                     "DLSS-NR native D3D11 guides: 1 evaluates in 2.0 s | 0 carried to the present pass\n"
                     "DLSS-NR present guides, identity applied, D3D11 bridge: 1 presents | 1 used the game's depth and motion,\n",
                     "DLSS-NR native D3D11 guides: 1 evaluates in 2.0 s | 1 carried to the present pass\n"
                     "DLSS-NR present guides, identity applied, D3D11 bridge: 1 presents | 0 used the game's depth and motion,\n",
                     "DLSS-NR native D3D11 guides: 1 evaluates in 2.0 s | 1 carried to the present pass\n"
                     "DLSS-NR present guides, identity applied, D3D12 present: 1 presents | 1 used the game's depth and motion,\n"):
            self.log.write_text('Creating new DLSS feature\n' + text)
            with self.assertRaises(ValueError): self.check()

    def test_stale_missing_or_wrong_loader(self):
        self.started = self.log.stat().st_mtime_ns + 10**9
        with self.assertRaises(ValueError): self.check()
        self.started = 1
        self.trace.write_text('Loaded L"Z:\\wrong\\OptiScaler.dll" at 0x1: native\n')
        with self.assertRaises(ValueError): self.check()
        self.harness.unlink()
        with self.assertRaises(ValueError): self.check()

    def test_dlss_selection_and_failed_creation_are_not_hidden(self):
        original = self.log.read_text()
        self.log.write_text(original.replace('Creating new DLSS feature', 'Creating new FSR 2.2.1 feature'))
        with self.assertRaises(ValueError): self.check()
        self.log.write_text(original + 'CreateFeature failed\n')
        with self.assertRaises(ValueError): self.check()
        self.log.write_text(original + 'changing backend to FSR 2.2.1\n')
        with self.assertRaises(ValueError): self.check()

    def test_pass_mixed_with_failure_is_rejected(self):
        self.harness.write_text('D3D11-PRESENT FAIL: stage=Evaluate code=1\n'
                                'D3D11-PRESENT PASS: evaluations=200 presents=200 elapsed_ms=6300\n')
        with self.assertRaises(ValueError): self.check()

    def test_short_window_or_incomplete_frames_rejected(self):
        for line in ('D3D11-PRESENT PASS: evaluations=200 presents=200 elapsed_ms=5999\n',
                     'D3D11-PRESENT PASS: evaluations=199 presents=200 elapsed_ms=6300\n',
                     'D3D11-PRESENT PASS: evaluations=200 presents=0 elapsed_ms=6300\n'):
            self.harness.write_text(line.replace('D3D11-PRESENT PASS:', 'D3D11-PRESENT LOOP-COMPLETE:') + line)
            with self.assertRaises(ValueError): self.check()

    def test_completed_loop_without_success_is_not_pass(self):
        self.harness.write_text('D3D11-PRESENT LOOP-COMPLETE: evaluations=200 presents=200 elapsed_ms=6300\n'
                                'D3D11-PRESENT FAIL: stage=ReleaseFeature code=1\n')
        with self.assertRaises(ValueError): self.check()
        self.harness.write_text('D3D11-PRESENT LOOP-COMPLETE: evaluations=200 presents=200 elapsed_ms=6300\n')
        with self.assertRaises(ValueError): self.check()

    def test_pass_without_current_loop_marker_is_rejected(self):
        self.harness.write_text('D3D11-PRESENT PASS: evaluations=200 presents=200 elapsed_ms=6300\n')
        with self.assertRaises(ValueError): self.check()

    def test_cleanup_boundaries_are_logged_in_order(self):
        source = Path(__file__).with_name('dx11_present_harness.cpp').read_text()
        self.assertIn('setvbuf(logFile, nullptr, _IONBF, 0)', source)
        for mark in ('Stage("Init", "begin")', 'Stage("D3D11_Init", "before")',
                     'Stage("D3D11_Init", "after")', 'Stage("CreateFeature", "before")',
                     'Stage("CreateFeature", "after")', 'Stage("Run", "begin")'):
            self.assertIn(mark, source)
        stages = ('Fence', 'ReleaseFeature', 'DestroyParameters', 'Shutdown1',
                  'ReleaseSwapchain', 'ReleaseContext', 'ReleaseDevice', 'DestroyWindow')
        positions = []
        for stage in stages:
            before = f'Stage("{stage}", "before")'
            after = f'Stage("{stage}", "after")'
            self.assertIn(before, source)
            self.assertIn(after, source)
            positions.extend((source.index(before), source.index(after)))
        self.assertEqual(positions, sorted(positions))
        self.assertLess(source.index('D3D11-PRESENT LOOP-COMPLETE:'), positions[0])
        self.assertLess(positions[-1], source.index('D3D11-PRESENT PASS:'))

    def test_harness_failure_cannot_pass(self):
        self.harness.write_text('D3D11-PRESENT FAIL: preflight refused\n')
        with self.assertRaises(ValueError): self.check()

if __name__ == '__main__':
    unittest.main()
