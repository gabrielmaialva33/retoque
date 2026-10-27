#!/usr/bin/env python3
"""Compile actual Vulkan shader/route bodies against host fake Vulkan calls.

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


def route_source(root):
    actual = root/'retoque/dlssnr/DlssNrFeature_Vk.cpp'
    if actual.exists():
        return actual.read_text(), actual
    excerpt = root/'feature-free-excerpts.txt'
    if not excerpt.exists():
        raise FileNotFoundError('need production DlssNrFeature_Vk.cpp (or staged baseline excerpt)')
    # The staged excerpt has original line-number prefixes. Remove only those;
    # extraction still executes the complete function bodies, without rewrites.
    return '\n'.join(match[1] for line in excerpt.read_text().splitlines()
                     if (match := re.match(r'^\d+ (.*)$', line))), excerpt


def generate(root, destination):
    destination.mkdir(parents=True, exist_ok=True)
    shader = root/'retoque/shaders/dlssnr/DlssNr_Vk.cpp'
    source = shader.read_text()
    has_reserve = bool(re.search(r'\bDlssNr_Vk::Reserve\s*\(', code_mask(source)))
    functions = [extract(source, r'^bool\s+DlssNr_Vk::CreateDummy\s*\('),
                 extract(source, r'^void\s+DlssNr_Vk::WriteDescriptors\s*\(')]
    if has_reserve:
        functions.append(extract(source, r'^std::optional\s*<\s*DlssNr_Vk::Batch\s*>\s+DlssNr_Vk::Reserve\s*\('))
    functions.append(extract(source, r'^bool\s+DlssNr_Vk::Dispatch\s*\('))
    dispatch = destination/'dispatch.cpp'
    dispatch.write_text(f'#define TEST_HAS_RESERVE {int(has_reserve)}\n'+
                        (HERE/'dispatch_cases.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', '\n\n'.join(functions)))
    route_text, route_path = route_source(root)
    busy_signature = r'\bbool\s+RouteRecordingBusy\s*\('
    busy = (extract(route_text, busy_signature) if re.search(busy_signature, code_mask(route_text))
            else 'bool RouteRecordingBusy() { return false; } // Baseline compatibility only; production bodies remain unchanged.')
    begin_signature = r'\bvoid\s+BeginRouteRecording\s*\('
    begin = (extract(route_text, begin_signature) if re.search(begin_signature, code_mask(route_text))
             else 'void BeginRouteRecording() {} // Baseline has no per-recording layout reset.')
    functions = [busy, begin, extract(route_text, r'\bvoid\s+Transition\s*\('),
                 extract(route_text, r'\bvoid\s+FreeRouteObjects\s*\('),
                 extract(route_text, r'\bvoid\s+ShutdownVk\s*\('),
                 extract(route_text, r'\bAfterUpscaleHandBack\s+FinishAfterUpscaleHandBack\s*\('),
                 extract(route_text, r'\bvoid\s+AfterUpscaleDeviceGone\s*\(')]
    route = destination/'route.cpp'
    route.write_text((HERE/'route_cases.cpp').read_text().replace('// PRODUCTION_FUNCTIONS', '\n\n'.join(functions)))
    print(f'Production shader: {shader}; Reserve API: {has_reserve}')
    print(f'Production route: {route_path}')
    return [dispatch, route]


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
                     '-I'+str(root/'retoque/shaders/dlssnr')]
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
    with tempfile.TemporaryDirectory(prefix='nr-vk-production-integration-') as temporary:
        return run(args.root.resolve(), Path(temporary), args.compiler or ['g++','clang++'])


if __name__ == '__main__':
    raise SystemExit(main())
