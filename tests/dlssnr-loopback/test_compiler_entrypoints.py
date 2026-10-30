#!/usr/bin/env python3
import fcntl
import importlib.util
import os
from pathlib import Path
import sys
import subprocess
import time
import signal
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
def load(path, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class Entrypoints(unittest.TestCase):
    def test_busy_lock_refuses_before_spawn_for_all_entrypoints(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = dict(os.environ, XDG_RUNTIME_DIR=tmp)
            with patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
                with (root / 'msvc-prefix.lock').open('a') as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    for path in ('tests/dlssnr-loopback/run.py', 'tools/lsfg-study/harness/run.py',
                                 'tools/lsfg-study/ctxbench/run.py'):
                        module = load(path, 'entry_' + str(len(path)))
                        marker = root / 'started'
                        marker.unlink(missing_ok=True)
                        cmd = [sys.executable, '-c', f"from pathlib import Path; Path({str(marker)!r}).touch()"]
                        with self.subTest(path=path), patch('time.sleep', lambda _: None):
                            with self.assertRaises((RuntimeError, SystemExit)):
                                if path.startswith('tests/'):
                                    module.run_watched(cmd, env=env, log=root / 'loop.log')
                                else:
                                    module.command(cmd, env, root / 'lsfg.log', root, 2)
                            self.assertFalse(marker.exists())

    def test_timeout_kills_only_owned_fake_compilers_and_retains_log(self):
        from tools import build_local
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
            root = Path(tmp)
            unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)', 'cl.exe'])
            self.addCleanup(lambda: unrelated.wait(timeout=3))
            self.addCleanup(lambda: unrelated.poll() is None and unrelated.terminate())
            for path in ('tests/dlssnr-loopback/run.py', 'tools/lsfg-study/harness/run.py',
                         'tools/lsfg-study/ctxbench/run.py'):
                module = load(path, 'fake_' + str(len(path)))
                log = root / (str(len(path)) + '.log')
                childfile = root / 'child'
                childfile.unlink(missing_ok=True)
                script = ("import subprocess,sys,time; from pathlib import Path; "
                          "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)','cl.exe']); "
                          f"Path({str(childfile)!r}).write_text(str(p.pid)); "
                          "print('compiler-start',flush=True); time.sleep(60)")
                def bounded(*args, **kwargs):
                    kwargs.update(inactivity_timeout=.3, hard_timeout=2, poll_interval=.02, term_grace=.1,
                                  cpu_progress_filter=None, waited_cpu_progress_filter=None)
                    return build_local.run_step(*args, **kwargs)
                with self.subTest(path=path), patch.object(module, 'run_step', bounded):
                    with self.assertRaises((SystemExit, RuntimeError)):
                        if path.startswith('tests/'):
                            module.run_watched([sys.executable, '-c', script], env=dict(os.environ), log=log)
                        else:
                            module.command([sys.executable, '-c', script], dict(os.environ), log, root, 2)
                    self.assertIn('compiler-start', log.read_text())
                    self.assertIn('timeout', log.read_text())
                    self.assertIsNone(unrelated.poll())
                    pid = int(childfile.read_text())
                    stat = Path(f'/proc/{pid}/stat')
                    self.assertTrue(not stat.exists() or stat.read_text().split(') ', 1)[1][0] == 'Z')
                    with (root / 'msvc-prefix.lock').open('a') as lock:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_existing_fixture_lease_is_not_reacquired(self):
        module = load('tests/dlssnr-loopback/run.py', 'nested_lease')
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
            root = Path(tmp)
            with (root / 'msvc-prefix.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.object(module, 'run_step', return_value=0) as run:
                    module.run_watched(['fake'], env=dict(os.environ), log=root / 'log', lease_held=True)
                    run.assert_called_once()

    def test_nonzero_is_propagated_without_retry(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {'XDG_RUNTIME_DIR': tmp}):
            root = Path(tmp)
            for path in ('tests/dlssnr-loopback/run.py', 'tools/lsfg-study/harness/run.py',
                         'tools/lsfg-study/ctxbench/run.py'):
                module = load(path, 'fail_' + str(len(path)))
                marker = root / 'calls'
                marker.unlink(missing_ok=True)
                script = f"from pathlib import Path; p=Path({str(marker)!r}); p.write_text(p.read_text()+'x' if p.exists() else 'x'); print('failure evidence'); raise SystemExit(23)"
                log = root / 'failure.log'
                with self.subTest(path=path), self.assertRaises((SystemExit, RuntimeError)) as raised:
                    if path.startswith('tests/'):
                        module.run_watched([sys.executable, '-c', script], env=dict(os.environ), log=log)
                    else:
                        module.command([sys.executable, '-c', script], dict(os.environ), log, root, 3)
                self.assertIn('23', str(raised.exception))
                self.assertEqual(marker.read_text(), 'x')
                self.assertIn('failure evidence', log.read_text())

    def test_interrupt_cleans_owned_child_and_releases_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            env = dict(os.environ, XDG_RUNTIME_DIR=tmp, PYTHONPATH=str(ROOT))
            for path in ('tests/dlssnr-loopback/run.py', 'tools/lsfg-study/harness/run.py',
                         'tools/lsfg-study/ctxbench/run.py'):
                marker = root / 'pid'
                marker.unlink(missing_ok=True)
                log = root / 'interrupt.log'
                compiler = "import os,time;from pathlib import Path; " + f"Path({str(marker)!r}).write_text(str(os.getpid())); " + "print('started',flush=True);time.sleep(60)"
                launch = ("import importlib.util,sys;from pathlib import Path; "
                          f"spec=importlib.util.spec_from_file_location('entry',{str(ROOT / path)!r}); "
                          "m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m); "
                          f"args=[sys.executable,'-c',{compiler!r}]; " +
                          (f"m.run_watched(args,env=dict(__import__('os').environ),log=Path({str(log)!r}))"
                           if path.startswith('tests/') else
                           f"m.command(args,dict(__import__('os').environ),Path({str(log)!r}),Path({str(root)!r}),60)"))
                child = subprocess.Popen([sys.executable, '-c', launch], env=env,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    deadline = time.monotonic() + 5
                    while not marker.exists() and time.monotonic() < deadline:
                        time.sleep(.02)
                    self.assertTrue(marker.exists(), path)
                    pid = int(marker.read_text())
                    child.send_signal(signal.SIGTERM)
                    self.assertNotEqual(child.wait(timeout=8), 0)
                    stat = Path(f'/proc/{pid}/stat')
                    self.assertTrue(not stat.exists() or stat.read_text().split(') ', 1)[1][0] == 'Z')
                    self.assertIn('interrupted', log.read_text())
                    with (root / 'msvc-prefix.lock').open('a') as lock:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()

if __name__ == '__main__':
    unittest.main()
