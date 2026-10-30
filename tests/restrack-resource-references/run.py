#!/usr/bin/env python3
"""D3D12 resource/descriptor reverse-reference tracking: production slices, differential and stress tests."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ANKERL = ROOT / 'external/unordered_dense/include'


def function(signature, source):
    begin = source.index(signature)
    start = source.index('{', begin)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def between(source, start, stop, inclusive=False):
    begin = source.index(start)
    end = source.index(stop, begin)
    return source[begin:end + (len(stop) if inclusive else 0)]


hudfix = (ROOT / 'OptiScaler/hudfix/Hudfix_Dx12.h').read_text()
header = (ROOT / 'OptiScaler/resource_tracking/ResTrack_dx12.h').read_text()
source = (ROOT / 'OptiScaler/resource_tracking/ResTrack_dx12.cpp').read_text()

# Verbatim production code: ResourceInfo, the tracking globals plus HeapInfo, and the two lifetime functions.
slices = '\n'.join([
    between(hudfix, 'typedef struct ResourceInfo', '} resource_info;', inclusive=True),
    between(header, 'struct HeapInfo;\n', 'struct ResourceHeapInfo'),
    function('void __stdcall ResTrack_Dx12::ResourceDestroyed(void* data)', source),
    function('bool ResTrack_Dx12::TrackResourceRelease(ID3D12Resource* resource)', source),
])

backends = [('std::unordered_map stand-in', HERE / 'shim')]
if (ANKERL / 'ankerl/unordered_dense.h').exists():
    backends.append(('ankerl::unordered_dense', ANKERL))
else:
    print('NOTE: external/unordered_dense not checked out; the production ankerl map is not exercised')

CXX = ['g++', '-std=c++20', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror', '-fno-omit-frame-pointer']
SANITIZERS = {'asan+ubsan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'],
              'tsan': ['-fsanitize=thread']}

if '--bench' in sys.argv[1:]:
    # Synthetic timing only, never part of pass/fail: optimised, unsanitised, production map when available.
    include = backends[-1][1]
    with tempfile.TemporaryDirectory(prefix='optiscaler-restrack-bench-') as directory:
        binary = Path(directory) / 'bench'
        subprocess.run(['g++', '-std=c++20', '-O2', '-DNDEBUG', '-pthread', '-I', str(include),
                        '-I', str(ROOT / 'retoque'), str(HERE / 'bench.cpp'), '-o', str(binary)], check=True)
        print(f'map backend: {backends[-1][0]}', flush=True)
        sys.exit(subprocess.run([str(binary)]).returncode)

failed = False
with tempfile.TemporaryDirectory(prefix='optiscaler-restrack-references-') as directory:
    directory = Path(directory)
    unit = directory / 'slices.cpp'
    unit.write_text((HERE / 'fakes.h').read_text() + '\n' + slices + '\n' + (HERE / 'cases.cpp').read_text())

    for backend, include in backends:
        for sanitizer, flags in SANITIZERS.items():
            binary = directory / f'slices-{len(include.parts)}-{sanitizer}'
            print(f'--- production slices, {backend}, {sanitizer}', flush=True)
            subprocess.run(CXX + flags + ['-I', str(include), '-I', str(ROOT / 'retoque'),
                                          str(unit), '-o', str(binary)], check=True)
            # TSan only adds value where threads race; ASan/UBSan runs every case.
            cases = ['slice-stress'] if sanitizer == 'tsan' else ['all']
            for case in cases:
                failed |= subprocess.run([str(binary), case]).returncode != 0

            # The index alone against the legacy model: 4 x 100k differential ops, then 8 threads x 250k
            # (2M) concurrent updates; TSan runs the concurrent case at 8 x 50k.
            binary = directory / f'index-{len(include.parts)}-{sanitizer}'
            print(f'--- reference index vs legacy model, {backend}, {sanitizer}', flush=True)
            subprocess.run(CXX + flags + ['-I', str(include), '-I', str(ROOT / 'retoque'),
                                          str(HERE / 'index.cpp'), '-o', str(binary)], check=True)
            runs = [['stress', '50000']] if sanitizer == 'tsan' else [['all', '250000']]
            for arguments in runs:
                failed |= subprocess.run([str(binary)] + arguments).returncode != 0

sys.exit(1 if failed else 0)
