#!/usr/bin/env python3
"""Pure advisor policy, ASan/UBSan; no GPU/game qualification."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='nr-advisor-') as temp:
    binary = str(Path(temp) / 'cases')
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), '-I', str(here),
                    str(here / 'cases.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
print('PASS: candidate order, timing identity, statistics, recommendation, restoration and aborts')

source = (root / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()
menu = source[source.index('static void RenderAdvisor('):source.index('static bool DeferredSlider(')]
gpu = (root / 'retoque/dlssnr/DlssNr_GpuTiming.h').read_text()
gpu = gpu[gpu.index('struct Metadata'):gpu.index('// Collection never waits')]
with tempfile.TemporaryDirectory(prefix='nr-advisor-menu-') as temp:
    generated = Path(temp) / 'menu.cpp'
    generated.write_text((here / 'menu_fakes.h').read_text() + '\nnamespace DlssNr::GpuTiming {\n' +
        gpu + '\nSnapshot snapshot; Snapshot GetSnapshot() {return snapshot;}\n}\nnamespace DlssNr {\n' +
        menu + '\n}\n' + (here / 'menu_cases.h').read_text())
    binary = str(Path(temp) / 'menu')
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), str(generated), '-o', binary], check=True)
    subprocess.run([binary], check=True)
print('PASS: actual advisor menu eligibility, start/cancel, copy, stale-context refusal and explicit apply')
with tempfile.TemporaryDirectory(prefix='nr-advisor-adapter-') as temp:
    binary = str(Path(temp) / 'adapter')
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), '-I', str(here),
                    str(here / 'adapter.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
print('PASS: production adapter clears overrides on all heartbeat/context cancellation paths')
# Small source-boundary guards supplement executable policy and menu tests; they are not D3D12 compilation.
renderer = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
collector = (root / 'retoque/dlssnr/DlssNr_GpuTiming.cpp').read_text()
runtime = (root / 'retoque/dlssnr/DlssNr_AdvisorRuntime.h').read_text()
assert 'Config' not in runtime.replace('// In-process adapter. No Config mutation, GPU object ownership, or calls into the renderer/timing collector.', '')
assert 'AdvisorRuntime::Confirm' in collector
assert collector.index('const auto certificate = Submission::TimingCertificate(slot.usage);', collector.index('void PollLocked')) < collector.index('AdvisorRuntime::Confirm')
assert 'advisor.settings ? advisor.settings->scale' in renderer
assert 'advisor.settings ? advisor.settings->cadence' in renderer
assert 'metadata.advisorToken = advisor.token' in renderer
assert 'cadence.cadence == advisor.settings->cadence' in renderer
assert 'AdvisorRuntime::Heartbeat' in (root / 'OptiScaler/menu/menu_common.cpp').read_text()
for file in ('DlssNr_PresentHost.cpp', 'DlssNr_SwapchainColour.cpp'):
    assert 'AdvisorRuntime::Cancel(Advisor::Abort::Resized)' in (root / 'retoque/dlssnr' / file).read_text()
print('PASS: override, provenance, confirmed-collector, close and resize source seams (static only)')
with tempfile.TemporaryDirectory(prefix='nr-advisor-confirmed-') as temp:
    fixture = (root / 'tests/nr-gpu-timing/runtime.cpp').read_text()
    assert fixture.count('\nint main(') == 1
    (Path(temp) / 'timing_fixture.h').write_text(fixture.split('\nint main(', 1)[0])
    binary = str(Path(temp) / 'confirmed')
    subprocess.run(['clang++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'tests/nr-gpu-timing/stubs'),
                    '-I', str(root), '-I', str(root / 'retoque'), '-I', str(here),
                    '-I', temp, '-I', str(root / 'tests/nr-gpu-timing'),
                    str(here / 'timing_cases.cpp'), str(root / 'retoque/dlssnr/DlssNr_GpuTiming.cpp'),
                    '-o', binary], check=True)
    subprocess.run([binary], check=True)
print('PASS: real timing collector feeds advisor only after sealed submission and completed fence; old run ignored')
