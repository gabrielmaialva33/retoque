#!/usr/bin/env python3
"""Host-only recording-retirement contract tests; no Vulkan calls or GPU execution."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--recording-root', type=Path, help='staged root containing existing recording header')
    parser.add_argument('--compiler', action='append')
    parser.add_argument('--unit-only', action='store_true', help='run Ring/Recording contracts without feature extraction')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='nr-vk-meter-readback-') as directory:
        for index, compiler in enumerate(args.compiler or [os.environ.get('CXX', 'g++')]):
            executable = Path(directory) / f'cases-{index}'
            command = shlex.split(compiler)
            includes = [args.root / 'retoque/dlssnr']
            if args.recording_root:
                includes.append(args.recording_root / 'retoque/dlssnr')
            flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            subprocess.run(command + flags + ['-I' + str(path) for path in includes] +
                           [str(HERE / 'cases.cpp'), '-o', str(executable)], check=True)
            env = dict(os.environ)
            env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
            env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
            subprocess.run([str(executable)], check=True, env=env, timeout=30)
    if not args.unit_only:
        command = [sys.executable, str(HERE / 'integration.py'), '--root', str(args.root)]
        if args.recording_root:
            command += ['--support-root', str(args.recording_root)]
        for compiler in args.compiler or []:
            command += ['--compiler', compiler]
        subprocess.run(command, check=True, timeout=180)


if __name__ == '__main__':
    main()
