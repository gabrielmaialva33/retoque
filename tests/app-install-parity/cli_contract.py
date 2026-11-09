"""CLI status/argument and interrupted-uninstall contracts on a synthetic release."""
import json
import os
from pathlib import Path
import subprocess
import tempfile


def run(executable):
    env = {k: v for k, v in os.environ.items() if not k.startswith('RETOQUE_DEPLOY_')}
    with tempfile.TemporaryDirectory(prefix='retoque-cli-') as tmp:
        root = Path(tmp)
        game = root / 'game'
        source = root / 'source'
        game.mkdir()
        source.mkdir()
        (source / 'OptiScaler.dll').write_bytes(b'MZ OptiScaler working as %s')
        (source / 'nvngx.dll_dlssnr.dll').write_bytes(b'MZ dlssnr_call_create')
        (source / 'OptiScaler.ini').write_bytes(b'[DlssNr]\r\nEnabled=auto\r\n')

        def invoke(*args, extra=None):
            return subprocess.run([executable, *map(str, args)], env={**env, **(extra or {})}, capture_output=True, text=True, timeout=30)

        empty = invoke('status', '--json', game)
        assert empty.returncode == 0
        state = json.loads(empty.stdout)
        assert state['receipt'] is None and state['nr_enabled'] is None and not state['model_present']
        assert list(game.iterdir()) == []
        assert invoke('install', '--unknown', game).returncode == 1
        assert invoke('uninstall', game, 'dxgi.dll').returncode == 1
        assert list(game.iterdir()) == []
        assert invoke('--help').returncode == 0
        installed = invoke('install', '--source-dir', source, '--no-nvngx', '--enable-nr', game, 'winmm.dll')
        assert installed.returncode == 0, installed.stderr
        state = json.loads(invoke('status', game, '--json').stdout)
        assert state['receipt']['status'] == 'complete' and state['nr_enabled'] is True
        assert state['optiscaler_proxies'] == ['winmm.dll']
        assert state['launch_option'] == 'WINEDLLOVERRIDES="winmm=n,b" %command%'
        assert len(state['build_sha256']) == 64
        stopped = invoke('uninstall', game, extra={'RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP': '2'})
        assert stopped.returncode == 1 and 'uninstall-incomplete' in stopped.stderr
        state = json.loads(invoke('status', game, '--json').stdout)
        assert state['receipt']['status'] == 'uninstall-incomplete'
        assert invoke('uninstall', game).returncode == 0
        assert list(game.iterdir()) == []
    print('PASS: CLI JSON status, usage refusals, and interrupted-uninstall recovery')
    return 0
