#!/usr/bin/env python3
"""Production registry fault injection with native and allocating-empty-container fixtures."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
CASES = ('cold-registry', 'begin', 'reset', 'reset-pool', 'destroy-pool', 'forget-device', 'free-vector',
         'replace-allocation', 'destructor', 'pin-failure', 'many-records')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    header = (args.root.resolve() / 'retoque/dlssnr/DlssNr_VkRecording.h').read_text()
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    failures = 0
    with tempfile.TemporaryDirectory(prefix='nr-vk-recording-oom-') as directory:
        directory = Path(directory)
        for mode in ('native-containers', 'allocating-empty-containers'):
            include = directory / mode
            (include / 'dlssnr').mkdir(parents=True)
            rewritten = header
            define = []
            if mode == 'allocating-empty-containers':
                for old, new in (
                    ('std::list<std::shared_ptr<void>>', 'AllocatingDefaultList<std::shared_ptr<void>>'),
                    ('std::unordered_map<Handle, Record>', 'AllocatingDefaultMap<Handle, Record>'),
                ):
                    if rewritten.count(old) != 1:
                        raise RuntimeError('the empty-container fixture must wrap exactly one type: ' + old)
                    rewritten = rewritten.replace(old, new)
                define = ['-DNR_TEST_ALLOCATING_LIST']
            (include / 'dlssnr/DlssNr_VkRecording.h').write_text(rewritten)
            executable = directory / (mode + '-cases')
            subprocess.run(compiler + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                                       '-I' + str(include)] + define +
                           [str(HERE / 'oom_cases.cpp'), '-o', str(executable)], check=True)
            for case in CASES:
                # A throwing destructor is noexcept and may abort. Separate processes preserve evidence from all
                # other lifecycle cases and keep an abort in a RED fixture from hiding the remaining results.
                result = subprocess.run([str(executable), case], env=env, text=True, capture_output=True, timeout=30)
                if result.returncode:
                    failures += 1
                    print(f'FAIL: {mode}/{case} (exit {result.returncode})', flush=True)
                    print((result.stderr or result.stdout)[:1500].strip(), flush=True)
                else:
                    print(f'PASS: {mode}/{case}', flush=True)
    if failures:
        raise SystemExit(f'{failures} registry OOM cases failed')
    print('PASS: registry OOM behavior with native and allocating-empty containers (host only)')


if __name__ == '__main__':
    main()
