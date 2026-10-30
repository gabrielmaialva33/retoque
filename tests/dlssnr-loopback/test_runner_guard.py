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
        self.assertFalse(self.answer(b'gpg-agent\0--homedir\0/run/user/1000/gnupg\0--daemon\0'))
        self.assertFalse(self.answer(b'ssh-agent\0-s\0'))

    def test_what_may_be_a_game_still_refuses(self):
        self.assertTrue(self.answer(b'Z:\\games\\Cyberpunk2077.exe\0'))
        self.assertTrue(self.answer(b'/usr/bin/wine64-preloader\0'))
        self.assertTrue(self.answer(b'/srv/steam/steamapps/common/Proton 11/files/bin/wine\0'))
        self.assertTrue(self.answer(b''))    # a kernel-style empty command line
        self.assertTrue(self.answer(None))   # none to read at all


if __name__ == '__main__':
    unittest.main()
