#!/usr/bin/env python3
"""Compile actual Vulkan scaler bodies against host fake Vulkan calls.

No Wine, Vulkan loader, model, device or GPU is used. Real Vulkan SDK headers
provide the ABI types and declarations; only external effects are substituted.
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
    """Blank strings/comments while retaining all source offsets and braces."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda match: ''.join('\n' if char == '\n' else ' ' for char in match[0]), source)


def extract(source, signature):
    masked = code_mask(source)
    match = re.search(signature, masked, re.M)
    if match is None:
        raise ValueError(f'production function not found: {signature}')
    start = match.start()
    opening = masked.find('{', match.end())
    if opening < 0:
        raise ValueError(f'function body missing: {signature}')
    depth = 1
    for index in range(opening+1, len(masked)):
        if masked[index] == '{':
            depth += 1
        elif masked[index] == '}':
            depth -= 1
            if depth == 0:
                return source[start:index+1]
    raise ValueError(f'unclosed production body: {signature}')


def generate(root, destination):
    destination.mkdir(parents=True, exist_ok=True)
    fork = root/'retoque/shaders/dlssnr/DlssNr_Scaler_Vk.cpp'
    modern = fork.exists()
    source_path = fork if modern else root/'OptiScaler/shaders/output_scaling/OS_Vk.cpp'
    source = source_path.read_text()
    if modern:
        signatures = [r'^DlssNr_Scaler_Vk::DlssNr_Scaler_Vk\s*\(',
                      r'^std::optional\s*<\s*DlssNr_Scaler_Vk::Batch\s*>\s+DlssNr_Scaler_Vk::Reserve\s*\(',
                      r'^bool\s+DlssNr_Scaler_Vk::Dispatch\s*\(']
    else:
        signatures = [r'^Scaler\s+OS_Vk::ActiveScaler\s*\(', r'^bool\s+OS_Vk::Dispatch\s*\(']
    functions = '\n\n'.join(extract(source, signature) for signature in signatures)
    unit = destination/'scaler.cpp'
    unit.write_text(f'#define TEST_RECORDING_SCALER {int(modern)}\n' +
                    (HERE/'scaler_cases.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', functions))
    print(f'Production scaler: {source_path}; recording-safe API: {modern}', flush=True)
    return [unit]


def vulkan_include(root):
    configured = os.environ.get('VULKAN_HEADERS')
    candidates = ([Path(configured)] if configured else []) + [
        root/'external/vulkan/include', root/'OptiScaler/external/vulkan/include', Path('/usr/include')]
    for candidate in candidates:
        if (candidate/'vulkan/vulkan.h').is_file():
            return candidate
    raise FileNotFoundError('real Vulkan headers required: set VULKAN_HEADERS to their include directory')


def run(root, directory, compilers):
    units = generate(root, directory)
    vulkan = vulkan_include(root)
    failed = 0
    for compiler in compilers:
        command = shlex.split(compiler)
        if not command or shutil.which(command[0]) is None:
            raise FileNotFoundError(f'C++ compiler unavailable: {compiler}')
        for unit in units:
            executable = directory/(unit.stem+'-'+Path(command[0]).name)
            flags = ['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
                     '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
                     '-I'+str(HERE/'stubs'), '-I'+str(vulkan), '-I'+str(root/'retoque'),
                     '-I'+str(root/'retoque/shaders/dlssnr'), '-I'+str(root/'OptiScaler')]
            built = subprocess.run(command+flags+[str(unit),'-o',str(executable)], check=False)
            if built.returncode:
                failed += 1
                continue
            env = dict(os.environ)
            env.setdefault('ASAN_OPTIONS','detect_leaks=1:abort_on_error=1')
            env.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
            result = subprocess.run([str(executable)], env=env, check=False, timeout=30)
            failed += result.returncode != 0
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--generate-only', type=Path, help='write extracted translation units without compiling')
    parser.add_argument('--compiler', action='append', help='repeat to select compilers; default: g++ and clang++')
    args = parser.parse_args()
    if args.generate_only:
        generate(args.root.resolve(), args.generate_only)
        return 0
    with tempfile.TemporaryDirectory(prefix='nr-vk-scaler-') as temporary:
        return run(args.root.resolve(), Path(temporary), args.compiler or ['g++','clang++'])


if __name__ == '__main__':
    raise SystemExit(main())
