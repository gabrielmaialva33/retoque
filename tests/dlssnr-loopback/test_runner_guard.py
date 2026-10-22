#!/usr/bin/env python3
"""Host checks for run.py's busy guard: a running build, never a command line that mentions one."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import sys
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SOURCE = (HERE / 'run.py').read_text()
GUARD = {'os': os, 'Path': Path}
exec(SOURCE[SOURCE.index('def ancestors():'):SOURCE.index('def run_watched(')], GUARD)


class BusyGuard(unittest.TestCase):
    # Each case classifies its own child, not the whole machine: a real build running beside the host tier (another
    # worktree's, or this session's) made the negative cases fail on 2026-10-20.
    def running(self, argv, cwd=None):
        child = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.3)
            return GUARD['is_build'](Path('/proc') / str(child.pid))
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
            # The whole-machine scan finds it too.
            child = subprocess.Popen([str(script)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(0.3)
                self.assertTrue(GUARD['build_running']())
            finally:
                child.kill()
                child.wait()

    def test_own_ancestors_are_skipped(self):
        self.assertIn(os.getpid(), GUARD['ancestors']())
        self.assertIn(os.getppid(), GUARD['ancestors']())


class LeaseGuard(unittest.TestCase):
    """run_moving.lease's answer for one of our processes whose maps it cannot read."""

    def answer(self, cmdline):
        import run_moving
        with tempfile.TemporaryDirectory() as scratch:
            proc = Path(scratch)
            if cmdline is not None:
                (proc / 'cmdline').write_bytes(cmdline)
            return run_moving.unreadable_may_be_game(proc)

    def test_agents_are_not_games(self):
        # The 2026-10-22 false positive: another project's gpg-agent, non-dumpable, refused every fixture.
        self.assertFalse(self.answer(b'gpg-agent\0--homedir\0/home/u/.cache/x/gnupg\0--daemon\0'))
        self.assertFalse(self.answer(b'ssh-agent\0-s\0'))

    def test_what_may_be_a_game_still_refuses(self):
        self.assertTrue(self.answer(b'Z:\\home\\u\\Cyberpunk2077.exe\0'))
        self.assertTrue(self.answer(b'/usr/bin/wine64-preloader\0'))
        self.assertTrue(self.answer(b'/home/u/.local/share/Steam/steamapps/common/Proton 11/files/bin/wine\0'))
        self.assertTrue(self.answer(b''))    # a kernel-style empty command line
        self.assertTrue(self.answer(None))   # none to read at all


if __name__ == '__main__':
    unittest.main()
