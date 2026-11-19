#!/usr/bin/env python3
"""Build the real menu + Dear ImGui and draw deterministic CPU-only fixtures."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
from generate import generate, ROOT


def build(directory):
    generate(directory)
    imgui = ROOT / 'OptiScaler/include/imgui'
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'freetype2'], text=True))
    binary = directory / 'menu-preview'
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g0',
                    '-I', str(directory), '-I', str(ROOT / 'tools/menu-preview'),
                    '-I', str(ROOT / 'retoque'), '-I', str(ROOT / 'OptiScaler'),
                    '-I', str(ROOT / 'OptiScaler/include'),
                    str(directory / 'menu.generated.cpp'),
                    *[str(imgui / f'imgui{name}.cpp') for name in ('', '_draw', '_widgets', '_tables')],
                    str(imgui / 'misc/freetype/imgui_freetype.cpp'),
                    *flags, '-o', str(binary)], check=True)
    return binary


def run(binary, output, states):
    available = subprocess.check_output([str(binary), '--list'], text=True).splitlines()
    for state in states or available:
        if state not in available:
            raise ValueError(f'Unknown state: {state}; choose from {", ".join(available)}')
        subprocess.run([str(binary), state, str(output), str(ROOT)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='PNG directory outside the repository (default: temporary directory)')
    parser.add_argument('--state', action='append', help='Named state; repeat or omit for all states')
    args = parser.parse_args()
    output = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='retoque-menu-pictures-'))
    if output.is_relative_to(ROOT):
        parser.error('Pictures must go outside the repository')
    with tempfile.TemporaryDirectory(prefix='retoque-menu-build-') as temporary:
        binary = build(Path(temporary))
        run(binary, output, args.state)
    print(f'Pictures: {output}')


if __name__ == '__main__':
    main()
