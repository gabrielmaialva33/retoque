#!/usr/bin/env python3
"""Execute exact production meter allocation bodies with controlled Vulkan calls.

This host-only suite proves ownership, failure cleanup and complete-tuple readiness.
It does not execute Vulkan commands or establish GPU readback retirement.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def code_mask(source):
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda match: ''.join('\n' if char == '\n' else ' ' for char in match[0]), source)


def extract(source, signature):
    masked = code_mask(source)
    match = re.search(signature, masked, re.M)
    if match is None:
        raise ValueError(f'production function not found: {signature}')
    opening = masked.find('{', match.end())
    depth = 1
    for index in range(opening + 1, len(masked)):
        if masked[index] == '{':
            depth += 1
        elif masked[index] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():index + 1]
    raise ValueError(f'unclosed production function: {signature}')


def generate(source, destination):
    production = source.read_text()
    functions = []
    ready = r'^bool\s+MeterReadbackReady\s*\('
    has_ready = bool(re.search(ready, code_mask(production), re.M))
    if has_ready:
        functions.append(extract(production, ready))
    functions += [extract(production, r'^bool\s+CreateMeterReadback\s*\('),
                  extract(production, r'^void\s+DestroyMeterReadback\s*\(')]
    destination.write_text(f'#define TEST_HAS_READY {int(has_ready)}\n' +
                           (HERE / 'cases.cpp').read_text().replace(
                               '// PRODUCTION_FUNCTIONS', '\n\n'.join(functions)))
    print(f'Production source: {source}; complete-tuple helper: {has_ready}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--source', type=Path, help='explicit canonical source path for staged host runs')
    parser.add_argument('--compiler', action='append', help='repeat to select compilers; defaults to CXX or g++')
    parser.add_argument('--generate-only', type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    source = args.source or root / 'retoque/dlssnr/DlssNrFeature_Vk.cpp'
    if args.generate_only:
        generate(source, args.generate_only)
        return 0
    compilers = args.compiler or [os.environ.get('CXX', 'g++')]
    failed = False
    with tempfile.TemporaryDirectory(prefix='nr-vk-meter-') as temporary:
        directory = Path(temporary)
        unit = directory / 'meter.cpp'
        generate(source, unit)
        for compiler in compilers:
            command = shlex.split(compiler)
            if not command or shutil.which(command[0]) is None:
                raise FileNotFoundError(f'C++ compiler unavailable: {compiler}')
            executable = directory / ('meter-' + Path(command[0]).name)
            flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                     '-I' + str(HERE)]
            print('Compiler:', compiler, flush=True)
            built = subprocess.run(command + flags + [str(unit), '-o', str(executable)], check=False)
            if built.returncode:
                failed = True
                continue
            env = dict(os.environ)
            env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
            env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
            result = subprocess.run([str(executable)], env=env, check=False, timeout=30)
            failed |= result.returncode != 0
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
