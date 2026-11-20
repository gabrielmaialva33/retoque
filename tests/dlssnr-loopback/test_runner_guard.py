#!/usr/bin/env python3
"""Host checks for run.py's busy guard: a running build, never a command line that mentions one."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import sys
import unittest
from unittest.mock import patch

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
            # cmdline alone is not identity: the shared guard also requires matching comm/executable
            # and a root-owned, non-writable system binary. Model those proc facts without host dependencies.
            name = cmdline.split(b'\0')[0].decode() if cmdline else 'unknown'
            (proc / 'comm').write_text(name)
            executable = Path('/usr/bin') / name if name in ('gpg-agent', 'ssh-agent') else proc / 'unknown'
            (proc / 'exe').symlink_to(executable)
            original_stat = Path.stat
            def system_binary(path, *args, **kwargs):
                if path == executable:
                    values = list(original_stat(proc))
                    values[0], values[4] = 0o100755, 0
                    return os.stat_result(values)
                return original_stat(path, *args, **kwargs)
            with patch.object(Path, 'stat', system_binary):
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
