#!/usr/bin/env python3
"""The exposure scan's candidate lifetime and capacity (docs/dlssnr/design/exposure-scan-capacity.md): the production
DlssNr_ExposureScan.cpp compiled against fake D3D12 objects and the production submission model under ASan/UBSan,
one case per process, then source guards on its call sites. No D3D12 runtime, GPU, driver or game."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]


def read(relative):
    return (root / relative).read_text(encoding='utf-8')


def function(text, signature):
    """The body of the one definition that starts with `signature`, braces matched outside strings and comments."""
    assert text.count(signature) == 1, f'expected one definition: {signature!r} ({text.count(signature)})'
    i = text.index('{', text.index(signature))
    depth, j = 0, i
    while True:
        c = text[j]
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        if c in '"\'':
            j += 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1


failures = []

with tempfile.TemporaryDirectory(prefix='nr-exposure-scan-') as directory:
    binary = Path(directory) / 'cases'
    flags = ['g++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall',
             '-Wextra', '-Werror', '-I', str(here / 'stubs'), '-I', str(root), '-I', str(root / 'retoque')]
    # The production unit has an unused parameter MSVC does not warn about (NoteUav's view description).
    subprocess.run(flags + ['-Wno-unused-parameter', '-c', str(root / 'retoque/dlssnr/DlssNr_ExposureScan.cpp'),
                            '-o', str(Path(directory) / 'scan.o')], check=True)
    subprocess.run(flags + ['-c', str(here / 'cases.cpp'), '-o', str(Path(directory) / 'cases.o')], check=True)
    subprocess.run(flags + [str(Path(directory) / 'cases.o'), str(Path(directory) / 'scan.o'), '-o', str(binary)],
                   check=True)
    names = subprocess.run([str(binary), '--list'], check=True, capture_output=True, text=True).stdout.split()
    assert len(names) >= 15, names
    for name in names:
        result = subprocess.run([str(binary), name], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            sys.stdout.write(result.stderr[-4000:])
            failures.append(name)

# --- Source guards --------------------------------------------------------------------------------------------------

scan = read('retoque/dlssnr/DlssNr_ExposureScan.cpp')
hooks = read('OptiScaler/hooks/D3D12_Hooks.cpp')
release = read('OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp')
guards = []


def guard(condition, what):
    try:
        ok = bool(condition())
    except (AssertionError, ValueError, TypeError) as error:  # a missing definition or anchor is a failed guard
        ok = False
        what += f' ({type(error).__name__}: {error})'[:160]
    guards.append((ok, what))


def body(text, signature):
    return function(text, signature) if text.count(signature) == 1 else ''


committed = body(hooks, 'static HRESULT hkCreateCommittedResource(')
placed = body(hooks, 'static HRESULT hkCreatePlacedResource(')
guard(lambda: committed.count('DlssNr::ExposureScan::NoteResource(pDesc, (ID3D12Resource*) *ppvResource);') == 1,
      'the committed hook notes its resource, with no heap')
guard(lambda: placed.count('DlssNr::ExposureScan::NoteResource(pDesc, (ID3D12Resource*) *ppvResource, pHeap);') == 1,
      'the placed hook passes its heap')

releaseFeature = body(release, 'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(')
guard(lambda: release.count('DlssNr::ExposureScan::ReleaseTrackedResources();') == 1, 'one boundary call')
guard(lambda: releaseFeature.index('DlssNr::ExposureScan::ReleaseTrackedResources();') <
      releaseFeature.index('auto handleId = InHandle->Id;'), 'the boundary comes before the feature is torn down')

tick = body(scan, 'void Tick(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)')
guard(lambda: 'ProbeLocked(' in tick and tick.index('ProbeLocked(') < tick.index('SweepLocked('),
      'every sweep is preceded by the probe in the same Tick')
sweep = body(scan, 'void SweepLocked(')
guard(lambda: 'Submission::Track(cmdList, slot.usage)' in sweep and 'Submission::Track(cmdList, t.usage)' in sweep,
      'the sweep tracks both the slot and each candidate it copies')
guard(lambda: sweep.index('Submission::Track(cmdList, t.usage)') < sweep.index('CopyBufferRegion('),
      'a candidate is tracked before its copy is recorded')
harvest = body(scan, 'void HarvestLocked(')
guard(lambda: 'Submission::Completed(slot.usage)' in harvest, 'readings come from completed recordings only')
guard(lambda: 'writes >= kSlots' not in scan, 'no reading on elapsed sweeps')
boundary = body(scan, 'void ReleaseTrackedResources()')
guard(lambda: 'Origin::View' in boundary, 'the boundary releases view candidates')
guard(lambda: 'tracked.clear()' not in boundary, 'the boundary does not forget the rest')

for ok, what in guards:
    print(('PASS ' if ok else 'FAIL ') + 'guard: ' + what)
    if not ok:
        failures.append('guard: ' + what)

if failures:
    print(f'FAIL: {len(failures)} of {len(names) + len(guards)}: ' + ', '.join(failures))
    sys.exit(1)
print(f'PASS: {len(names)} cases and {len(guards)} source guards')
