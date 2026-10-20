#!/usr/bin/env python3
"""Host checks for run.py's busy guard: a running build, never a command line that mentions one."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
SOURCE = (HERE / 'run.py').read_text()
GUARD = {'os': os, 'Path': Path}
exec(SOURCE[SOURCE.index('def ancestors():'):SOURCE.index('def run_watched(')], GUARD)


class BusyGuard(unittest.TestCase):
    def running(self, argv, cwd=None):
        child = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.3)
            return GUARD['build_running']()
        finally:
            child.kill()
            child.wait()

    def test_mentions_are_not_builds(self):
        # The 2026-10-20 false positive: a shell that ran build-local.sh earlier in the same command string.
        self.assertFalse(self.running(['bash', '-c', 'sleep 20 # ./build-local.sh Release']))
        self.assertFalse(self.running(['sh', '-c', 'sleep 20; ./build-local.sh Release']))

    def test_running_script_is_a_build(self):
        with tempfile.TemporaryDirectory() as scratch:
            script = Path(scratch) / 'build-local.sh'
            script.write_text('#!/usr/bin/env bash\nsleep 20\n')  # the real scripts' shebang
            script.chmod(0o755)
            self.assertTrue(self.running([str(script)]))

    def test_own_ancestors_are_skipped(self):
        self.assertIn(os.getpid(), GUARD['ancestors']())
        self.assertIn(os.getppid(), GUARD['ancestors']())


if __name__ == '__main__':
    unittest.main()
