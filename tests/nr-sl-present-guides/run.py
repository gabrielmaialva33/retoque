#!/usr/bin/env python3
"""b1 host policy and source-boundary contracts; no Windows compiler or GPU."""
from pathlib import Path
import subprocess
import tempfile
import sys
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/_support'))
from present_contract import present_body as function
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-pthread', '-I' + str(ROOT / 'retoque')]
with tempfile.TemporaryDirectory(prefix='nr-sl-present-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(FLAGS + [str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

source = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp').read_text()
renderer = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
hooks = (ROOT / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text()
present = function(renderer, 'RunPresentPass')
assert present.index('ObserveSlPresentBoundary(') < present.index('if (g_nr.failed') < present.index('kStartupWarmupFlips')
assert 'const bool slBoundaryObserved = boundary &&' in present
assert 'AskObservedPresentGuides(slObservedPresent' in present
assert 'SlPresentSlotEligible(token, boundary)' in present
assert present.index('SlPresentSlotEligible(token, boundary)') < present.index('Submission::SubmittedOnlyOn(writeUsage, queue)')
for name in ('hkslSetTagNoteOnly', 'hkslSetTagForFrameNoteOnly', 'hkslSetTag', 'hkslSetTagForFrame'):
    body = function(hooks, 'StreamlineHooks::' + name)
    assert body.index('if (DlssNr::SlPresent::Enabled())') < body.index('SlPresent::Tags(') < body.index('return o_slSetTag')
    assert 'renderApi == sl::RenderAPI::eD3D12' in body
body = function(source, 'Tags')
assert 'tag->lifecycle != sl::ResourceLifecycle::eOnlyValidNow' in body
assert body.index('CopyRecording(list)') < body.index('CaptureGuides(list, source, frame)')
assert 'static_cast<float>(md.Width)' in body and 'static_cast<float>(md.Height)' in body
assert 'kBufferTypeHUDLessColor' not in body  # HUD-less never becomes colour, guide or model input
capture = function(source, 'CaptureGuides')
assert capture.index('slGuides.frames.Capture(') < capture.index('AcquireForRecord(')
assert capture.index('Submission::Track(cmdList, clone.captureWriteUsage)') < capture.index('CopyTextureRegion(')
assert 'SlUpscalerCaptured(g_guideIdentity.tracker.Epoch())' in capture
assert capture.index('SlSourceRetirementCapacity(') < capture.index('Submission::Track(cmdList, clone.captureWriteUsage)')
assert capture.index('Submission::Track(cmdList, clone.captureWriteUsage)') < capture.index('retainedDepth->AddRef()')
assert capture.index('ParkNrClone(retainedMotion, clone.captureWriteUsage, true)') < capture.index('CopyTextureRegion(')
eligible = function(source, 'SlPresentSlotEligible')
assert '!SlUpscalerCaptured(stamp.presentEpoch)' in eligible and 'slGuides.frames.Matches(' in eligible
assert 'stamp.presentEpoch + 1 == slGuides.boundaryEpoch' in eligible
for path, fragment in (
    ('OptiScaler/Config.h', 'DlssNrPresentSlGuides { false }'),
    ('OptiScaler/Config.cpp', 'DlssNrPresentSlGuides.set_from_config(readBool("DlssNr", "PresentSlGuides"))'),
    ('OptiScaler/Config.cpp', 'GetBoolValue(Instance()->DlssNrPresentSlGuides.value_for_config())'),
    ('OptiScaler.ini', 'PresentSlGuides=false'),
):
    assert fragment in (ROOT / path).read_text()
assert not any('PresentSlGuides' in p.read_text() for p in (ROOT / 'OptiScaler/menu').glob('*.cpp'))
print('PASS: production boundary, tag routing, tracking-before-copy, priority, config and no HUD-less use')
# Execute the actual adapter functions, with the vendored portable Streamline ABI and D3D12 host fakes.
state_start = source.index('struct SlGuideState\n')
state_end = source.index('thread_local SlPresent::Marker slMarker;', state_start) + len('thread_local SlPresent::Marker slMarker;')
capture_start = source.index('struct GuideCapture\n')
capture_end = source.index('\n};', capture_start) + 3
adapter = (HERE / 'adapter_fakes.h').read_text() + '\n' + source[capture_start:capture_end] + '\n' + source[state_start:state_end]
adapter += '\n' + function(source, 'SlUpscalerCaptured') + '\n'
adapter += '\n' + function(source, 'SlSourceRetirementCapacity') + '\n'
adapter += '''
int copies = 0, writeToken, otherWrite;
DlssNrFrameInfo lastFrame;
bool CaptureGuides(ID3D12GraphicsCommandList*, const GuideCapture& source, const DlssNrFrameInfo& frame)
{
    assert(source.output.streamline);
    if (!slGuides.frames.Capture(source.output.slToken, source.output.slRevision, g_guideIdentity.tracker.Epoch())) return false;
    ++copies; lastFrame = frame;
    auto stamp = source.output; stamp.device = slGuides.device;
    stamp.renderWidth = frame.RenderSubrectWidth; stamp.renderHeight = frame.RenderSubrectHeight;
    g_guideIdentity.tracker.StampCapture(0, stamp, 100);
    g_presentGuideRing.CommitRecord(0, frame.RenderSubrectWidth, frame.RenderSubrectHeight, true, 100, &writeToken, nullptr);
    return true;
}
namespace DlssNr::SlPresent {
'''
for name in ('Enabled', 'MarkerStart', 'MarkerEnd', 'Constants', 'Tags'):
    adapter += function(source, name) + '\n'
adapter += '}\nnamespace DlssNr::Dx12Internal {\n'
for name in ('ObserveSlPresentBoundary', 'SlPresentSlotEligible', 'AskObservedPresentGuides'):
    adapter += function(source, name) + '\n'
adapter += '}\n' + (HERE / 'adapter_cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='nr-sl-present-adapter-') as directory:
    cpp = Path(directory) / 'adapter.cpp'; binary = Path(directory) / 'adapter'
    cpp.write_text(adapter)
    subprocess.run(FLAGS + ['-I' + str(ROOT / 'external/streamline'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
recording = (ROOT / 'retoque/dlssnr/DlssNr_SlPresentRecording.cpp').read_text()
# Execute the production detours and installer with fake private-data/Detours APIs, including transaction failure.
recording = recording[recording.index('namespace DlssNr::SlPresent'):]
with tempfile.TemporaryDirectory(prefix='nr-sl-present-recording-') as directory:
    cpp = Path(directory) / 'recording.cpp'; binary = Path(directory) / 'recording'
    cpp.write_text((HERE / 'recording_fakes.h').read_text() + recording + (HERE / 'recording_cases.h').read_text())
    subprocess.run(FLAGS + [str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
# Refusal mutations must fail executable policy tests (a compile error is not a detected mutant).
policy = (ROOT / 'retoque/dlssnr/DlssNr_SlPresentPolicy.h').read_text()
mutants = {
    'copy inside pass': ('return open && !inside && !tainted;', 'return open && !tainted;'),
    'unknown list accepted': ('bool open = false, inside = false, tainted = true;', 'bool open = true, inside = false, tainted = false;'),
    'marker reused': ('        End();\n        return result;', '        return result;'),
    'incomplete pair accepted': ('e.tags || !complete || !e.constants', 'e.tags || !e.constants'),
    'missing capture epoch accepted': ('if (!epoch || !revision', 'if (!revision'),
    'missing tag accepted': ('!e.constants || !e.tags ||', '!e.constants ||'),
    'old frame accepted': ('e.captureEpoch + 1 == epoch', 'e.captureEpoch < epoch'),
    'unknown HUD guard accepted': ('return sameFrameGuard && pixels > 0', 'return pixels > 0'),
}
with tempfile.TemporaryDirectory(prefix='nr-sl-present-mutants-') as directory:
    out = Path(directory); include = out / 'dlssnr'; include.mkdir()
    for name, (old, new) in mutants.items():
        assert policy.count(old) == 1, (name, old)
        (include / 'DlssNr_SlPresentPolicy.h').write_text(policy.replace(old, new, 1))
        binary = out / 'mutant'
        # Unused parameters introduced by a mutation are immaterial to its behavioral rejection.
        flags = ['g++', '-std=c++20', '-O0', '-I' + str(out), '-I' + str(ROOT / 'retoque'),
                 '-I' + str(ROOT / 'retoque/dlssnr')]
        subprocess.run(flags + [str(HERE / 'cases.cpp'), '-o', str(binary)], check=True, capture_output=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert result.returncode != 0, f'mutant survived: {name}'
print(f'PASS: {len(mutants)} refusal mutants rejected by executable cases')
with tempfile.TemporaryDirectory(prefix='nr-sl-present-marker-') as directory:
    cpp = Path(directory) / 'marker.cpp'; binary = Path(directory) / 'marker'
    cpp.write_text((HERE / 'marker_fakes.h').read_text() + function(hooks, 'StreamlineHooks::hkslPCLSetMarker') +
                   (HERE / 'marker_cases.h').read_text())
    subprocess.run(FLAGS + [str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
