#!/usr/bin/env python3
"""The offline replay seam (docs/dlssnr/design/offline-replay-seam.md): logical clock, adapter state machine, the
outcome notes, and where the renderer reads the clock."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DLSSNR = ROOT / 'retoque/dlssnr'
renderer = renderer_source()
adapter = (DLSSNR / 'DlssNr_Replay.cpp').read_text()


def body(source, signature):
    start = source.index(signature)
    open_brace = source.index('{', start)
    depth = 0
    for i in range(open_brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[open_brace:i + 1]
    raise AssertionError(signature)


# --- Source contract ------------------------------------------------------------------------------------------
dispatch = body(renderer, 'bool DlssNr_Dx12::Dispatch(')
assert 'const auto now = DlssNr::ReplayClock::Now();' in dispatch                # cadence, settle gates, chain
# Creation spacing reads the clock at each call, as it did before the facade: outside a replay that is a fresh real
# read, not the earlier Dispatch-entry instant.
assert dispatch.count('g_chainSchedule.Create(observedFrame, DlssNr::ReplayClock::Now())') == 2
assert 'g_chainSchedule.Create(observedFrame)' not in renderer and 'Create(observedFrame, now)' not in renderer
# The enlargement model's frame time is an input to a model: recorded time under replay.
enlarge = renderer[renderer.index('DlssNr::PrivateSr::Frame sr;') - 600:renderer.index('DlssNr::PrivateSr::Frame sr;')]
assert 'const auto now = DlssNr::ReplayClock::Now();' in enlarge
for gate in ('if (now - g_nr.uiMaskLast > std::chrono::milliseconds(250))',
             'const bool lapsed = now - g_nr.stabilizerLast > std::chrono::milliseconds(250);'):
    head = dispatch[:dispatch.index(gate)]
    assert head.rstrip().endswith('const auto now = DlssNr::ReplayClock::Now();'), gate
pause = body(renderer, 'bool PausedWithoutFrameGen(const Config& cfg)')
assert 'Clock::now()' not in pause and pause.count('DlssNr::ReplayClock::Now()') == 2
# The outcome: every skip reason, before the once-only log filter; the dispatch summary on the full path.
# The creation frame records nothing by design and logs no skip; replay still says why.
assert 'DlssNr::Replay::NoteSkip("the model was created on this frame; it first runs on the next");' in dispatch
skip = body(renderer, 'void ReportSkipOnce(const char* reason)')
assert skip.index('DlssNr::Replay::NoteSkip(reason);') < skip.index('for (const char* logged : seen)')
tail = dispatch[dispatch.index('timing.FinishOnScopeExit(carried ? 0u : chain.completed, timingComplete);'):]
assert tail.index('DlssNr::Replay::NoteDispatch(wrote, carried, cadence.cadence, passSnapshot.Count,') < \
    tail.index('return wrote;')
# The adapter: unmangled exports outside Source.def, gated by the process environment, entering production.
for export in ('uint32_t RetoqueReplay_Version()', 'int RetoqueReplay_Begin()', 'int RetoqueReplay_Evaluate(',
               'void RetoqueReplay_End()'):
    assert 'extern "C" __declspec(dllexport) ' + export in adapter, export
assert 'RetoqueReplay' not in (ROOT / 'OptiScaler/Source.def').read_text()
assert 'DlssNr::EvaluateAfterUpscale(cmdList, params, queue, false, frame->rayReconstruction != 0);' in adapter
project = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text()
filters = (ROOT / 'OptiScaler/OptiScaler.vcxproj.filters').read_text()
for item in ('<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_Replay.cpp" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_Replay.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_ReplayClock.h" />'):
    assert project.count(item) == 1, item
    assert filters.count(item.replace(' />', '>')) == 1, item

# --- Behaviour: the clock and the adapter, compiled against fakes -----------------------------------------------
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'no C++ compiler'
with tempfile.TemporaryDirectory() as scratch:
    work = Path(scratch)
    for name in ('DlssNr_Replay.cpp', 'DlssNr_Replay.h', 'DlssNr_ReplayClock.h'):
        shutil.copy(DLSSNR / name, work / name)
    for fake in (HERE / 'fakes').iterdir():
        shutil.copy(fake, work / fake.name)
    exe = work / 'seam'
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(work), str(work / 'DlssNr_Replay.cpp'), str(HERE / 'cases.cpp'), '-o', str(exe)],
                   check=True)
    env = {k: v for k, v in os.environ.items() if k != 'RETOQUE_REPLAY'}
    run = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
    print(run.stdout, end='')
    print(run.stderr, end='')
    if run.returncode:
        raise SystemExit(f'seam cases failed ({run.returncode})')
print('PASS: replay clock and adapter; decision clocks, outcome notes and exports wired as designed')

# Harness scheduling metadata and comparison policy, including negative controls.
subprocess.run([sys.executable, str(HERE / 'inflight.py')], check=True)
