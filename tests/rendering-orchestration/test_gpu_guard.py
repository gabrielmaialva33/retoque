"""Process evidence must keep real/unknown games out without rejecting native services."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _gpu_guard import guard_no_game, unreadable_may_be_game


class ProcessGuard(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.proc = self.root / '123'
        self.proc.mkdir()
        self.native = self.root / 'native-service'
        self.native.write_bytes(b'native')
        self.original_stat = Path.stat
        self.original_read = Path.read_text
        self.service('tailscaled', b'/usr/bin/tailscaled\0--state=mem:\0')

    def service(self, comm, argv):
        (self.proc / 'comm').write_text(comm)
        (self.proc / 'cmdline').write_bytes(argv)
        (self.proc / 'maps').write_text('')

    def run_hidden(self, *, exe='/usr/bin/tailscaled', writable=False, owner=0):
        def read(path, *args, **kwargs):
            if path == self.proc / 'maps':
                raise PermissionError()
            return self.original_read(path, *args, **kwargs)

        def file_stat(path, *args, **kwargs):
            if str(path) in ('/usr/bin/tailscaled', '/usr/lib/systemd/systemd'):
                values = list(self.original_stat(self.native))
                values[0] = 0o100777 if writable else 0o100755
                values[4] = owner
                return os.stat_result(values)
            return self.original_stat(path, *args, **kwargs)

        def link(path):
            if exe is None:
                raise PermissionError()
            return Path(exe)

        with patch.object(Path, 'read_text', read), patch.object(Path, 'stat', file_stat), \
                patch.object(Path, 'readlink', link):
            guard_no_game(proc_root=self.root)

    def test_native_capability_service_with_hidden_maps_and_exe(self):
        self.run_hidden(exe=None)

    def test_exposed_executable_must_agree(self):
        self.run_hidden()
        with self.assertRaisesRegex(RuntimeError, 'cannot inspect'):
            self.run_hidden(exe='/usr/bin/wine')

    def test_systemd_and_pam_exact_identities(self):
        self.service('systemd', b'/usr/lib/systemd/systemd\0--user\0')
        self.run_hidden(exe=None)
        self.service('(sd-pam)', b'(sd-pam)\0')
        self.run_hidden(exe=None)

    def test_unknown_hidden_process_is_refused_even_without_wine_words(self):
        for comm, argv in [('native-game', b'/opt/game\0'), ('tailscaled', b'tailscaled\0'),
                           ('tailscaled', b'/opt/tailscaled\0'), ('tailscaled', b''),
                           ('tailscaled', b'/usr/bin/tailscaled\0Z:\\Game.exe\0'),
                           ('tailscaled', b'/usr/bin/tailscaled\0proton\0'),
                           ('tailscaled', b'/usr/bin/tailscaled\0/steamapps/common/game\0'),
                           ('wine', b'/usr/bin/tailscaled\0'), ('(sd-pam)', b'(sd-pam)\0extra\0')]:
            with self.subTest(comm=comm, argv=argv):
                self.service(comm, argv)
                with self.assertRaisesRegex(RuntimeError, 'cannot inspect'):
                    self.run_hidden(exe=None)

    def test_untrusted_native_binary_is_not_an_explanation(self):
        for options in ({'writable': True}, {'owner': os.getuid() or 1234}):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, 'cannot inspect'):
                self.run_hidden(exe=None, **options)

    def test_missing_identity_fails_closed(self):
        (self.proc / 'cmdline').unlink()
        self.assertTrue(unreadable_may_be_game(self.proc))

    def test_readable_game_mapping_and_staged_mapping_still_refused(self):
        for mapping, work in [('/library/steamapps/common/Game/game.so', None),
                              ('/fixture/staged/dxgi.dll', '/fixture/staged')]:
            (self.proc / 'maps').write_text(mapping)
            with self.assertRaises(RuntimeError):
                guard_no_game(work, proc_root=self.root)

    def test_compiler_refused_and_exited_process_ignored(self):
        (self.proc / 'comm').write_text('CL.exe')
        with self.assertRaisesRegex(RuntimeError, 'compiler'):
            guard_no_game(proc_root=self.root)
        (self.proc / 'comm').unlink()
        guard_no_game(proc_root=self.root)

    def test_runtime_exception_only_when_requested(self):
        (self.proc / 'maps').write_text('/library/steamapps/common/Proton 11/wine.so')
        with self.assertRaises(RuntimeError):
            guard_no_game(proc_root=self.root)
        guard_no_game(proc_root=self.root, allow_steam_runtime=True)
