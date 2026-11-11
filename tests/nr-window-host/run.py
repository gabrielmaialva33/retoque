#!/usr/bin/env python3
"""NR on any window, phase 1 (docs/dlssnr/design/nr-anything.md): the RetoqueWindow_* seam in the production DLL is
inert without RETOQUE_WINDOW_HOST=1, keeps its rules inside a session, and leaves every default present route as it
was. Host only: the seam compiled as it is against fakes, plus source contracts. No D3D12, no GPU, no model."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DLSSNR = ROOT / 'retoque/dlssnr'
seam = (DLSSNR / 'DlssNr_WindowHost.cpp').read_text()
header = (DLSSNR / 'DlssNr_WindowHost.h').read_text()
feature = (DLSSNR / 'DlssNrFeature_Dx12.h').read_text()
renderer = renderer_source()


def body(source, signature):
    start = source.index(signature)
    open_brace = source.index('{', start)
    depth = 0
    for i in range(open_brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[open_brace:i + 1]
    raise AssertionError(signature)


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


# --- Source contract: the seam -----------------------------------------------------------------------------------
assert seam.startswith('#include "pch.h"'), 'a translation unit starts with pch.h'
assert 'pch.h' not in header, 'headers never include pch.h'
for export in ('uint32_t RetoqueWindow_Version()', 'int RetoqueWindow_Begin()', 'int RetoqueWindow_Present(',
               'void RetoqueWindow_End()'):
    assert seam.count('extern "C" __declspec(dllexport) ' + export) == 1, export
assert 'RetoqueWindow' not in (ROOT / 'OptiScaler/Source.def').read_text(), 'exports stay outside Source.def'
assert 'inline constexpr char kEnvironment[] = "RETOQUE_WINDOW_HOST";' in header
begin = body(seam, 'int RetoqueWindow_Begin()')
assert begin.index('ProcessAskedForWindowHost()') < begin.index('g_session.store(true'), 'gate before the session'
present = body(seam, 'int RetoqueWindow_Present(')
# The pass is entered as the wrapped swapchain enters it, once, and after every refusal.
assert present.count('DlssNr::RunPresentPass(swapchain, queue, false);') == 1
entry = present.index('DlssNr::RunPresentPass(')
for refusal in ('IsOwner()', 'swapchain == nullptr || queue == nullptr', 'OptiScalerWrapsASwapchain()',
                'g_presenting.compare_exchange_strong'):
    assert present.index(refusal) < entry, refusal
# The seam owns nothing on the GPU and records nothing: no creation, release, submission, wait or shutdown.
code = strip_comments(seam)
for forbidden in ('Create', 'Release(', 'ExecuteCommandLists', 'Signal(', 'Wait(', 'Shutdown(', 'new ', 'delete ',
                  'Config::Instance'):
    assert forbidden not in code, f'the window-host seam must not call {forbidden!r}'
for slot in ('currentSwapchain', 'currentWrappedSwapchain', 'currentFGSwapchain'):
    assert f'state.{slot} != nullptr' in seam, slot

# --- Source contract: the default routes are unchanged -----------------------------------------------------------
# The present pass keeps its signature and its three production entries; the seam is the only new caller.
assert ('void RunPresentPass(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool fgHook, '
        'bool boundary = false);') in feature
callers = {}
for path in list((ROOT / 'OptiScaler').rglob('*.cpp')) + list((ROOT / 'retoque').rglob('*.cpp')):
    text = strip_comments(path.read_text(errors='replace'))
    count = len(re.findall(r'DlssNr::RunPresentPass\(', text))
    if count:
        callers[path.relative_to(ROOT).as_posix()] = count
assert callers == {
    'OptiScaler/wrapped/wrapped_swapchain.cpp': 1,
    'OptiScaler/hooks/FG_Hooks.cpp': 1,
    'retoque/dlssnr/DlssNr_SlBoundaryProbe.cpp': 1,
    'retoque/dlssnr/DlssNr_WindowHost.cpp': 1,
}, callers
# The one accessor the seam added reads a counter and does nothing else.
assert 'unsigned long long PassesRecorded() { return g_passesRecorded.load(); }' in renderer
assert 'unsigned long long PassesRecorded();' in feature
# Nothing in the DLL reaches the exports or reads the variable but the seam itself.
for path in [p for root in ('OptiScaler', 'retoque') for p in (ROOT / root).rglob('*')
             if p.suffix in ('.cpp', '.h', '.hpp', '.def')]:
    rel = path.relative_to(ROOT).as_posix()
    if rel.startswith('retoque/window_host/') or rel in ('retoque/dlssnr/DlssNr_WindowHost.cpp',
                                                          'retoque/dlssnr/DlssNr_WindowHost.h'):
        continue
    text = path.read_text(errors='replace')
    assert 'RetoqueWindow_' not in text and 'RETOQUE_WINDOW_HOST' not in text and \
        'DlssNr_WindowHost.h"' not in text, rel
project = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (ROOT / 'OptiScaler/OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
for item in ('<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_WindowHost.cpp" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_WindowHost.h" />'):
    assert project.count(item) == 1, item
    assert filters.count(item.replace(' />', '>')) == 1, item

# --- Behaviour: the seam compiled as shipped against fakes ---------------------------------------------------------
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'no C++ compiler'
with tempfile.TemporaryDirectory() as scratch:
    work = Path(scratch)
    for name in ('DlssNr_WindowHost.cpp', 'DlssNr_WindowHost.h'):
        shutil.copy(DLSSNR / name, work / name)
    for fake in (HERE / 'fakes').iterdir():
        shutil.copy(fake, work / fake.name)
    exe = work / 'seam'
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-pthread', '-I', str(work), str(work / 'DlssNr_WindowHost.cpp'), str(HERE / 'cases.cpp'), '-o',
                    str(exe)], check=True)
    clean = {k: v for k, v in os.environ.items() if k != 'RETOQUE_WINDOW_HOST'}
    for mode in ('inert', 'session'):
        run = subprocess.run([str(exe), mode], env=clean, capture_output=True, text=True)
        print(run.stdout, end='')
        print(run.stderr, end='')
        if run.returncode:
            raise SystemExit(f'window-host seam {mode} cases failed ({run.returncode})')
print('PASS: window-host seam: inert without the variable, rules inside a session, default present routes unchanged')
