#!/usr/bin/env python3
"""Compile actual NGX FG create/release methods with a fake provider and instrumented mutex."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/framegen/nvngx/Nvngx_FG.cpp').read_text()
header = (root / 'OptiScaler/framegen/nvngx/Nvngx_FG.h').read_text()
def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]
methods = [function('NVSDK_NGX_Result Nvngx_FG::' + name + '(') for name in
           ('D3D12_CreateFeature', 'VULKAN_CreateFeature', 'VULKAN_CreateFeature1',
            'D3D12_ReleaseFeature', 'VULKAN_ReleaseFeature')]
declarations = '\n'.join('    static ' + m[:m.index('{')].replace('Nvngx_FG::', '').strip() + ';' for m in methods)
a = header.index('    struct Nvngx_FG_Handle'); b = header.index('    static inline', a)
handle = header[a:b].replace('std::shared_mutex', 'InspectMutex')
harness = (here / 'nvngx_lifetime_cases.cpp').read_text().replace('// @handle@', handle).replace(
    '// @declarations@', declarations).replace('// @implementations@', '\n'.join(methods))
with tempfile.TemporaryDirectory(prefix='retoque-fg-handle-') as tmp:
    cpp, binary = Path(tmp) / 'case.cpp', Path(tmp) / 'case'
    cpp.write_text(harness)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    for mode in (list(map(int, sys.argv[1:])) if len(sys.argv) > 1 else range(7)):
        print(f'case {mode}', flush=True)
        subprocess.run([str(binary), str(mode)], check=True)
