"""A real Rust rename crash must remain recoverable after every legacy-script refusal."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from cross_receipts import REPO, cases


def tree_state(game):
    # Content/mode plus inode/mtime, including directories: even a create/remove or same-byte
    # receipt rewrite would violate the requirement that refusing the journal writes nothing.
    entries = [game, *game.rglob('*')]
    metadata = {str(p.relative_to(game)): (p.lstat().st_ino, p.lstat().st_mtime_ns) for p in entries}
    return cases.snapshot(game), metadata


def run(executable):
    consumers = ['bash']
    if shutil.which('pwsh'):
        consumers.append('powershell')
    else:
        print('SKIP: crash -> PowerShell journal refusal needs pwsh on PATH')
    if not shutil.which('cc'):
        print('FAIL: pending-journal crash tests need cc for the child-only rename interceptor')
        return 1
    env = {k: v for k, v in os.environ.items() if not k.startswith('RETOQUE_DEPLOY_') and k != 'LD_PRELOAD'}
    failed = []
    with tempfile.TemporaryDirectory(prefix='retoque-journal-cross-') as tmp:
        root = Path(tmp)
        interceptor = root / 'crash.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(REPO / 'tools/app/crates/retoque-cli/tests/fixtures/crash_rename.c'),
                        '-ldl', '-o', str(interceptor)], check=True, capture_output=True)
        for consumer in consumers:
            try:
                fixture = root / consumer
                fixture.mkdir()
                source = cases.make_src(fixture, 'pending')
                (source / 'OptiScaler.ini').write_bytes(b'[Plugins]\nLoadReshade=false\n')
                game = cases.make_game(fixture, {'dxgi.dll': cases.RESHADE, 'game.exe': cases.PE})
                original = cases.snapshot(game)
                crash = subprocess.run([executable, 'install', '--no-nvngx', '--keep-reshade',
                                        '--source-dir', str(source), str(game)],
                                       env={**env, 'LD_PRELOAD': str(interceptor),
                                            'RETOQUE_CRASH_SUFFIX': 'ReShade64.dll', 'RETOQUE_CRASH_PHASE': 'after'},
                                       capture_output=True, text=True, timeout=60)
                assert crash.returncode == 86, f'real rename did not hit the interceptor: {crash}'
                assert (game / '.retoque/receipt.txt').is_file()
                assert (game / '.retoque/pending-receipt.txt').is_file()
                assert not (game / 'dxgi.dll').exists()
                assert (game / 'ReShade64.dll').read_bytes() == cases.RESHADE
                before = tree_state(game)
                for mode in ('uninstall', 'install', 'uninstall-dry', 'install-dry'):
                    uninstall = mode.startswith('uninstall')
                    dry = mode.endswith('-dry')
                    if consumer == 'bash':
                        command = ['bash', str(REPO / 'tools/deploy-to-game.sh')]
                        command += ['--uninstall'] if uninstall else ['--no-nvngx', '--source-dir', str(source)]
                        command += ['--dry-run'] if dry else []
                        command += [str(game)]
                    else:
                        command = ['pwsh', '-NoProfile', '-NonInteractive', '-File',
                                   str(REPO / 'tools/install-windows.ps1'), '-GameDir', str(game)]
                        command += ['-Uninstall'] if uninstall else ['-SourceDir', str(source)]
                        command += ['-DryRun'] if dry else []
                    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
                    assert result.returncode == 1, f'{mode} accepted pending journal (exit {result.returncode}); base receipt survived={(game / ".retoque/receipt.txt").exists()}'
                    message = result.stdout + result.stderr
                    assert 'interrupted Retoque app operation' in message and "app or 'retoque' first" in message, message
                    assert tree_state(game) == before, f'{mode} changed the game or recovery state'
                recovered = subprocess.run([executable, 'uninstall', str(game)], env=env,
                                           capture_output=True, text=True, timeout=60)
                assert recovered.returncode == 0, f'core recovery failed: {recovered}'
                assert cases.snapshot(game) == original, 'original mod/game not restored after refusals'
                print(f'PASS: real Rust rename crash -> {consumer} refuses 4 operations unchanged -> Rust recovers')
            except AssertionError as error:
                failed.append(consumer)
                print(f'FAIL: pending-journal {consumer}: {error}')
    print(f'{len(consumers) - len(failed)} pending-journal crash/recovery cases passed, {len(failed)} failed')
    return bool(failed)
