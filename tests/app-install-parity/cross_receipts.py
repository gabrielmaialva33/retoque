"""Cross-uninstall and upgrade real receipts from Bash, Rust and (when available) PowerShell."""
import importlib.util
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('deploy_cases', REPO / 'tests/deploy-safety/run.py')
cases = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cases)


def run(executable):
    implementations = ['bash', 'rust']
    if shutil.which('pwsh'):
        implementations.append('powershell')
    else:
        print('SKIP: PowerShell cross-receipt cases need pwsh on PATH')
    env = {k: v for k, v in os.environ.items() if not k.startswith('RETOQUE_DEPLOY_')}

    def invoke(impl, game, source=None, mixed=False, partial=False):
        uninstall = source is None
        if impl == 'powershell':
            cmd = ['pwsh', '-NoProfile', '-NonInteractive', '-File', str(REPO / 'tools/install-windows.ps1'), '-GameDir', str(game)]
            cmd += ['-Uninstall'] if uninstall else ['-SourceDir', str(source)]
            if mixed:
                cmd += ['-KeepReShade', '-ReplaceForeign', '-EnableNR']
        else:
            cmd = ['bash', str(REPO / 'tools/deploy-to-game.sh')] if impl == 'bash' else [executable, 'uninstall' if uninstall else 'install']
            if uninstall and impl == 'bash':
                cmd += ['--uninstall']
            if not uninstall:
                cmd += ['--no-nvngx', '--source-dir', str(source)]
            if mixed:
                cmd += ['--keep-reshade', '--replace-foreign', '--enable-nr']
            cmd += [str(game)]
        result = subprocess.run(cmd, env={**env, **({'RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP': '2'} if partial else {})}, capture_output=True, text=True, cwd=REPO, timeout=60)
        assert result.returncode == (1 if partial else 0), f'{impl}: {result.stdout}\n{result.stderr}'

    count = 0
    for producer in implementations:
        for consumer in implementations:
            if producer == consumer:
                continue
            for partial in (False, True):
                with tempfile.TemporaryDirectory(prefix='retoque-cross-') as tmp:
                    root = Path(tmp)
                    source = cases.make_src(root, 'one')
                    (source / 'OptiScaler.ini').write_bytes(b'[DlssNr]\nEnabled=auto\n[Plugins]\nLoadReshade=auto\n')
                    game = cases.make_game(root, {} if partial else {
                        'dxgi.dll': cases.RESHADE,
                        'nvngx.dll_dlssnr.dll': cases.FOREIGN,
                        'OptiScaler.ini': b'[DlssNr]\r\nEnabled=false\r\n[Plugins]\r\nloadreshade =false\r\n',
                        'libxell.dll': b'game xell',
                        'OptiScaler/libxell.dll': b'old support xell',
                        'game.exe': cases.PE,
                    })
                    before = cases.snapshot(game)
                    invoke(producer, game, source, mixed=not partial, partial=partial)
                    invoke(consumer, game)
                    assert cases.snapshot(game) == before, f'{producer} -> {consumer}: snapshot was not restored'
                count += 1
                print(f'PASS: cross receipt {producer} -> {consumer} ({"partial" if partial else "complete"})')
            with tempfile.TemporaryDirectory(prefix='retoque-upgrade-') as tmp:
                root = Path(tmp)
                source = cases.make_src(root, 'one')
                (source / 'OptiScaler.ini').write_bytes(b'[DlssNr]\nEnabled=auto\n')
                game = cases.make_game(root)
                invoke(producer, game, source)
                source2 = cases.make_src(root, 'two')
                (source2 / 'OptiScaler.ini').write_bytes(b'[DlssNr]\nEnabled=auto\n')
                invoke(consumer, game, source2)
                assert cases.receipt(game)['backups'] == [], f'{producer} -> {consumer}: upgrade incorrectly backed up owned files'
                assert (game / 'dxgi.dll').read_bytes() == cases.opti('two')
                invoke(producer, game)
                assert cases.snapshot(game) == {}
            count += 1
            print(f'PASS: cross upgrade {producer} -> {consumer}')
    print(f'{count} cross-receipt cases passed')
    return 0
