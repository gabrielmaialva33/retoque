#!/usr/bin/env python3
"""Build the shared installer and run deploy-safety against its CLI using synthetic folders."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]


def main():
    if not shutil.which('cargo'):
        print('SKIP: cargo is not on PATH; app-install-parity needs the Rust toolchain')
        return 0
    manifest = REPO / 'tools/app/Cargo.toml'
    cmd = ['cargo', 'build', '--manifest-path', str(manifest), '-p', 'retoque-cli', '-j', '4', '--message-format=json-render-diagnostics']
    # On the workstation the scope bounds Rust and its children, including linker memory.
    if sys.platform == 'linux' and shutil.which('systemd-run') and os.environ.get('XDG_RUNTIME_DIR'):
        cmd = ['systemd-run', '--user', '--scope', '-p', 'MemoryHigh=7G', '-p', 'MemoryMax=8G',
               '-p', 'MemorySwapMax=1G', '-p', 'OOMPolicy=kill', '--', *cmd]
    build = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    executable = None
    for line in build.stdout.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            print(line)
            continue
        if item.get('reason') == 'compiler-artifact' and item.get('target', {}).get('name') == 'retoque':
            executable = item.get('executable')
        if item.get('reason') == 'compiler-message':
            print(item['message'].get('rendered', ''), end='')
    print(build.stderr, end='')
    if build.returncode or not executable:
        print('FAIL: could not build the retoque CLI')
        return 1
    result = subprocess.call([sys.executable, str(REPO / 'tests/deploy-safety/run.py'), '--impl', executable], cwd=REPO)
    if result:
        return result
    from cross_receipts import run as cross_receipts
    from cli_contract import run as cli_contract
    from pending_journal import run as pending_journal
    from dlss_receipts import run as dlss_receipts
    return cross_receipts(executable) or cli_contract(executable) or pending_journal(executable) or dlss_receipts(executable)


if __name__ == '__main__':
    sys.exit(main())
