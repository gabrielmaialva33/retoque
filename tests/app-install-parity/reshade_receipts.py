"""Offline ReShade/add-on plans, recovery and unchanged v1 receipt compatibility."""
from pathlib import Path
import os
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]


def pe(markers):
    data = bytearray(328)
    data[:2] = b'MZ'
    data[60:64] = (64).to_bytes(4, 'little')
    data[64:68] = b'PE\0\0'
    data[68:70] = (0x8664).to_bytes(2, 'little')
    data[84:86] = (240).to_bytes(2, 'little')
    data[86:88] = (0x2000).to_bytes(2, 'little')
    data[88:90] = (0x20b).to_bytes(2, 'little')
    return bytes(data) + markers


def run(executable):
    def cli(*args):
        result = subprocess.run([executable, *map(str, args)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout

    with tempfile.TemporaryDirectory(prefix='retoque-reshade-parity-') as temp:
        root = Path(temp)
        game = root / 'game'
        game.mkdir()
        (game / 'dxgi.dll').write_bytes(b'OptiScaler working as synthetic')
        original = b'; user\r\n[Plugins]\r\nOther=true'
        (game / 'OptiScaler.ini').write_bytes(original)
        preset = b'[GENERAL]\r\nPresetPath=custom.ini\r\n'
        (game / 'ReShade.ini').write_bytes(preset)
        source = root / 'ReShade64.dll'
        source.write_bytes(pe(b'ReShade LoadFromDllMain'))
        addon = root / 'renodx-dlss5.addon64'
        addon.write_bytes(pe(b'synthetic addon'))
        cli('reshade', 'install', '--dry-run', game, source)
        assert not (game / '.retoque').exists()
        cli('reshade', 'install', game, source)
        cli('addon', 'add', game, addon)
        cli('addon', 'disable', game, addon.name)
        assert addon.is_file(), 'import moved the original'
        cli('addon', 'remove', game, addon.name)
        cli('reshade', 'uninstall', game)
        assert (game / 'OptiScaler.ini').read_bytes() == original
        assert (game / 'ReShade.ini').read_bytes() == preset
        assert not (game / source.name).exists()
        assert not (game / addon.name).exists()
        # The old scripts can still consume the component's ordinary File/Backup records.
        limited = pe(b'ReShade only limited add-on functionality')
        (game / 'ReShade64.dll').write_bytes(limited)
        cli('reshade', 'install', game, source)
        result = subprocess.run(['bash', str(REPO / 'tools/deploy-to-game.sh'), '--uninstall', str(game)],
                                capture_output=True, text=True, env=os.environ.copy())
        assert result.returncode == 0, result.stdout + result.stderr
        assert (game / 'ReShade64.dll').read_bytes() == limited
        assert (game / 'ReShade.ini').read_bytes() == preset
    print('PASS: ReShade/add-on dry-run, byte-exact undo, source preservation and Bash receipt-v1 removal')
    return 0
