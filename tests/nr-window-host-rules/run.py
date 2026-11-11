#!/usr/bin/env python3
"""NR on any window, phase 2 (docs/dlssnr/design/nr-anything.md): the capture host's pure rules -- client crop,
candidate windows, source identity, pause and stop, the adapter rule, frame pacing, source age, the command line,
stdout events and stdin commands -- compiled as they ship (g++, sanitizers, no Windows types). Plus a source contract
on the Windows units: every translation unit starts with pch.h, the pure headers include no Windows header, and the
host loads OptiScaler.dll only after its D3D objects exist."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
HOST = ROOT / 'retoque/window_host'
PURE = ('Geometry.h', 'Policy.h', 'Pacing.h', 'Protocol.h')

# --- Source contract ---------------------------------------------------------------------------------------------
for name in PURE:
    text = (HOST / name).read_text()
    for forbidden in ('<windows.h>', '<d3d', '<dxgi', 'winrt', 'pch.h', 'HWND', 'HRESULT'):
        assert forbidden not in text, f'{name} must stay Windows-free: {forbidden}'
units = sorted(HOST.glob('*.cpp'))
assert units, 'the host has translation units'
for unit in units:
    assert unit.read_text().startswith('#include "pch.h"'), f'{unit.name} starts with pch.h'
for header in HOST.glob('*.h'):
    if header.name != 'pch.h':
        assert '#include "pch.h"' not in header.read_text(), f'{header.name}: headers never include pch.h'
main = (HOST / 'Main.cpp').read_text()
# The seam's hooks never meet the host's own DXGI and D3D12 objects: they are all created before the DLL is loaded,
# and the host never creates a factory or a swapchain afterwards (a resize is ResizeBuffers).
run = main[main.index('int RunCapture('):]
load = run.index('seam.Load(')
for creation in ('devices.Create(', 'output.Create('):
    assert run.index(creation) < load, f'{creation} comes before the DLL is loaded'
for later in ('CreateDXGIFactory', 'CreateSwapChainFor', 'D3D12CreateDevice', 'D3D11CreateDevice'):
    assert later not in run[load:], f'{later} after the DLL is loaded'
seam = (HOST / 'NrSeam.cpp').read_text()
assert 'SetEnvironmentVariableW(L"RETOQUE_WINDOW_HOST", L"1")' in seam
assert seam.index('SetEnvironmentVariableW(') < seam.index('LoadLibraryExW(')
assert 'FreeLibrary' not in seam, 'the DLL stays loaded until the process exits'
for export in ('RetoqueWindow_Version', 'RetoqueWindow_Begin', 'RetoqueWindow_Present', 'RetoqueWindow_End'):
    assert f'"{export}"' in seam, export
# The DLL-side outcome and the host's copy of it stay one layout.
dll = (ROOT / 'retoque/dlssnr/DlssNr_WindowHost.h').read_text()
fields = re.findall(r'^\s+(u?int\d+_t|char) (\w+)(\[\d+\])? = ', dll[dll.index('struct Outcome'):], re.M)
assert [f[1] for f in fields] == ['version', 'entered', 'recorded', 'running', 'passesRecorded', 'status'], fields
assert '#include "../dlssnr/DlssNr_WindowHost.h"' in seam or '#include "../dlssnr/DlssNr_WindowHost.h"' in \
    (HOST / 'NrSeam.h').read_text(), 'the host uses the DLL header itself, not a copy'
# Attribution for the parts re-derived from NeuRotic/Magpie.
for name in ('Geometry.h', 'Capture.cpp'):
    text = (HOST / name).read_text()
    assert 'NeuRotic beae4eea' in text and 'Magpie 69d6105d' in text and 'GPL-3.0' in text, name
assert (ROOT / 'Licenses/Magpie-LICENSE.txt').is_file(), 'Magpie licence kept with its adaptation'

# --- Behaviour ---------------------------------------------------------------------------------------------------
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'no C++ compiler'
with tempfile.TemporaryDirectory() as scratch:
    exe = Path(scratch) / 'rules'
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(HOST), str(HERE / 'cases.cpp'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if result.returncode:
        raise SystemExit(f'window host rules failed ({result.returncode})')
print('PASS: window host source contract: pch.h first, pure headers Windows-free, DLL loaded after the D3D objects, '
      'one outcome layout, attribution kept')
