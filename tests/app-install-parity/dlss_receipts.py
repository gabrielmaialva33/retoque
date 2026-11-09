"""Both legacy uninstallers consume an app-generated standalone root DLSS v1 receipt."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
REPO = Path(__file__).resolve().parents[2]

def run(_executable):
    for impl in ['bash', 'powershell']:
        if impl == 'powershell' and not shutil.which('pwsh'):
            print('SKIP: DLSS root PowerShell parity needs pwsh')
            continue
        with tempfile.TemporaryDirectory(prefix='dlss-root-') as temp:
            root = Path(temp)
            subprocess.run(['cargo', 'run', '--quiet', '-p', 'retoque-core', '--example', 'dlss_fixture', '-j', '4', '--', str(root)], cwd=REPO / 'tools/app', check=True)
            game = root / 'game'
            dll = game / 'Engine/Plugins/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll'
            assert dll.read_bytes() == b'synthetic updated DLSS'
            if impl == 'bash':
                cmd = ['bash', str(REPO/'tools/deploy-to-game.sh'), '--uninstall', str(game)]
            else:
                cmd = ['pwsh', '-NoProfile', '-NonInteractive', '-File', str(REPO/'tools/install-windows.ps1'), '-Uninstall', '-GameDir', str(game)]
            env = {k:v for k,v in os.environ.items() if not k.startswith('RETOQUE_DEPLOY_')}
            result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, result.stdout + result.stderr
            assert dll.read_bytes() == b'synthetic original DLSS'
            assert not (game/'.retoque/receipt.txt').exists()
            assert not (game/'.retoque/dlss.txt').exists()
            print(f'PASS: app DLSS root receipt -> {impl} restores original bytes')
    return 0
