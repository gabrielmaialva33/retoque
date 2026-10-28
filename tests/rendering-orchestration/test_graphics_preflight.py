"""Inherited X11 connection checks and owned supervision, without a desktop/GPU."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from test_orchestration import report


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT/'tools/graphics_preflight.py'


class GraphicsProbe(unittest.TestCase):
    def helper(self):
        self.assertTrue(HELPER.is_file(), 'the supervised graphics preflight helper is missing')
        spec = importlib.util.spec_from_file_location('graphics_preflight_under_test', HELPER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def library(self, events, *, connected=True):
        class Function:
            def __init__(self, function):
                self.function = function
            def __call__(self, *args):
                return self.function(*args)
        class Library:
            pass
        library = Library()
        def opened(name):
            events.append(('open', name))
            return 12345 if connected else None
        def closed(handle):
            events.append(('close', handle))
            return 0
        library.XOpenDisplay = Function(opened)
        library.XCloseDisplay = Function(closed)
        return library

    def test_exact_inherited_display_is_opened_and_closed_without_environment_changes(self):
        helper = self.helper()
        events = []
        env = {'DISPLAY': ':77.3', 'WAYLAND_DISPLAY': 'wayland-retained', 'XAUTHORITY': '/retained/auth'}
        with patch.dict(os.environ, env, clear=True):
            before = dict(os.environ)
            result = helper.probe_display(os.environ, load_library=lambda _name: self.library(events))
            self.assertEqual(dict(os.environ), before)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['display'], ':77.3')
        self.assertEqual(result['backend'], 'x11')
        self.assertFalse(result['runtime_coverage'])
        self.assertIn('X11', result['scope'])
        self.assertEqual(events, [('open', b':77.3'), ('close', 12345)])

    def test_present_but_refused_display_fails_without_another_connection_attempt(self):
        helper = self.helper()
        events = []
        result = helper.probe_display({'DISPLAY': ':77'},
                                      load_library=lambda _name: self.library(events, connected=False))
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(result['connected'])
        self.assertEqual(events, [('open', b':77')])
        self.assertIn(':77', result['reason'])

    def test_missing_display_and_missing_library_are_terminal_failures(self):
        helper = self.helper()
        def unavailable(_name):
            raise OSError('controlled missing X11 library')
        for env in ({}, {'DISPLAY': ''}, {'DISPLAY': ':77'}):
            with self.subTest(env=env):
                result = helper.probe_display(env, load_library=unavailable)
                self.assertEqual(result['status'], 'FAIL')
                self.assertFalse(result['connected'])
                self.assertFalse(result['runtime_coverage'])

    def test_real_helper_without_display_writes_current_terminal_receipt(self):
        self.helper()
        with tempfile.TemporaryDirectory() as value:
            receipt = Path(value)/'result.json'
            env = dict(os.environ, DISPLAY='', RETOQUE_TEST_RUN_ID='controlled-invocation')
            result = subprocess.run([sys.executable, str(HELPER), '--report', str(receipt)],
                                    env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
            data = json.loads(receipt.read_text())
            self.assertEqual(data['schema'], 'retoque-suite-result')
            self.assertIs(type(data['version']), int)
            self.assertEqual(data['version'], 1)
            self.assertEqual(data['run_id'], 'controlled-invocation')
            self.assertEqual(data['status'], 'FAIL')
            self.assertFalse(data['runtime_coverage'])
            self.assertIn('X11', result.stdout)


class GraphicsSupervision(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root/'tests/fixture').mkdir(parents=True)
        (self.root/'x64/out').mkdir(parents=True)
        for filename in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll'):
            (self.root/'x64/out'/filename).write_bytes(filename.encode())
        self.receipt = self.root/'receipt.json'
        self.logs = self.root/'logs'
        self.logs.mkdir()
        for target, attribute, value in ((report, 'ROOT', self.root), (report.runner, 'ROOT', self.root),
                                         (report.runner, 'TESTS', self.root/'tests')):
            self.enterContext(patch.object(target, attribute, value))
        self.expected = report.runtime_hashes()

    def step(self, code, timeout=5):
        return report.Step('graphics-preflight', [sys.executable, '-c', code],
                           {'path': 'fixture', 'tier': 'host', 'requires': [], 'timeout': timeout},
                           'preflight', self.receipt)

    def emitter(self, **changes):
        payload = {'schema': 'retoque-suite-result', 'version': 1, 'status': 'PASS',
                   'backend': 'x11', 'display': ':77', 'probe': 'XOpenDisplay/XCloseDisplay',
                   'connected': True, 'runtime_coverage': False}
        payload.update(changes)
        return ('import json,os;from pathlib import Path;'
                f'data={payload!r};data.setdefault("run_id",os.environ["RETOQUE_TEST_RUN_ID"]);'
                f'Path({str(self.receipt)!r}).write_text(json.dumps(data))')

    def test_successful_preflight_does_not_require_production_dll_hashes(self):
        result = report.run_step(self.step(self.emitter()), self.logs, {})
        self.assertEqual(result['status'], 'PASS', result['findings'])
        self.assertEqual(result['evidence']['run_id'], result['run_id'])
        self.assertFalse(result['evidence']['runtime_coverage'])

    def test_zero_exit_cannot_pass_with_stale_or_malformed_preflight_receipt(self):
        for changed in ({'run_id': 'stale'}, {'schema': 'other'}, {'version': True}, {'version': 2},
                        {'status': 'RUNNING'}, {'runtime_coverage': True}, {'connected': False},
                        {'backend': 'wayland'}, {'display': ''}):
            with self.subTest(changed=changed):
                result = report.run_step(self.step(self.emitter(**changed)), self.logs, self.expected)
                self.assertEqual(result['returncode'], 0)
                self.assertEqual(result['status'], 'FAIL')
                self.assertTrue(result['findings'])

    def test_timeout_preserves_terminal_status_even_without_receipt(self):
        result = report.run_step(self.step('import time;time.sleep(60)', timeout=.05),
                                 self.logs, self.expected)
        self.assertEqual(result['status'], 'TIMEOUT')
        self.assertEqual(result['runner_status'], 'TIMEOUT')
        self.assertLess(result['returncode'], 0)
        self.assertLess(result['seconds'], 7)

    def test_interruption_preserves_terminal_status_even_without_receipt(self):
        ready = self.root/'ready'
        stop = threading.Event()
        def interrupt():
            deadline = time.monotonic()+5
            while not ready.exists() and time.monotonic()<deadline:
                time.sleep(.01)
            stop.set()
        watcher = threading.Thread(target=interrupt)
        watcher.start()
        try:
            code = ('import time;from pathlib import Path;'
                    f'Path({str(ready)!r}).write_text("ready");time.sleep(60)')
            result = report.run_step(self.step(code), self.logs, self.expected, stop=stop)
        finally:
            watcher.join(timeout=6)
        self.assertTrue(ready.exists())
        self.assertEqual(result['status'], 'ABORTED')
        self.assertEqual(result['runner_status'], 'ABORTED')
        self.assertLess(result['returncode'], 0)


class GraphicsPlan(unittest.TestCase):
    def test_full_plan_preflights_before_host_build_or_window_work(self):
        with tempfile.TemporaryDirectory() as value:
            plan = report.build_steps(report.parse_args([]), Path(value), {'host': {'tier': 'host'}})
            self.assertEqual([step.name for step in plan[:3]], ['graphics-preflight', 'host', 'build'])
            self.assertEqual(plan[0].kind, 'preflight')
            self.assertEqual(plan[0].entry['timeout'], 8)
            self.assertEqual(plan[0].command[:2], [sys.executable, str(HELPER)])
            self.assertEqual([step.name for step in report.build_steps(
                report.parse_args(['--host-only']), Path(value), {'host': {'tier': 'host'}})], ['host'])

    def test_missing_display_terminalizes_real_full_plan_before_later_steps(self):
        self.assertTrue(HELPER.is_file(), 'the supervised graphics preflight helper is missing')
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            (root/'tools').mkdir()
            (root/'tests/rendering-orchestration').mkdir(parents=True)
            shutil.copy2(HELPER, root/'tools/graphics_preflight.py')
            shutil.copy2(ROOT/'tests/_suite_result.py', root/'tests/_suite_result.py')
            summary = root/'summary.json'
            with patch.object(report, 'ROOT', root), patch.object(report.runner, 'ROOT', root), \
                    patch.object(report.runner, 'TESTS', root/'tests'), \
                    patch.object(report.runner, 'load_manifest', return_value={'host': {'tier': 'host'}}), \
                    patch.dict(os.environ, {'DISPLAY': ''}), contextlib.redirect_stdout(io.StringIO()):
                code = report.main(['--no-build', '--report', str(summary), '--log-dir', str(root/'logs')])
            data = json.loads(summary.read_text())
            self.assertEqual(code, 1)
            self.assertEqual(data['status'], 'FAIL')
            self.assertFalse(data['runtime_coverage'])
            self.assertEqual(data['steps'][0]['name'], 'graphics-preflight')
            self.assertEqual(data['steps'][0]['status'], 'FAIL')
            self.assertEqual(data['steps'][0]['evidence']['run_id'], data['steps'][0]['run_id'])
            self.assertTrue(all(row['status'] == 'NOT_RUN' for row in data['steps'][1:]))


if __name__ == '__main__':
    unittest.main()
