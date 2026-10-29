#!/usr/bin/env python3
"""Exercise the real suite runner against controlled child processes."""
import importlib.util
import contextlib
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

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("retoque_runner_under_test", ROOT / "tests/run_all.py")
sys.path.insert(0, str(ROOT / "tests"))
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)
REAL_PROC = (os.name == 'posix' and Path('/proc/self/stat').exists() and
             int(Path('/proc/self/stat').read_text().split(' ', 1)[0]) == os.getpid())
PROC_REASON = 'requires native Linux process identities; do not run kill tests in a virtualized procfs'


def alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


class RunnerResults(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="retoque-runner-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tests = self.root / "tests"
        self.suite = self.tests / "fixture"
        self.suite.mkdir(parents=True)
        self.logs = self.root / "logs"
        self.logs.mkdir()
        self.original_paths = (runner.ROOT, runner.TESTS, runner.MANIFEST)
        runner.ROOT, runner.TESTS, runner.MANIFEST = self.root, self.tests, self.tests / "suites.toml"
        self.addCleanup(self.restore)
        self.entry = dict(path="fixture", domain="tooling", requires=["python"],
                          tier="host", timeout=2, result="artifacts/result.json")
        self.result_path = self.suite / "artifacts/result.json"

    def restore(self):
        runner.ROOT, runner.TESTS, runner.MANIFEST = self.original_paths

    def script(self, text):
        (self.suite / "run.py").write_text(text)

    def emitter(self, status, *, exit_code=0, identity="os.environ.get('RETOQUE_TEST_RUN_ID', '')"):
        self.script(
            "import json, os\nfrom pathlib import Path\n"
            "path=Path(__file__).parent/'artifacts/result.json'\n"
            "path.parent.mkdir(exist_ok=True)\n"
            f"payload={{'schema':'retoque-suite-result','version':1,'run_id':{identity},"
            f"'status':{status!r},'reason':'controlled result','runtime_coverage':False}}\n"
            "path.write_text(json.dumps(payload))\n"
            f"raise SystemExit({exit_code})\n"
        )

    def run_suite(self):
        return runner.run_suite("fixture", self.entry, self.logs, False)

    def test_skip_is_not_pass(self):
        self.emitter("SKIP")
        result = self.run_suite()
        self.assertEqual(result.status, "SKIP")
        self.assertIn("controlled result", result.note)

    def test_pass_requires_current_terminal_evidence(self):
        self.emitter("PASS")
        result = self.run_suite()
        self.assertEqual(result.status, "PASS")
        saved = json.loads(self.result_path.read_text())
        self.assertTrue(saved["run_id"])

    def test_inconclusive_is_not_pass(self):
        self.emitter("INCONCLUSIVE")
        self.assertEqual(self.run_suite().status, "INCONCLUSIVE")

    def test_declared_failure_overrides_zero_exit(self):
        self.emitter("FAIL")
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_nonzero_exit_overrides_claimed_pass(self):
        self.emitter("PASS", exit_code=7)
        result = self.run_suite()
        self.assertEqual(result.status, "FAIL")
        self.assertIn("7", result.note)

    def test_nonzero_exit_preserves_current_structured_evidence(self):
        self.emitter('FAIL', exit_code=7)
        result = self.run_suite()
        self.assertEqual(result.status, 'FAIL')
        self.assertIsNotNone(result.evidence)
        self.assertEqual(result.evidence['reason'], 'controlled result')
        self.assertEqual(result.evidence['run_id'], result.run_id)

    def test_invalid_optional_fields_fail_only_the_suite(self):
        for fields in ({'reason': None, 'missing': None}, {'reason': []},
                       {'missing': 'not-a-list'}, {'runtime_coverage': 'yes'}):
            with self.subTest(fields=fields):
                self.script(
                    'import json, os\nfrom pathlib import Path\n'
                    "p=Path(__file__).parent/'artifacts/result.json'\n"
                    'p.parent.mkdir(exist_ok=True)\n'
                    "payload={'schema':'retoque-suite-result','version':1,"
                    "'run_id':os.environ['RETOQUE_TEST_RUN_ID'],'status':'PASS'}\n"
                    f'payload.update({fields!r})\n'
                    'p.write_text(json.dumps(payload))\n')
                result = self.run_suite()
                self.assertEqual(result.status, 'FAIL')
                self.assertIn('invalid result', result.note)

    def test_nonfinite_evidence_is_rejected_at_read_boundary(self):
        self.result_path.parent.mkdir()
        for number in ('NaN', 'Infinity', '-Infinity', '1e999'):
            with self.subTest(number=number):
                self.result_path.write_text(
                    '{"schema":"retoque-suite-result","version":1,"run_id":"current",'
                    '"status":"PASS","metric":'+number+'}')
                with self.assertRaises(ValueError):
                    runner._read_result(self.result_path, 'current')

    def test_unsafe_suite_name_cannot_write_outside_log_directory(self):
        self.emitter('PASS')
        outside = self.root/'outside.log'
        outside.write_text('keep this file')
        result = runner.run_suite('../outside', self.entry, self.logs, False)
        self.assertEqual(result.status, 'FAIL')
        self.assertEqual(outside.read_text(), 'keep this file')
        self.assertFalse(self.result_path.exists(), 'unsafe name launched its suite')

    def test_prelaunch_abort_is_runner_evidence(self):
        stop = threading.Event()
        stop.set()
        result = runner.run_suite('fixture', self.entry, self.logs, False, stop=stop)
        self.assertEqual(result.status, 'ABORTED')
        self.assertEqual(result.evidence_source, 'runner')

    @unittest.skipUnless(os.name == 'posix', 'POSIX SIGCHLD contract')
    def test_nondefault_sigchld_refuses_before_spawn(self):
        self.script('pass\n')
        for handler in (signal.SIG_IGN, lambda *_: None):
            with self.subTest(handler=handler), \
                    patch.object(runner.signal, 'getsignal', return_value=handler), \
                    patch.object(runner.subprocess, 'Popen', side_effect=AssertionError('must not spawn')):
                result = self.run_suite()
                self.assertEqual(result.status, 'FAIL')
                self.assertIn('default SIGCHLD', result.note)

    def test_stale_pass_is_rejected(self):
        self.emitter("PASS", identity="'previous-run'")
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_missing_result_is_rejected(self):
        self.script("pass\n")
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_running_result_is_not_terminal(self):
        self.emitter("RUNNING")
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_malformed_result_is_rejected(self):
        self.script("from pathlib import Path\np=Path(__file__).parent/'artifacts/result.json'\n"
                    "p.parent.mkdir(exist_ok=True)\np.write_text('{bad')\n")
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_result_cannot_escape_suite(self):
        self.emitter("PASS")
        self.entry["result"] = "../../outside.json"
        self.assertEqual(self.run_suite().status, "FAIL")

    def test_suite_created_result_symlink_is_rejected_after_exit(self):
        outside = self.root/'external'
        outside.mkdir()
        self.script(
            'import json, os\nfrom pathlib import Path\n'
            f'outside=Path({str(outside)!r})\n'
            "(Path(__file__).parent/'artifacts').symlink_to(outside, target_is_directory=True)\n"
            "(outside/'result.json').write_text(json.dumps({'schema':'retoque-suite-result',"
            "'version':1,'run_id':os.environ['RETOQUE_TEST_RUN_ID'],'status':'PASS'}))\n")
        self.assertEqual(self.run_suite().status, 'FAIL')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_timeout_does_not_write_through_suite_created_symlink(self):
        outside = self.root/'external'
        outside.mkdir()
        external_result = outside/'result.json'
        external_result.write_text('external evidence must survive')
        self.entry['timeout'] = .3
        self.script(
            'import time\nfrom pathlib import Path\n'
            f"(Path(__file__).parent/'artifacts').symlink_to(Path({str(outside)!r}), target_is_directory=True)\n"
            'time.sleep(60)\n')
        result = self.run_suite()
        self.assertEqual(result.status, 'TIMEOUT')
        self.assertIsInstance(result.returncode, int)
        self.assertIn('receipt unavailable', result.note)
        self.assertEqual(external_result.read_text(), 'external evidence must survive')

    def test_legacy_exit_only_suite_still_runs(self):
        del self.entry["result"]
        self.script("raise SystemExit(0)\n")
        self.assertEqual(self.run_suite().status, "PASS")

    def remove_child(self, pid_path):
        if pid_path.exists():
            pid = int(pid_path.read_text())
            if alive(pid):
                os.kill(pid, signal.SIGKILL)

    def test_timeout_stops_owned_child(self):
        self.entry["timeout"] = 0.4
        pid_path = self.root / "descendant.pid"
        self.addCleanup(self.remove_child, pid_path)
        self.script("import subprocess, sys, time\nfrom pathlib import Path\n"
                    "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])\n"
                    f"Path({str(pid_path)!r}).write_text(str(p.pid))\n"
                    "time.sleep(60)\n")
        result = self.run_suite()
        self.assertEqual(result.status, "TIMEOUT")
        self.assertTrue(pid_path.exists())
        pid = int(pid_path.read_text())
        deadline = time.monotonic() + 2
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(alive(pid), "timed-out suite left its child alive")

    def prepare_cli(self):
        shutil.copy2(ROOT / "tests/run_all.py", self.tests / "run_all.py")
        helper = ROOT / "tests/_suite_result.py"
        if helper.exists():
            shutil.copy2(helper, self.tests / "_suite_result.py")
        (self.tests / "suites.toml").write_text(
            '[fixture]\npath="fixture"\ndomain="tooling"\nrequires=["python"]\n'
            'tier="host"\ntimeout=60\nresult="artifacts/result.json"\n')
        return [sys.executable, str(self.tests / "run_all.py"), "--log-dir", str(self.logs),
                "--report", str(self.root / "summary.json")]

    def test_cli_preserves_skip_and_strict_mode_fails(self):
        self.emitter("SKIP")
        command = self.prepare_cli()
        completed = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        summary = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(summary["results"][0]["status"], "SKIP")
        self.assertEqual(summary["counts"].get("PASS", 0), 0)
        strict = subprocess.run(command + ["--strict"], capture_output=True, text=True, timeout=10)
        self.assertEqual(strict.returncode, 1, strict.stdout + strict.stderr)

    def test_empty_selection_replaces_previous_report(self):
        self.emitter('PASS')
        command = self.prepare_cli()
        report = self.root/'summary.json'
        report.write_text(json.dumps({'status':'PASS','run_id':'previous'}))
        completed = subprocess.run(command+['--skip','fixture'], capture_output=True,
                                   text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stdout+completed.stderr)
        payload = json.loads(report.read_text())
        self.assertEqual(payload['status'], 'INCONCLUSIVE')
        self.assertNotEqual(payload['run_id'], 'previous')
        self.assertEqual(payload['selected'], [])
        self.assertEqual(payload['results'], [])

    def test_empty_selection_is_nonzero_in_strict_mode(self):
        self.emitter('PASS')
        command = self.prepare_cli()
        completed = subprocess.run(command + ['--skip', 'fixture', '--strict'],
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        payload = json.loads((self.root/'summary.json').read_text())
        self.assertEqual(payload['status'], 'INCONCLUSIVE')
        self.assertEqual(payload['selected'], [])
        self.assertEqual(payload['results'], [])

    def terminal_signal_case(self, *, empty, after_commit):
        self.emitter('PASS')
        command = self.prepare_cli()[2:]
        if empty:
            command.extend(['--skip', 'fixture'])
        terminal = 'INCONCLUSIVE' if empty else 'PASS'
        original_writer = runner.atomic_json
        injected = []
        def inject():
            if not injected:
                injected.append(True)
                # Invoke the installed handler directly: deterministic delivery
                # at the write/commit boundary, with no process-directed signal.
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
        def persist(path, payload):
            if not after_commit and payload['status'] == terminal:
                inject()
            original_writer(path, payload)
        def printed(*values, **kwargs):
            text = ' '.join(map(str, values))
            prefix = 'nothing selected; report:' if empty else 'report:'
            if after_commit and text.startswith(prefix):
                inject()
        with patch.object(sys, 'argv', ['run_all.py', *command]), \
                patch.object(runner, 'atomic_json', side_effect=persist), \
                patch('builtins.print', side_effect=printed):
            code = runner.main()
        self.assertTrue(injected, 'test did not reach the requested completion boundary')
        payload = json.loads((self.root/'summary.json').read_text())
        self.assertEqual(code, 0 if after_commit else 143)
        self.assertEqual(payload['status'], terminal if after_commit else 'ABORTED')
        self.assertEqual(payload['selected'], [] if empty else ['fixture'])

    def test_signal_during_pass_write_repersists_aborted(self):
        self.terminal_signal_case(empty=False, after_commit=False)

    def test_signal_during_empty_selection_write_repersists_aborted(self):
        self.terminal_signal_case(empty=True, after_commit=False)

    def test_signal_after_pass_commit_cannot_change_final_exit(self):
        self.terminal_signal_case(empty=False, after_commit=True)

    def test_signal_after_empty_selection_commit_cannot_change_final_exit(self):
        self.terminal_signal_case(empty=True, after_commit=True)

    def test_prerequisite_skip_is_runner_evidence(self):
        self.emitter('PASS')
        command = self.prepare_cli()
        manifest = self.tests/'suites.toml'
        manifest.write_text(manifest.read_text().replace('requires=["python"]', 'requires=["cxx"]'))
        completed = subprocess.run(command, env=dict(os.environ, PATH=''), capture_output=True,
                                   text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stdout+completed.stderr)
        payload = json.loads((self.root/'summary.json').read_text())
        self.assertEqual(payload['results'][0]['status'], 'SKIP')
        self.assertEqual(payload['results'][0]['evidence_source'], 'runner')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_report_write_failure_requests_stop_before_waiting_for_workers(self):
        self.emitter('PASS')
        slow = self.tests/'slow'
        slow.mkdir()
        ready, stopped = self.root/'slow-ready', self.root/'slow-stopped'
        (slow/'run.py').write_text(
            'import signal, time\nfrom pathlib import Path\n'
            f'def stop(*_):\n    Path({str(stopped)!r}).write_text("stopped")\n    raise SystemExit(0)\n'
            'signal.signal(signal.SIGTERM, stop)\n'
            f'Path({str(ready)!r}).write_text("ready")\n'
            'time.sleep(1.5)\n')
        # Make the first result arrive only after the other worker is observable.
        self.script(
            'import time\nfrom pathlib import Path\n'
            f'p=Path({str(ready)!r})\n'
            'deadline=time.monotonic()+2\n'
            'while not p.exists() and time.monotonic()<deadline: time.sleep(.01)\n')
        self.prepare_cli()
        (self.tests/'suites.toml').write_text(
            '[fixture]\npath="fixture"\nrequires=["python"]\ntier="host"\ntimeout=4\n'
            '[slow]\npath="slow"\nrequires=["python"]\ntier="host"\ntimeout=4\n')
        original = runner.atomic_json
        injected = False
        def persist(path, payload):
            nonlocal injected
            if len(payload['results']) == 1 and not injected:
                injected = True
                raise OSError('injected report write failure')
            original(path, payload)
        with patch.object(sys, 'argv', ['run_all.py','--jobs','2','--log-dir',str(self.logs)]), \
                patch.object(runner, 'atomic_json', side_effect=persist), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(OSError):
                runner.main()
        self.assertTrue(stopped.exists(), 'report error waited for worker instead of stopping it')

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_timeout_allows_three_second_detached_child_cleanup(self):
        self.entry['timeout'] = .4
        pid_path, cleaned = self.root/'detached.pid', self.root/'cleaned'
        self.addCleanup(self.remove_child, pid_path)
        child_code = ('import signal,time\nfrom pathlib import Path\n'
                      'signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                      f'Path({str(self.root/"detached-ready")!r}).write_text("ready")\n'
                      'time.sleep(60)')
        self.script(
            'import signal, subprocess, sys, time\nfrom pathlib import Path\n'
            'def interrupted(*_): raise KeyboardInterrupt\n'
            'signal.signal(signal.SIGTERM, interrupted)\n'
            f'p=subprocess.Popen([sys.executable,"-c",{child_code!r}],start_new_session=True)\n'
            f'Path({str(pid_path)!r}).write_text(str(p.pid))\n'
            'try:\n'
            '    time.sleep(60)\n'
            'finally:\n'
            '    p.terminate()\n'
            '    try: p.wait(timeout=3)\n'
            '    except subprocess.TimeoutExpired:\n'
            '        p.kill()\n'
            '        p.wait()\n'
            f'    Path({str(cleaned)!r}).write_text("cleaned")\n')
        result = self.run_suite()
        self.assertEqual(result.status, 'TIMEOUT')
        self.assertTrue(cleaned.exists(), 'runner killed wrapper before its three-second cleanup')
        self.assertFalse(alive(int(pid_path.read_text())))

    @unittest.skipUnless(REAL_PROC, PROC_REASON)
    def test_stop_child_does_not_poll_before_final_signals(self):
        ready = self.root/'stop-ready'
        code = ('import signal,time\nfrom pathlib import Path\n'
                'signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                f'Path({str(ready)!r}).write_text("ready")\n'
                'time.sleep(60)')
        process = subprocess.Popen([sys.executable,'-c',code], start_new_session=True)
        try:
            deadline = time.monotonic()+3
            while not ready.exists() and time.monotonic()<deadline:
                time.sleep(.01)
            self.assertTrue(ready.exists())
            with patch.object(process, 'poll', side_effect=AssertionError('leader may be reaped early')):
                runner._stop_child(process)
            self.assertIsNotNone(process.returncode)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)

    @unittest.skipUnless(os.name == 'posix', 'POSIX group signalling')
    def test_stop_child_does_not_signal_group_after_leader_was_reaped(self):
        process = subprocess.Popen([sys.executable,'-c','pass'], start_new_session=True)
        process.wait(timeout=3)
        with patch.object(runner.os, 'killpg', side_effect=AssertionError('reaped PGID is no longer owned')):
            runner._stop_child(process)

    @unittest.skipUnless(os.name == "posix", "POSIX signal contract")
    def test_standalone_signal_records_aborted(self):
        marker = self.root / "started"
        self.script(
            "import sys, time\nfrom pathlib import Path\n"
            f"sys.path.insert(0, {str(ROOT / 'tests')!r})\n"
            "from _suite_result import result_run\n"
            f"with result_run(Path({str(self.result_path)!r})):\n"
            f"    Path({str(marker)!r}).write_text('ready')\n"
            "    time.sleep(60)\n"
        )
        child = subprocess.Popen([sys.executable, str(self.suite / "run.py")],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                 start_new_session=True)
        try:
            deadline = time.monotonic() + 3
            while not marker.exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(marker.exists(), "standalone result guard did not start")
            child.send_signal(signal.SIGTERM)
            output, _ = child.communicate(timeout=5)
            self.assertEqual(child.returncode, 143, output)
            result = json.loads(self.result_path.read_text())
            self.assertEqual(result["status"], "ABORTED")
            self.assertTrue(result["run_id"])
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
            child.communicate(timeout=3)

    @unittest.skipUnless(os.name == "posix", "POSIX signal contract")
    def test_signal_persists_aborted_and_stops_child(self):
        pid_path = self.root / "fixture.pid"
        self.addCleanup(self.remove_child, pid_path)
        self.script("import os, time\nfrom pathlib import Path\n"
                    f"Path({str(pid_path)!r}).write_text(str(os.getpid()))\n"
                    "time.sleep(60)\n")
        child = subprocess.Popen(self.prepare_cli(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            while not pid_path.exists() and child.poll() is None and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(pid_path.exists(), "runner did not launch its suite")
            child.send_signal(signal.SIGTERM)
            output, _ = child.communicate(timeout=8)
            self.assertEqual(child.returncode, 143, output)
            summary = json.loads((self.root / "summary.json").read_text())
            self.assertEqual(summary["status"], "ABORTED")
            self.assertEqual(summary["results"][0]["status"], "ABORTED")
            self.assertFalse(alive(int(pid_path.read_text())))
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.communicate(timeout=3)


class RunnerDiscovery(unittest.TestCase):
    """Selection mistakes must not silently run the wrong domain or hide suites."""

    manifest = {
        'alpha': dict(tier='host', domain='nr', path='alpha', requires=['python'], summary='A | B'),
        'beta': dict(tier='wine', domain='nr', path='beta', requires=['wine-toolchain'], summary='GPU'),
        'gamma': dict(tier='host', domain='fg', path='gamma', requires=['python'], summary='FG'),
    }

    def args(self, **kw):
        from argparse import Namespace
        return Namespace(**(dict(tier='host', only=None, skip=None, domain=None) | kw))

    def test_domain_intersects_tier_and_only_before_skip(self):
        cases = [
            (dict(domain='nr'), ['alpha']),
            (dict(tier='all', domain='nr'), ['alpha', 'beta']),
            (dict(domain=' nr,fg,nr ', skip='gamma'), ['alpha']),
            (dict(only='beta,gamma', domain='nr'), ['beta']),
            (dict(only='alpha,gamma', domain='nr', skip='alpha'), []),
        ]
        for args, expected in cases:
            with self.subTest(args=args):
                self.assertEqual(runner.selected(self.manifest, self.args(**args)), expected)

    def test_unknown_or_empty_domain_is_rejected(self):
        for domain in ('typo', 'nr,typo', ' , '):
            with self.subTest(domain=domain), self.assertRaises(SystemExit):
                runner.selected(self.manifest, self.args(domain=domain))

    def cli(self, *args):
        return subprocess.run([sys.executable, str(ROOT/'tests/run_all.py'), '--list', *args],
                              capture_output=True, text=True, timeout=15)

    def test_default_listing_keeps_all_tiers(self):
        result = self.cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('nr-invariants', result.stdout)
        self.assertIn('nr-gpu-timing-d3d12', result.stdout)

    def test_list_respects_explicit_tier_domain_and_only(self):
        result = self.cli('--tier', 'host', '--domain', 'tooling', '--group-by', 'domain')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('## tooling', result.stdout)
        self.assertIn('runner-results', result.stdout)
        self.assertNotIn('nr-invariants', result.stdout)
        only = self.cli('--only', 'nr-invariants', '--domain', 'nr')
        self.assertEqual(only.returncode, 0, only.stderr)
        self.assertIn('nr-invariants', only.stdout)
        self.assertNotIn('nr-menu ', only.stdout)

    def test_markdown_has_real_links_escapes_cells_and_no_host_availability(self):
        self.assertTrue(hasattr(runner, 'registry_markdown'), 'deterministic registry Markdown is missing')
        text = runner.registry_markdown(self.manifest, sorted(self.manifest), 'domain')
        self.assertIn('## nr', text)
        self.assertIn('[alpha](alpha/README.md)', text)
        self.assertIn('A \\| B', text)
        self.assertNotIn('ready', text)
        self.assertNotIn('unavailable', text)
        self.assertEqual(text, runner.registry_markdown(dict(reversed(list(self.manifest.items()))),
                                                        list(reversed(sorted(self.manifest))), 'domain'))

    def test_unknown_domain_fails_in_list_mode(self):
        result = self.cli('--domain', 'not-a-domain')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('domain', result.stderr)


if __name__ == "__main__":
    unittest.main()
