"""Exercise the real overlay instrumenter against production source, without Wine."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('overlay_runner', ROOT / 'tests/vulkan-overlay/run.py')
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


class OverlayInstrumentation(unittest.TestCase):
    def instrument(self, source):
        def git(args, **kwargs):
            if args[1] == 'show':
                return source
            if args[1] == 'diff':
                return b''
            if args[1] == 'rev-parse':
                return 'fixture-revision\n'
            self.fail(f'unexpected git call: {args}')
        with tempfile.TemporaryDirectory() as value, patch.object(overlay, 'OUT', Path(value)), \
                patch.object(overlay.subprocess, 'check_output', side_effect=git):
            overlay.instrument('HEAD')
            return (Path(value) / 'menu_overlay_vk.instrumented.cpp').read_text()

    def test_current_definition_keeps_one_entry_counter_and_all_controls(self):
        source = (ROOT / 'OptiScaler/menu/menu_overlay_vk.cpp').read_text()
        result = self.instrument(source)
        self.assertIn('static bool DestroyVulkanObjectsLocked(bool shutdown, bool deviceDying, '
                      'std::optional<bool> drainProof)\n{\n    ++VkLifetimeProbe::stats.teardownCalls;', result)
        for counter, expected in (('teardownCalls', 1), ('createCalls', 1), ('destroyCalls', 1),
                                   ('presentCalls', 1), ('overlaySubmits', 2), ('crossFamilyDraws', 1),
                                   ('fenceWaits', 1)):
            self.assertEqual(result.count('++VkLifetimeProbe::stats.' + counter + ';'), expected)

    def test_changed_or_duplicated_signature_cannot_silently_lose_coverage(self):
        source = (ROOT / 'OptiScaler/menu/menu_overlay_vk.cpp').read_text()
        signature = 'static bool DestroyVulkanObjectsLocked(bool shutdown, bool deviceDying, std::optional<bool> drainProof)\n{'
        for broken in (source.replace(signature, signature.replace('bool deviceDying', 'int deviceDying')),
                       source + '\n' + signature + '\n}'):
            with self.assertRaisesRegex(RuntimeError, 'instrumentation anchor changed'):
                self.instrument(broken)
