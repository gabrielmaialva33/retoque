#!/usr/bin/env python3
"""Real subprocess regressions; no Wine, SDK, GPU or third-party packages."""
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'tools/build_local.py'
REAL_PROC = int(Path('/proc/self/stat').read_text().split(' ', 1)[0]) == os.getpid()
PROC_REASON = 'procfs PID namespace differs from os.getpid; run these cases on native Linux'


def alive(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1][0] != 'Z'
    except FileNotFoundError:
        return False


class BuildLifecycle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='retoque-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in ('tools', 'OptiScaler', 'runtime', 'bin', 'x64/Release', 'x64/out'):
            (self.root / directory).mkdir(parents=True)
        shutil.copy2(ROOT / 'tools/build-local.sh', self.root / 'tools/build-local.sh')
        if HELPER.exists():
            shutil.copy2(HELPER, self.root / 'tools/build_local.py')
        self.env = dict(os.environ, MSVC_BIN=str(self.root / 'bin'),
                        WINEPREFIX=str(self.root / 'prefix'), XDG_RUNTIME_DIR=str(self.root / 'runtime'))
        self.stamp = self.root / 'OptiScaler/resource_build_commit.h'
        self.stamp.write_text('existing-build\n')
        self.compiler("""
import json, os, sys
from pathlib import Path
with Path('calls.jsonl').open('a') as f:
    f.write(json.dumps({'args': sys.argv[1:], 'prefix': os.environ['WINEPREFIX'], 'debug': os.environ['WINEDEBUG']})+'\\n')
project = sys.argv[1]
configuration = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('/p:Configuration='))
output = Path('x64')/configuration
output.mkdir(parents=True, exist_ok=True)
name = 'nvngx.dll_dlssnr.dll' if 'forwarder' in project else 'OptiScaler.dll'
if os.environ.get('FAIL_STEP') == ('first' if 'forwarder' in project else 'second'):
    print('intentional compiler failure', flush=True)
    sys.exit(23)
if os.environ.get('OMIT') != name:
    (output/name).write_text('new-'+name)
print('compiled '+name, flush=True)
""")

    def compiler(self, body):
        compiler = self.root / 'bin/msbuild'
        compiler.write_text('#!/usr/bin/env python3\n'+body)
        compiler.chmod(0o755)

    def invoke(self, args=(), timeout=5):
        child = subprocess.Popen(['bash', str(self.root/'tools/build-local.sh'), *args], cwd=self.root,
                                 env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 start_new_session=True, text=True)
        try:
            output, _ = child.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            output, _ = child.communicate(timeout=3)
            return 124, output
        return child.returncode, output

    def old_pair(self):
        for name in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll'):
            (self.root/'x64/out'/name).write_text('old-'+name)

    def assert_old_pair(self):
        for name in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll'):
            self.assertEqual((self.root/'x64/out'/name).read_text(), 'old-'+name)

    def test_locked_prefix_does_not_touch_build(self):
        with (self.root/'runtime/msvc-prefix.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
            code, output = self.invoke()
        self.assertEqual(code, 75, output)
        self.assertFalse((self.root/'calls.jsonl').exists())
        self.assertEqual(self.stamp.read_text(), 'existing-build\n')
        self.assertFalse((self.root/'x64/logs').exists())

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_success_flags_pair_logs_and_lock_release(self):
        code, output = self.invoke(('Debug', '/p:Custom=a b'))
        self.assertEqual(code, 0, output)
        calls = [json.loads(s) for s in (self.root/'calls.jsonl').read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertIn('forwarder', calls[0]['args'][0])
        self.assertIn('OptiScaler', calls[1]['args'][0])
        for call in calls:
            for arg in ('/p:Configuration=Debug', '/p:Platform=x64', '/p:CL_MPCount=16',
                        '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false', '/p:Custom=a b'):
                self.assertIn(arg, call['args'])
            self.assertEqual(call['prefix'], self.env['WINEPREFIX'])
            self.assertEqual(call['debug'], '-all')
        for name in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll'):
            self.assertEqual((self.root/'x64/out'/name).read_text(), 'new-'+name)
        self.assertEqual(len(list((self.root/'x64/logs').glob('*/build-*.log'))), 2)
        with (self.root/'runtime/msvc-prefix.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_empty_configuration_keeps_release_default(self):
        code, output = self.invoke(('', '/p:Extra=true'))
        self.assertEqual(code, 0, output)
        calls = [json.loads(line) for line in (self.root/'calls.jsonl').read_text().splitlines()]
        for call in calls:
            self.assertIn('/p:Configuration=Release', call['args'])
            self.assertIn('/p:Extra=true', call['args'])

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_compiler_failures_retain_old_pair_and_logs(self):
        for step in ('first', 'second'):
            with self.subTest(step=step):
                self.old_pair()
                self.env['FAIL_STEP'] = step
                code, output = self.invoke()
                self.assertEqual(code, 23, output)
                self.assert_old_pair()
                self.assertTrue(any('intentional compiler failure' in p.read_text()
                                    for p in (self.root/'x64/logs').glob('*/build-*.log')))

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_missing_artifact_rejects_pair(self):
        self.old_pair()
        self.env['OMIT'] = 'OptiScaler.dll'
        code, output = self.invoke()
        self.assertEqual(code, 1, output)
        self.assertIn('missing', output)
        self.assert_old_pair()

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_incremental_forwarder_can_remain_unchanged(self):
        name = 'nvngx.dll_dlssnr.dll'
        source = self.root/'x64/Release'/name
        source.write_text('valid-incremental-forwarder')
        self.env['OMIT'] = name
        code, output = self.invoke()
        self.assertEqual(code, 0, output)
        self.assertEqual((self.root/'x64/out'/name).read_text(), 'valid-incremental-forwarder')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_stale_optiscaler_after_changed_stamp_rejected(self):
        self.old_pair()
        (self.root/'x64/Release/OptiScaler.dll').write_text('stale')
        self.env['OMIT'] = 'OptiScaler.dll'
        code, output = self.invoke()
        self.assertEqual(code, 1, output)
        self.assertIn('stale', output)
        self.assert_old_pair()

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_invalid_output_target_retains_pair(self):
        self.old_pair()
        target = self.root/'x64/out/nvngx.dll_dlssnr.dll'
        target.unlink()
        target.mkdir()
        (target/'keep').write_text('existing directory')
        code, output = self.invoke()
        self.assertEqual(code, 1, output)
        self.assertIn('not a regular file', output)
        self.assertEqual((self.root/'x64/out/OptiScaler.dll').read_text(), 'old-OptiScaler.dll')
        self.assertEqual((target/'keep').read_text(), 'existing directory')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_lock_descriptor_not_inherited(self):
        self.compiler("""
import fcntl, os, sys
from pathlib import Path
with (Path(os.environ['XDG_RUNTIME_DIR'])/'msvc-prefix.lock').open('a') as lock:
    try:
        fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:
        pass
    else:
        sys.exit(66)
for entry in Path('/proc/self/fd').iterdir():
    try:
        if str(entry.resolve()).endswith('/msvc-prefix.lock'):
            sys.exit(67)
    except FileNotFoundError:
        pass
config=next(a.split('=',1)[1] for a in sys.argv if a.startswith('/p:Configuration='))
name='nvngx.dll_dlssnr.dll' if 'forwarder' in sys.argv[1] else 'OptiScaler.dll'
p=Path('x64')/config/name
p.parent.mkdir(parents=True,exist_ok=True)
p.write_text('new')
""")
        code, output = self.invoke()
        self.assertEqual(code, 0, output)

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_cli_interrupt_releases_lock_and_cleans_work(self):
        self.compiler("import os, time\nfrom pathlib import Path\nPath('pid').write_text(str(os.getpid()))\nwhile True: time.sleep(1)\n")
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signum):
                marker = self.root/'pid'
                marker.unlink(missing_ok=True)
                child = subprocess.Popen(['bash', str(self.root/'tools/build-local.sh')], cwd=self.root,
                                         env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.addCleanup(lambda p=child: p.wait(timeout=3))
                self.addCleanup(lambda p=child: p.poll() is None and p.kill())
                deadline = time.monotonic()+4
                while not marker.exists() and time.monotonic()<deadline:
                    time.sleep(.02)
                self.assertTrue(marker.exists())
                pid = int(marker.read_text())
                child.send_signal(signum)
                self.assertEqual(child.wait(timeout=5), 128+signum)
                self.assertFalse(alive(pid))
                with (self.root/'runtime/msvc-prefix.lock').open('a') as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)


@unittest.skipUnless(HELPER.exists(), 'new supervisor not implemented')
@unittest.skipUnless(REAL_PROC, PROC_REASON)
class Supervisor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('build_local', HELPER)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='retoque-supervisor-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_step(self, code, **kw):
        return self.module.run_step([sys.executable, '-c', code], cwd=self.root,
                                    log_path=self.root/'step.log', inactivity_timeout=.25,
                                    hard_timeout=2, poll_interval=.03, term_grace=.12, **kw)

    def test_unavailable_root_identity_fails_closed(self):
        with patch.object(self.module, '_process', return_value=None):
            result = self.module.run_step([sys.executable, '-c', 'pass'], cwd=self.root,
                                          log_path=self.root/'identity.log', inactivity_timeout=.5,
                                          hard_timeout=2, poll_interval=.02, term_grace=.1)
        self.assertNotEqual(result, 0)
        self.assertIn('identity', (self.root/'identity.log').read_text())

    def test_idle_hang_preserves_log(self):
        started = time.monotonic()
        code = self.run_step("import time; print('last evidence', flush=True); time.sleep(60)")
        self.assertEqual(code, 124)
        self.assertLess(time.monotonic()-started, 2)
        self.assertIn('last evidence', (self.root/'step.log').read_text())

    def test_cpu_and_log_progress_do_not_false_timeout(self):
        for code in ("import time; end=time.monotonic()+.65\nwhile time.monotonic()<end: pass",
                     "import time\nfor n in range(8): print(n, flush=True); time.sleep(.08)"):
            with self.subTest(code=code):
                self.assertEqual(self.run_step(code), 0)

    def test_short_waited_workers_count_as_cpu_progress(self):
        code = r"""import subprocess, sys
for n in range(16):
 p=subprocess.Popen([sys.executable,'-c','import time;end=time.process_time()+.025\nwhile time.process_time()<end:pass'])
 p.wait()
"""
        result = self.module.run_step([sys.executable, '-c', code], cwd=self.root,
                                     log_path=self.root/'step.log', inactivity_timeout=.2,
                                     hard_timeout=4, poll_interval=.12, term_grace=.1)
        self.assertEqual(result, 0)

    def test_descendant_cpu_counts_as_progress(self):
        code = r"""import subprocess, sys
p=subprocess.Popen([sys.executable,'-c','import time; end=time.monotonic()+.65\nwhile time.monotonic()<end: pass'])
p.wait()
"""
        self.assertEqual(self.run_step(code), 0)

    def test_success_cleans_background_descendant(self):
        code = """import subprocess,sys,time
from pathlib import Path
p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])
Path('child').write_text(str(p.pid))
time.sleep(.15)
"""
        self.assertEqual(self.run_step(code), 0)
        self.assertFalse(alive(int((self.root/'child').read_text())))

    def test_build_progress_ignores_owned_wineserver_spin(self):
        result = self.module.run_step([sys.executable, '-c',
                                      'import time;end=time.monotonic()+2\nwhile time.monotonic()<end:pass',
                                      'wineserver'],
                                     cwd=self.root, log_path=self.root/'step.log', inactivity_timeout=.25,
                                     hard_timeout=2, poll_interval=.02, term_grace=.1,
                                     cpu_progress_filter=self.module._build_cpu_progress)
        self.assertEqual(result, 124)
        self.assertIn('inactivity timeout', (self.root/'step.log').read_text())

    def test_build_progress_accepts_owned_msbuild_cpu(self):
        result = self.module.run_step([sys.executable, '-c',
                                      'import time;end=time.monotonic()+.65\nwhile time.monotonic()<end:pass',
                                      r'C:\MSBuild\MSBuild.exe'],
                                     cwd=self.root, log_path=self.root/'step.log', inactivity_timeout=.25,
                                     hard_timeout=2, poll_interval=.02, term_grace=.1,
                                     cpu_progress_filter=self.module._build_cpu_progress)
        self.assertEqual(result, 0)

    def test_excluded_owned_cpu_does_not_hide_hang(self):
        result = self.module.run_step([sys.executable, '-c',
                                      'import time;end=time.monotonic()+2\nwhile time.monotonic()<end:pass'],
                                     cwd=self.root, log_path=self.root/'step.log', inactivity_timeout=.25,
                                     hard_timeout=2, poll_interval=.02, term_grace=.1,
                                     cpu_progress_filter=lambda pid: False)
        self.assertEqual(result, 124)
        self.assertIn('inactivity timeout', (self.root/'step.log').read_text())

    def test_hard_timeout_even_with_log_progress(self):
        result = self.module.run_step([sys.executable, '-c',
                                      "import time\nwhile True: print('alive',flush=True); time.sleep(.04)"],
                                     cwd=self.root, log_path=self.root/'step.log', inactivity_timeout=.3,
                                     hard_timeout=.5, poll_interval=.02, term_grace=.1)
        self.assertEqual(result, 124)

    def test_owned_children_and_observed_detached_child_cleaned_unrelated_survives(self):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        self.addCleanup(lambda: unrelated.wait(timeout=3))
        self.addCleanup(lambda: unrelated.poll() is None and unrelated.kill())
        # Parent remains alive long enough for an escaped child's ancestry to be observed.
        code = """import os, subprocess, sys, time
from pathlib import Path
pids=[]
for detached in (False, True):
 p=subprocess.Popen([sys.executable,'-c','import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60)'],start_new_session=detached)
 pids.append(p.pid)
Path('children').write_text(' '.join(map(str,pids)))
time.sleep(60)
"""
        self.assertEqual(self.run_step(code), 124)
        for pid in map(int, (self.root/'children').read_text().split()):
            self.assertFalse(alive(pid), f'owned child {pid} survived')
        self.assertIsNone(unrelated.poll())

    def test_spawn_error_preserves_log_and_reports_failure(self):
        result = self.module.run_step(['/definitely/missing/compiler'], cwd=self.root,
                                     log_path=self.root/'step.log')
        self.assertEqual(result, 127)
        self.assertTrue((self.root/'step.log').read_text())


@unittest.skipUnless(HELPER.exists(), 'new supervisor not implemented')
class Publication(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('build_local_publication', HELPER)
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='retoque-publication-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_group_without_live_known_anchor_is_not_adopted(self):
        root = (1, 9, 100, 100, 0, 'S', 0)
        child = (2, 100, 100, 100, 0, 'S', 0)
        with patch.object(self.module, '_process', return_value=root):
            owned = self.module.OwnedProcesses(SimpleNamespace(pid=100))
        with patch.object(self.module, '_snapshot', return_value={100: root, 200: child}):
            self.assertIn(200, owned.observe())
        reused_group_member = (90, 1, 100, 100, 10, 'S', 0)
        with patch.object(self.module, '_snapshot', return_value={300: reused_group_member}):
            self.assertEqual(owned.observe(), {})
        self.assertNotIn(300, owned.identities)
        reused_leader = (99, 1, 100, 100, 1, 'S', 0)
        with patch.object(self.module, '_snapshot', return_value={100: reused_leader, 300: reused_group_member}):
            self.assertEqual(owned.observe(), {})

    def test_build_wrapper_cannot_credit_reaped_infrastructure_cpu(self):
        with patch.object(self.module.Path, 'read_bytes', return_value=b'/bin/bash\0/opt/toolchain/bin/msbuild\0'):
            self.assertTrue(self.module._build_cpu_progress(123))
            self.assertFalse(self.module._build_waited_cpu_progress(123))
        with patch.object(self.module.Path, 'read_bytes', return_value=b'wine64\0C:\\MSBuild\\MSBuild.exe\0'):
            self.assertTrue(self.module._build_waited_cpu_progress(123))
        with patch.object(self.module.Path, 'read_bytes', return_value=b'wineserver\0'):
            self.assertFalse(self.module._build_cpu_progress(123))
            self.assertFalse(self.module._build_waited_cpu_progress(123))

    def test_pidfd_unavailable_refuses_unsafe_fallback(self):
        with patch.object(self.module.os, 'pidfd_open', side_effect=OSError('pidfd unavailable')):
            with self.assertRaisesRegex(RuntimeError, 'pidfd'):
                self.module._require_pidfds()

    def test_signal_after_first_replace_rolls_back_pair(self):
        sources = self.root/'source'
        output = self.root/'out'
        sources.mkdir()
        output.mkdir()
        for name in ('first.dll', 'second.dll'):
            (sources/name).write_text('new-'+name)
            (output/name).write_text('old-'+name)
        replace = os.replace
        signalled = []
        def interrupt_after_replace(source, target):
            result = replace(source, target)
            if not signalled:
                signalled.append(True)
                os.kill(os.getpid(), signal.SIGTERM)
            return result
        with self.module._signals(self.module._interrupt), patch.object(self.module.os, 'replace', side_effect=interrupt_after_replace):
            with self.assertRaises(self.module.Interrupted):
                self.module.publish_pair([sources/'first.dll', sources/'second.dll'], output)
        for name in ('first.dll', 'second.dll'):
            self.assertEqual((output/name).read_text(), 'old-'+name)

    def test_second_promotion_error_rolls_back_first(self):
        sources = self.root/'source'
        output = self.root/'out'
        sources.mkdir()
        output.mkdir()
        for name in ('first.dll', 'second.dll'):
            (sources/name).write_text('new-'+name)
            (output/name).write_text('old-'+name)
        replace = os.replace
        calls = []
        def fail_second(source, target):
            calls.append(Path(target).name)
            if len(calls) == 2:
                raise PermissionError('injected second promotion error')
            return replace(source, target)
        with patch.object(self.module.os, 'replace', side_effect=fail_second):
            with self.assertRaises(PermissionError):
                self.module.publish_pair([sources/'first.dll', sources/'second.dll'], output)
        self.assertEqual(calls, ['first.dll', 'second.dll', 'first.dll'])
        for name in ('first.dll', 'second.dll'):
            self.assertEqual((output/name).read_text(), 'old-'+name)



if __name__ == '__main__':
    unittest.main(verbosity=2)
