#!/usr/bin/env python3
"""Extract actual feature helpers, or the canonical old consumer for behavioral RED.

Real Ring, Recording registry and Exposure policies are compiled. No GPU work is
executed; mapped float writes model externally completed transfers.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
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
    if not match:
        raise ValueError(f'production function missing: {signature}')
    opening = masked.find('{', match.end())
    depth = 1
    for position in range(opening + 1, len(masked)):
        if masked[position] == '{':
            depth += 1
        elif masked[position] == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():position + 1]
    raise ValueError(f'production function not closed: {signature}')


def generate(source, target):
    text = source.read_text()
    modern = bool(re.search(r'\bReserveMeterReadback\s*\(', code_mask(text)))
    if modern:
        functions = [extract(text, r'^bool\s+MeterReadbackReady\s*\('),
                     extract(text, r'^std::optional\s*<\s*VkMeterReadback::Reservation\s*>\s+ReserveMeterReadback\s*\('),
                     extract(text, r'^void\s+ConsumeMeterReadback\s*\('),
                     extract(text, r'^void\s+ForgetMeterReadback\s*\(')]
    else:
        start = text.index('// Take the grid written four frames ago.')
        end = text.index('// Said when it moves', start)
        # The canonical age-based statements are preserved exactly; only the
        # surrounding standalone test function is added.
        functions = ['void ConsumeMeterReadback()\n{\n' + text[start:end] + '\n}']
    target.write_text(f'#define TEST_FEATURE_HELPERS {int(modern)}\n' +
                      (HERE / 'integration_cases.cpp').read_text().replace(
                          '// PRODUCTION_FUNCTIONS', '\n\n'.join(functions)))
    print(f'Production source: {source}; actual new helpers: {modern}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--support-root', type=Path, help='staged root for actual Recording and Exposure headers')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--compiler', action='append')
    parser.add_argument('--generate-only', type=Path)
    args = parser.parse_args()
    source = args.source or args.root / 'retoque/dlssnr/DlssNrFeature_Vk.cpp'
    if args.generate_only:
        generate(source, args.generate_only)
        return
    with tempfile.TemporaryDirectory(prefix='nr-vk-meter-feature-') as temporary:
        folder = Path(temporary)
        unit = folder / 'feature.cpp'
        generate(source, unit)
        includes = [args.root / 'retoque/dlssnr']
        if args.support_root:
            includes.append(args.support_root / 'retoque/dlssnr')
        for index, compiler in enumerate(args.compiler or [os.environ.get('CXX', 'g++')]):
            executable = folder / f'feature-{index}'
            flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
            subprocess.run(shlex.split(compiler) + flags + ['-I' + str(path) for path in includes] +
                           [str(unit), '-o', str(executable)], check=True)
            env = dict(os.environ)
            env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
            env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
            subprocess.run([str(executable)], env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
