"""Shared, fail-closed process guard for GPU fixtures on the user's workstation."""
import os
from pathlib import Path
import stat


# Only native system services with known identities can explain unreadable maps.
# Never infer safety merely from absence of Wine strings in an arbitrary argv.
_NATIVE_SERVICES = {'systemd': '/usr/lib/systemd/systemd',
                    '(sd-pam)': '(sd-pam)', 'tailscaled': '/usr/bin/tailscaled',
                    'gpg-agent': '/usr/bin/gpg-agent', 'ssh-agent': '/usr/bin/ssh-agent'}


def unreadable_may_be_game(proc):
    try:
        comm = (proc / 'comm').read_text().strip()
        raw = (proc / 'cmdline').read_bytes()
        if not raw or any(marker in raw.lower() for marker in
                          (b'wine', b'proton', b'.exe', b'/steamapps/')):
            return True
        argv = raw.rstrip(b'\0').split(b'\0')
        expected = _NATIVE_SERVICES.get(comm)
        if expected is None:
            return True
        allowed_argv0 = {expected.encode()}
        if comm in ('gpg-agent', 'ssh-agent'):
            allowed_argv0.add(comm.encode())
        if argv[0] not in allowed_argv0:
            return True
        if comm == '(sd-pam)':
            return argv != [b'(sd-pam)']
        # If the kernel exposes the executable, it must agree with argv[0].
        # Capabilities (tailscaled) and non-dumpable services can hide this link too.
        try:
            if (proc / 'exe').readlink() != Path(expected):
                return True
        except PermissionError:
            pass
        info = Path(expected).stat()
        return info.st_uid != 0 or not stat.S_ISREG(info.st_mode) or bool(info.st_mode & 0o022)
    except OSError:
        return True


def guard_no_game(work=None, *, allow_steam_runtime=False, proc_root=Path('/proc')):
    for proc in proc_root.iterdir():
        if not proc.name.isdigit():
            continue
        try:
            comm = (proc / 'comm').read_text().strip().lower()
            if comm in ('msbuild.exe', 'cl.exe', 'link.exe'):
                raise RuntimeError('compiler prefix already in use')
            mappings = (proc / 'maps').read_text()
            if work is not None and str(work) in mappings:
                raise RuntimeError('an earlier GPU fixture still maps its staged files')
            for line in mappings.splitlines():
                if '/steamapps/common/' not in line:
                    continue
                component = line.split('/steamapps/common/', 1)[1].split('/', 1)[0]
                if allow_steam_runtime and component.startswith(('Proton', 'SteamLinuxRuntime')):
                    continue
                raise RuntimeError('a Steam common application is mapped; close it before the GPU fixture')
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            try:
                owned = proc.stat().st_uid == os.getuid()
            except (FileNotFoundError, ProcessLookupError):
                continue
            if owned and unreadable_may_be_game(proc):
                raise RuntimeError('cannot inspect an owned process for active game mappings')
