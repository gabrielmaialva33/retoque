"""Loopback's actual nested supervision and interrupt guard, without GPU work."""
import contextlib
import importlib.util
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

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests'))
import run_all as runner
spec = importlib.util.spec_from_file_location('loopback_moving_cleanup', ROOT/'tests/dlssnr-loopback/run_moving.py')
moving = importlib.util.module_from_spec(spec)
spec.loader.exec_module(moving)
REAL_PROC = (os.name == 'posix' and Path('/proc/self/stat').exists()
             and int(Path('/proc/self/stat').read_text().split(' ', 1)[0]) == os.getpid())
PROC_REASON = 'requires native Linux process identities; remote native nested cleanup test required'


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()[0] != 'Z'
    except FileNotFoundError:
        return False


class LoopbackCleanup(unittest.TestCase):
    def test_exception_never_reaps_child_before_final_group_kill(self):
        events = []
        class Child:
            pid, returncode = 123456, None
            def wait(self, timeout=None):
                if not events:
                    events.append('WAIT')
                    raise subprocess.TimeoutExpired(['controlled'], timeout)
                if events[-1] != 'KILL':
                    raise AssertionError('leader reaped before final group kill')
                self.returncode = -9
                return -9
        def group_signal(_pid, number):
            events.append('TERM' if number == signal.SIGTERM else 'KILL')
        with tempfile.TemporaryDirectory() as value, \
                patch.object(moving.subprocess, 'Popen', return_value=Child()), \
                patch.object(runner.os, 'killpg', side_effect=group_signal), \
                patch.object(runner.time, 'monotonic', side_effect=[0, 0, 3]), \
                patch.object(runner.time, 'sleep'), patch.object(runner, '_group_members', return_value={123456: 1}):
            with self.assertRaises(subprocess.TimeoutExpired):
                moving.execute(['controlled'], cwd=value, env={}, log=Path(value)/'log', timeout=.1)
        self.assertEqual(events, ['WAIT', 'TERM', 'KILL'])

    def test_reaped_child_never_authorizes_group_signalling(self):
        class Child:
            pid, returncode = 123456, None
            def wait(self, timeout=None):
                self.returncode = 0
                raise KeyboardInterrupt
        with tempfile.TemporaryDirectory() as value, \
                patch.object(moving.subprocess, 'Popen', return_value=Child()), \
                patch.object(moving.os, 'killpg', side_effect=AssertionError('reaped group is not owned')):
            with self.assertRaises(KeyboardInterrupt):
                moving.execute(['controlled'], cwd=value, env={}, log=Path(value)/'log', timeout=.1)

    def test_nondefault_sigchld_refuses_before_launch(self):
        with tempfile.TemporaryDirectory() as value, \
                patch.object(moving.signal, 'getsignal', return_value=signal.SIG_IGN), \
                patch.object(moving.subprocess, 'Popen', side_effect=AssertionError('must not launch')):
            with self.assertRaises(RuntimeError):
                moving.execute(['controlled'], cwd=value, env={}, log=Path(value)/'log', timeout=.1)

    def stage(self, root):
        fixture = root/'tests/fixture'
        fixture.mkdir(parents=True)
        for filename in ('run.py', 'run_moving.py'):
            shutil.copy2(ROOT/'tests/dlssnr-loopback'/filename, fixture/filename)
        for filename in ('run_all.py', '_suite_result.py'):
            shutil.copy2(ROOT/'tests'/filename, root/'tests'/filename)
        (root/'tools').mkdir()
        shutil.copy2(ROOT/'tools/build_local.py', root/'tools/build_local.py')
        return fixture

    def entry_code(self, fixture, body):
        return ('import contextlib,os,runpy,signal,sys\n'
                f'sys.path.insert(0,{str(fixture)!r})\nimport run_moving\n'
                '@contextlib.contextmanager\ndef lease(_work):\n'
                + ''.join('    '+line+'\n' for line in body.splitlines())
                + '    yield\nrun_moving.lease=lease\n'
                f'sys.argv=[{str(fixture/"run.py")!r},"--moving-scene"]\n'
                f'runpy.run_path({str(fixture/"run.py")!r},run_name="__main__")\n')

    def test_actual_entrypoint_records_sigterm_without_running_gpu_main(self):
        with tempfile.TemporaryDirectory() as value:
            fixture = self.stage(Path(value))
            result = subprocess.run([sys.executable, '-c', self.entry_code(
                fixture, 'os.kill(os.getpid(),signal.SIGTERM)')], capture_output=True, text=True, timeout=6)
            self.assertEqual(result.returncode, 143, result.stdout+result.stderr)
            receipt = json.loads((fixture/'artifacts/interruption-result.json').read_text())
            self.assertEqual(receipt['status'], 'ABORTED')

    def test_success_does_not_create_an_authoritative_legacy_replacement(self):
        with tempfile.TemporaryDirectory() as value:
            fixture = self.stage(Path(value))
            result = subprocess.run([sys.executable, '-c', self.entry_code(
                fixture, 'raise SystemExit(0)')], capture_output=True, text=True, timeout=6)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertFalse((fixture/'artifacts/interruption-result.json').exists())

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_nested_timeout_cleans_detached_group_and_preserves_unrelated_process(self):
        self.nested('TIMEOUT')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_nested_interrupt_cleans_detached_group_and_preserves_unrelated_process(self):
        self.nested('ABORTED')

    def nested(self, expected):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            fixture = self.stage(root)
            marker = root/'nested-pids.json'
            ready = root/'grandchild-ready'
            grandchild = ('import signal,time;from pathlib import Path;'
                          'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
                          f'Path({str(ready)!r}).write_text("ready");time.sleep(60)')
            child_code = ('import json,os,signal,subprocess,sys,time\nfrom pathlib import Path\n'
                          'signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                          f'p=subprocess.Popen([sys.executable,"-c",{grandchild!r}])\n'
                          f'while not Path({str(ready)!r}).exists(): time.sleep(.005)\n'
                          f'Path({str(marker)!r}).write_text(json.dumps([os.getpid(),p.pid]))\n'
                          'time.sleep(60)')
            body = ('from pathlib import Path\n'
                    f'run_moving.execute([sys.executable,"-c",{child_code!r}],cwd={str(root)!r},'
                    f'env=dict(os.environ),log=Path({str(root/"inner.log")!r}),timeout=60)')
            command = [sys.executable, '-c', self.entry_code(fixture, body)]
            sibling = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(60)'], start_new_session=True)
            stop = threading.Event()
            def request_stop():
                deadline = time.monotonic()+5
                while not marker.exists() and time.monotonic()<deadline:
                    time.sleep(.01)
                stop.set()
            watcher = threading.Thread(target=request_stop) if expected == 'ABORTED' else None
            if watcher:
                watcher.start()
            try:
                with patch.object(runner, 'ROOT', root), patch.object(runner, 'TESTS', root/'tests'):
                    result = runner.run_suite('fixture', {'tier':'host','requires':[],
                                                          'timeout':.8 if expected=='TIMEOUT' else 10},
                                              root, False, command=command, stop=stop)
                self.assertEqual(result.status, expected)
                self.assertTrue(marker.exists())
                pids = json.loads(marker.read_text())
                deadline = time.monotonic()+2
                while any(alive(pid) for pid in pids) and time.monotonic()<deadline:
                    time.sleep(.02)
                self.assertFalse(any(alive(pid) for pid in pids), 'detached owned group survived outer cleanup')
                self.assertIsNone(sibling.poll(), 'unrelated sibling was signalled')
                receipt = json.loads((fixture/'artifacts/interruption-result.json').read_text())
                self.assertEqual(receipt['status'], 'ABORTED')
            finally:
                if watcher:
                    watcher.join(timeout=6)
                sibling.terminate()
                sibling.wait(timeout=3)
                if marker.exists():
                    for pid in json.loads(marker.read_text()):
                        if alive(pid):
                            os.kill(pid, signal.SIGKILL)


if __name__ == '__main__':
    unittest.main()
