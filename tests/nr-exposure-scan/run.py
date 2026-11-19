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
sys.path.insert(0, str(here.parent / '_support'))
from internal_scope import DECLARATION as SCOPE, scope_class  # noqa: E402  the production scope class, from State.h


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
    # stubs/pch.h includes it: the scope the scan skips is the production class, not a stand-in.
    (Path(directory) / 'internal_scope.h').write_text('#pragma once\n' + scope_class())
    flags = ['g++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Wall',
             '-Wextra', '-Werror', '-pthread', '-I', str(here / 'stubs'), '-I', directory, '-I', str(root),
             '-I', str(root / 'retoque')]

    def build(name, extra):
        out = Path(directory) / name
        # The production unit has an unused parameter MSVC does not warn about (NoteUav's view description).
        subprocess.run(flags + extra + ['-Wno-unused-parameter', '-c',
                                        str(root / 'retoque/dlssnr/DlssNr_ExposureScan.cpp'),
                                        '-o', str(out) + '-scan.o'], check=True)
        subprocess.run(flags + extra + ['-c', str(here / 'cases.cpp'), '-o', str(out) + '-cases.o'], check=True)
        subprocess.run(flags + [str(out) + '-cases.o', str(out) + '-scan.o', '-o', str(out)], check=True)
        return out

    binary = build('cases', [])
    # Review E-05: the copy budget, made small enough to reach in a few frames.
    budget = build('cases-budget', ['-DDLSSNR_EXPOSURE_SCAN_COPY_BUDGET=8'])
    names = subprocess.run([str(binary), '--list'], check=True, capture_output=True, text=True).stdout.split()
    assert len(names) >= 26, names
    runs = [(binary, name) for name in names] + [(budget, 'budget')]
    for exe, name in runs:
        result = subprocess.run([str(exe), name], capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode != 0:
            sys.stdout.write(result.stderr[-4000:])
            failures.append(name)
    names.append('budget')

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
sweep = body(scan, 'const char* SweepLocked(')
guard(lambda: 'Submission::Track(cmdList, slot.usage)' in sweep and 'Submission::Track(cmdList, t.usage)' in sweep,
      'the sweep tracks both the slot and each candidate it copies')
guard(lambda: sweep.index('Submission::Track(cmdList, t.usage)') < sweep.index('CopyBufferRegion('),
      'a candidate is tracked before its copy is recorded')
harvest = body(scan, 'void HarvestLocked(')
guard(lambda: 'Submission::Completed(slot.usage)' in harvest, 'readings come from completed recordings only')
guard(lambda: 'writes >= kSlots' not in scan, 'no reading on elapsed sweeps')
boundary = body(scan, 'void ReleaseTrackedResources()')
carried = body(scan, 'bool CarriedAcrossRelease(const Tracked& t)')
guard(lambda: 'CarriedAcrossRelease(*it)' in boundary and 'Origin::Committed' in carried,
      'the boundary carries only committed candidates (review E-03); placed and view ones are released there')
guard(lambda: 'tracked.clear()' not in boundary, 'the boundary does not forget the rest')
guard(lambda: boundary.index('!Wanted()') < boundary.index('LetGoLocked(true);') < boundary.index('auto boundary'),
      'with the scan or NR off, the boundary lets everything go (review E-02)')
guard(lambda: tick.index('if (!Wanted())') < tick.index('LetGoLocked(false);') < tick.index('BlindSinceStart()'),
      'a Tick with the scan off lets go of what is held (review E-02)')
guard(lambda: "!g_scan.slots[s].pending && DlssNr::Submission::Settled(g_scan.slots[s].usage)" in sweep,
      'a readback slot is reused only once its last recording settled (review E-04)')
submission = read('retoque/dlssnr/DlssNr_Submission.cpp')
prune = body(submission, 'void PruneFinished(Usage& usage)')
guard(lambda: 'Detail::PruneFinished(' in prune and 'RefreshOrphans(' not in prune and 'PruneLifetime' not in prune,
      'PruneFinished drops finished epochs only, without the capacity cut or an orphan refresh (review E-01, cost)')
stub = read('tests/nr-exposure-scan/cases.cpp')
guard(lambda: 'void PruneFinished(Usage& usage) { Detail::PruneFinished(' in stub,
      'the suite\'s PruneFinished stub is the production rule')

# OptiScaler's own creations (review 2026-11-18, item 8). The scan skips what is created inside
# ScopedInternalResourceCreation; that is only as good as the scopes at the places Retoque creates through the game's
# device. A PCSX2 session with no game UAV at all spent 32 of its 40 near-miss lines on the pass's scratch and meter,
# the model's buffers and the zero guides (the other eight look like FFX's swapchain outputs, which no scope here
# covers), and three of the pass's textures are shaped like a number (the enlargement's 1x1 exposure texel, cadence
# smoothing's 4x1 light state and 16x16 light grid).
scope = scope_class()
guard(lambda: 'static thread_local int depth = 0;' in scope and
      not any(word in scope for word in ('mutex', 'lock', 'atomic', 'State::', 'Config::')),
      'the scope is a counter on its own thread: it takes no lock and shares no state, so it adds no lock order')
for name, signature in (
        ('NoteResource',
         'void NoteResource(const D3D12_RESOURCE_DESC* desc, ID3D12Resource* resource, ID3D12Heap* heap)'),
        ('NoteUav', 'void NoteUav(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)')):
    note = body(scan, signature)
    guard(lambda note=note: note.index('ScopedInternalResourceCreation::Active()') < note.index('g_scanMutex') and
          note.index('ScopedInternalResourceCreation::Active()') < note.index('g_scan.examined++'),
          f'{name} leaves a scoped creation before it takes the scan\'s lock or counts it')


def sources(*trees):
    for tree in trees:
        for path in sorted((root / tree).rglob('*')):
            if path.suffix in ('.cpp', '.h') and path.is_file():
                yield path.relative_to(root).as_posix(), path.read_text(encoding='utf-8', errors='replace')


readers = [name for name, text in sources('OptiScaler', 'retoque')
           if 'ScopedInternalResourceCreation::Active()' in text]
guard(lambda: readers == ['retoque/dlssnr/DlssNr_ExposureScan.cpp'],
      f'only the exposure scan reads the scope, so resource tracking and HUDfix see what they saw ({readers})')


def scoped(text, *creations):
    """The scope opens once in `text`, before the first of each creation."""
    return text.count(SCOPE) == 1 and all(text.index(SCOPE) < text.index(creation) for creation in creations)


resources = read('retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp')
for name, signature in (
        ('CreateScratch',
         'ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, '
         'unsigned int height)'),
        ('CreatePreUpscaleScratch',
         'ID3D12Resource* CreatePreUpscaleScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,')):
    guard(lambda signature=signature: scoped(body(resources, signature), 'CreateCommittedResource('),
          f'{name} creates its surface inside the scope, whichever route asks for it')

# The pass: one scope for everything Dispatch creates after its admission checks -- the meter, the pulse, cadence,
# the model's CreateFeature and EvaluateFeature (NGX allocates and makes its views through the same device), and
# the views the pass's own dispatches make every frame, which reach NoteUav like the game's.
core = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
units = [core] + [path.read_text(encoding='utf-8-sig')
                  for path in sorted((root / 'retoque/shaders/dlssnr').glob('DlssNr_Dx12_*.cpp'))]
dispatch_at = core.index('bool DlssNr_Dx12::Dispatch(')
dispatch = core[dispatch_at:core.index('\nnamespace DlssNr\n{', dispatch_at)]
creates = ('CreateScratch(', 'CreateCommittedResource(', 'new DlssNr_', 'DispatchPass(', 'g_nr.create(',
           'g_nr.evaluate(', 'EnsureCapabilityParams(', 'ExposureScan::Tick(')
guard(lambda: dispatch.count('\n    ' + SCOPE + '\n') == 1 and scoped(dispatch, *creates),
      'Dispatch opens the scope once, at function level, before anything it creates')
guard(lambda: dispatch.index('!g_nrDeviceOwner.Accept(_device)') < dispatch.index(SCOPE) <
      dispatch.index('TrackNrRecording('), 'after the admission checks and before the recording is tracked')
inside = dispatch[dispatch.index(SCOPE):] if SCOPE in dispatch else ''
for call in ('g_nr.create(', 'g_nr.evaluate('):
    guard(lambda call=call: sum(unit.count(call) for unit in units) == inside.count(call) >= 1,
          f'every {call[:-1]} call of the D3D12 renderer is inside it')
# Defined in the renderer's other units (or the proxy experiment), called from the core: each call is inside it.
for call in ('g_nr.meter = CreateScratch(', 'new DlssNr_Cadence_Dx12(', 'UpdatePulse(', 'UpdateMotionProbe(',
             'EnlargeEdit(', 'DlssNr::Proxy::Run(', 'ProbeProxyDispatch('):
    guard(lambda call=call: core.count(call) == inside.count(call) >= 1,
          f'{call[:-1]} is reached only from inside it')

zero = read('retoque/dlssnr/DlssNr_ZeroGuides.cpp')
allocate = body(zero, 'bool ZeroGuides::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)')
guard(lambda: scoped(allocate, 'CreateGuide(', 'CreateUnorderedAccessView(') and
      zero.count('CreateGuide(') - 1 == allocate.count('CreateGuide(') == 2 and
      zero.count('CreateUnorderedAccessView(') == allocate.count('CreateUnorderedAccessView(') == 1,
      'the zero guides and their clear views are created inside the scope')

presenter = read('retoque/lsfg/presenter/MfgSwapChain_Dx12.cpp')
creators = [body(presenter, signature) for signature in (
    'bool MfgSwapChainDX12::verifyBackbufferDuplicateResources()',
    'bool MfgSwapChainDX12::verifyInterpolationOutputs(uint32_t set, uint32_t count)',
    'void MfgSwapChainDX12::createTiming()',
    'bool MfgSwapChainDX12::verifyUiDuplicateResource()')]
guard(lambda: all(scoped(creator, 'CreateCommittedResource(') for creator in creators),
      'the MFG presenter creates its replacement buffers, outputs, timing readback and UI copy inside the scope')
guard(lambda: presenter.count('CreateCommittedResource(') == sum(c.count('CreateCommittedResource(') for c in creators)
      and not any(call in presenter for call in ('CreatePlacedResource(', 'CreateReservedResource(')),
      'and creates no resource anywhere else')

for ok, what in guards:
    print(('PASS ' if ok else 'FAIL ') + 'guard: ' + what)
    if not ok:
        failures.append('guard: ' + what)

if failures:
    print(f'FAIL: {len(failures)} of {len(names) + len(guards)}: ' + ', '.join(failures))
    sys.exit(1)
print(f'PASS: {len(names)} cases and {len(guards)} source guards')
