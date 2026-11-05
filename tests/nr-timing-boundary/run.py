#!/usr/bin/env python3
"""Exercise production timing metadata/UI with host fakes, and guard marker placement."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
out = here / 'artifacts'
out.mkdir(exist_ok=True)
renderer = renderer_source()
menu = (root / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()

def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]

# TimingContract and TimingMetadata, now in DlssNr_Dx12_Diagnostics.cpp (split step 6), up to the next helper there.
metadata = between(renderer, 'std::string TimingContract(', '// Cleared once per run, so a session')
controls = between(menu, 'static void RenderGpuTimingReadout(', '// A slider that only writes')
dispatch = between(renderer, 'bool DlssNr_Dx12::Dispatch(', '\nnamespace DlssNr\n{')
assert 'GpuTime_Dx12' not in renderer
assert 'DLSS-NR cost:' not in renderer
assert 'std::optional<double> LastGpuTime() { return std::nullopt; }' in renderer
assert dispatch.index('GpuTiming::Evaluation timing(') < dispatch.index('Barrier(cmdList, target, outputArrival,')
assert dispatch.index('timing.SetMetadata(') < dispatch.index('timing.ModelBegin()')
assert dispatch.count('timing.ModelBegin();') == dispatch.count('timing.ModelEnd();') == 2
first_begin = dispatch.index('timing.ModelBegin();')
first_end = dispatch.index('timing.ModelEnd();')
assert dispatch[first_begin:first_end].count('g_nr.evaluate(') == 1  # shared single/chain first pass
second_begin = dispatch.index('timing.ModelBegin();', first_end)
second_end = dispatch.index('timing.ModelEnd();', second_begin)
assert dispatch[second_begin:second_end].count('g_nr.evaluate(') == 1  # repeated per extra pass
assert dispatch.count('g_nr.evaluate(') == 2  # no unmeasured forwarder calls outside the markers
assert second_begin > dispatch.index('for (unsigned i = 1; i < passSnapshot.Count; ++i)')
assert dispatch.index('timing.FinishOnScopeExit(') > dispatch.index('colorRead.Restore();')
assert 'wrote && result == 1 && chain.completed == passSnapshot.Count' in dispatch
# Model cadence (docs/dlssnr/design/model-cadence.md): a carried frame calls no model, so it records no model markers,
# is never rejected as a partial chain, finishes with zero passes, and holds a pending reset.
carry_call = dispatch.index('g_nr.cadence->Carry(')
assert carry_call < first_begin, 'the carry must be the alternative to the model call, not after it'
assert dispatch[carry_call:first_begin].count('g_nr.evaluate(') == 0
assert 'if (!carried && chain.completed != passSnapshot.Count)\n        timing.Reject("partial-chain");' in dispatch
assert 'timing.FinishOnScopeExit(carried ? 0u : chain.completed, timingComplete);' in dispatch
assert 'if (!carried)\n        g_nr.reset = false;' in dispatch
assert dispatch.count('g_nr.reset = false;') == 2  # the proxy path and the model frame, never a carried one
header = (root / 'retoque/dlssnr/DlssNr_GpuTiming.h').read_text()
records = between(header, 'struct Metadata\n', '// Collection never waits')
unit = (here / 'fakes.h').read_text()
unit += '\nnamespace DlssNr::GpuTiming {\n' + records + '\n}\n'
unit += 'DlssNr::GpuTiming::Snapshot published; int snapshotReads=0;\n'
unit += 'namespace DlssNr::GpuTiming { void SetEnabled(bool) {} Snapshot GetSnapshot() { ++snapshotReads; return published; } }\n'
unit += metadata
unit += '\nnamespace DlssNr {\n' + controls + '\n}\n' + (here / 'cases.h').read_text()
(out / 'boundary.cpp').write_text(unit)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root), str(out / 'boundary.cpp'), '-o', str(out / 'boundary')], check=True)
subprocess.run([str(out / 'boundary')], check=True)


# D3D12 Present route and guide-capture regions (docs/dlssnr/design/gpu-timing.md). No host fixture executes
# RunPresentPass or CaptureGuides, so these are source-order guards, each proven by a mutant it must reject below.
# They keep EndQuery/ResolveQueryData on a still-open list and the never-enabled path free of the settings mutex.
import re  # noqa: E402


def present_region_guards(source):
    present = between(source, 'void RunPresentPass(', '// ReportSpatialContract, GatherGuideFrame')
    capture = between(source, 'static bool CaptureGuides(', 'void CaptureTemporal(')
    assert 'const bool routeTimingArmed = cfg.DlssNrGpuTimingRequested();' in present
    assert present.count('GetDlssNrGpuTimingSettings()') == 2  # the gated route read and the existing HDR one
    assert 'if (routeTimingArmed)\n        routeTimingSettings = cfg.GetDlssNrGpuTimingSettings();' in present
    first_begin = present.index('beginPresentTiming();')
    assert present.count('presentTiming->Finish(true);') == 1
    finish = present.index('presentTiming->Finish(true);')
    assert finish < present.index('SubmitPresentList(queue, slot)'), 'Finish must precede Close/Execute'
    drops = [m.start() for m in re.finditer(r'DropPresentList\(slot\);', present) if m.start() > first_begin]
    guarded = [m.end() for m in re.finditer(r'presentTiming\.reset\(\);[^\n]*\n\s*DropPresentList\(slot\);', present)]
    assert drops and len(drops) == len(guarded), 'every drop after the region began resolves it first'
    assert all(any(abs(g - d) < 40 for g in guarded) for d in drops)
    assert capture.count('GetDlssNrGpuTimingSettings()') == 1
    gate = capture.index('if (Config::Instance()->DlssNrGpuTimingRequested())')
    assert capture.index('ParkNrClone(clone.motion, savedWriteUsage, clone.tracked);') < gate
    assert gate < capture.index('GetDlssNrGpuTimingSettings()') < capture.index('captureTiming.emplace(')
    assert capture.index('g_presentGuideRing.CommitRecord(') < \
        capture.index('NoteGuideCapture(DlssNr::PresentGuideIdentity::ProducerReason::Recorded,') < \
        capture.index('captureTiming->Finish(true);')
    # CPU stream (DlssNr_PresentCpuTiming.h): gated like the regions, no new clock reads, submitted presents only.
    assert 'const bool cpuTimed = cfg.DlssNrGpuTimingRequested() && !cfg.DlssNrUseProxy.value_or_default();' in present
    assert present.count('steady_clock::now()') == 2  # PresentSync's own wait log, which predates the CPU stream
    assert 'GetPresentCommandList(device, slot, cpuTimed ? &cpuSample[DlssNr::PresentCpu::Wait::Allocator] : nullptr)' \
        in present
    assert re.search(r'DlssNr::PresentCpu::Timed\(cpuTimed \? &cpuSample\[DlssNr::PresentCpu::Wait::SynthIdle\] : nullptr,'
                     r'\s*\[\] \{ return WaitPresentListsIdle\(2000\); \}\)', present)
    assert present.count('WaitPresentListsIdle(') == 1
    assert present.index('cpuSample.recordMs =') < present.index('SubmitPresentList(queue, slot)') < \
        present.index('cpuSample.submitMs =')
    assert 'if (cpuTimed && executed && g_presentCpu.Add(' in present
    assert present.index('WaitForSingleObject(g_presentList.fenceEvent, 1000)') < present.index('g_presentCpu.Add(')
    # The route's split: marks immediately around the one Dispatch call, the region lent by RunPresentPass.
    evaluate = between(source, 'bool EvaluateAtPresent(', 'ScopedPreUpscale::ScopedPreUpscale(')
    assert evaluate.count('g_compose->Dispatch(') == 1
    assert re.search(r'if \(route != nullptr\)\n\s*route->BeforeDispatch\(\);\n\s*const bool recorded = '
                     r'g_compose->Dispatch\([^;]*;\n\s*if \(route != nullptr\)\n\s*route->AfterDispatch\(\);', evaluate)
    assert evaluate.count('route->BeforeDispatch();') == evaluate.count('route->AfterDispatch();') == 1
    assert present.count('EvaluateAtPresent(') == 1 and 'presentTiming ? &*presentTiming : nullptr);' in present


present_region_guards(renderer)
mutants = {
    'finish after submit': lambda s: s.replace('presentTiming->Finish(true);', '', 1).replace(
        'SubmitPresentList(queue, slot);', 'SubmitPresentList(queue, slot);\n    presentTiming->Finish(true);', 1),
    'drop without reset': lambda s: s.replace('presentTiming.reset();', '', 1),
    'reset after drop': lambda s: re.sub(r'(presentTiming\.reset\(\);[^\n]*\n)(\s*)(DropPresentList\(slot\);)',
                                         r'\2\3\n\2presentTiming.reset();', s, count=1),
    'ungated route settings': lambda s: s.replace('if (routeTimingArmed)\n        routeTimingSettings =',
                                                  'routeTimingSettings =', 1),
    'ungated capture settings': lambda s: s.replace('if (Config::Instance()->DlssNrGpuTimingRequested())', 'if (true)', 1),
    'capture note after finish': lambda s: s.replace(
        'NoteGuideCapture(DlssNr::PresentGuideIdentity::ProducerReason::Recorded, acquire, ResidentGuideBytes(),\n                     ParkedGuideBytes());\n',
        '', 1).replace(
        '    g_temporalValid = true;',
        '    NoteGuideCapture(DlssNr::PresentGuideIdentity::ProducerReason::Recorded, acquire, ResidentGuideBytes(),\n                     ParkedGuideBytes());\n    g_temporalValid = true;', 1),
    'dispatch end unmarked': lambda s: s.replace('route->AfterDispatch();', ';', 1),
    'dispatch begin after the call': lambda s: s.replace(
        '    if (route != nullptr)\n        route->BeforeDispatch();\n', '', 1).replace(
        'route->AfterDispatch();', 'route->BeforeDispatch();\n        route->AfterDispatch();', 1),
    'route not lent': lambda s: s.replace('presentTiming ? &*presentTiming : nullptr);', 'nullptr);', 1),
    'cpu stream ungated': lambda s: s.replace(
        'cpuTimed ? &cpuSample[DlssNr::PresentCpu::Wait::Allocator] : nullptr',
        '&cpuSample[DlssNr::PresentCpu::Wait::Allocator]', 1),
    'unguarded clock read': lambda s: s.replace('    cpuSample.recordMs =',
                                                '    (void) std::chrono::steady_clock::now();\n    cpuSample.recordMs =', 1),
    'dropped present counted': lambda s: s.replace('if (cpuTimed && executed && g_presentCpu.Add(',
                                                   'if (cpuTimed && g_presentCpu.Add(', 1),
    'capture finish before commit': lambda s: s.replace('g_presentGuideRing.CommitRecord(',
                                                        'captureTiming->Finish(true);\n    g_presentGuideRing.CommitRecord(', 1),
}
for name, mutate in mutants.items():
    mutated = mutate(renderer)
    assert mutated != renderer, f'mutant did not apply: {name}'
    try:
        present_region_guards(mutated)
    except (AssertionError, ValueError):  # a missing anchor rejects the mutant too
        continue
    raise SystemExit(f'FAIL: placement guard accepted the mutant: {name}')
print(f"PASS Present/capture region placement guards; {len(mutants)} mutants rejected")
print("PASS production timing placement guards")
