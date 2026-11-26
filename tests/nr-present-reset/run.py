#!/usr/bin/env python3
"""Pending camera resets, independent of guide capture and confirmed only on submission. Host only."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
with tempfile.TemporaryDirectory(prefix='nr-present-reset-') as tmp:
    binary = Path(tmp) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    '-I', str(root / 'retoque'), str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Break the pending-state rules independently: each mutant must be rejected by the behavioral cases.
source = (root / 'retoque/dlssnr/DlssNr_PresentReset.h').read_text()
mutants = {
    'disabled records a cut': ('if (enabled && reset)', 'if (reset)'),
    'false records a cut': ('if (enabled && reset)', 'if (enabled)'),
    'policy cause leaks': ('frame.ResetIsPolicy = false;', 'frame.ResetIsPolicy = true;'),
    'scene-cut cause leaks': ('frame.ResetIsSceneCut = false;', 'frame.ResetIsSceneCut = true;'),
    'skipped model consumes': ('ticket == 0 || !recorded || !submitted', 'ticket == 0 || !submitted'),
    'abandoned list consumes': ('ticket == 0 || !recorded || !submitted', 'ticket == 0 || !recorded'),
    'newer racing cut consumed': ('compare_exchange_weak(consumed, ticket,', 'compare_exchange_weak(consumed, _issued.load(),'),
}
for name, (old, new) in mutants.items():
    assert source.count(old) == 1, name
    with tempfile.TemporaryDirectory(prefix='nr-present-reset-mutant-') as tmp:
        directory = Path(tmp)
        (directory / 'dlssnr').mkdir()
        (directory / 'dlssnr/DlssNr_PresentReset.h').write_text(source.replace(old, new))
        binary = directory / 'cases'
        subprocess.run(['g++', '-std=c++20', '-O1', '-pthread', '-I', str(directory),
                        str(here / 'cases.cpp'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode != 0, f'mutant survived: {name}'
print(f'present reset: {len(mutants)} behavioral mutants rejected')

# Compile the actual two hook functions against narrow fakes: forwarding/FG isolation and Boolean semantics.
def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    level = 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

hooks = (root / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='nr-present-reset-hooks-') as tmp:
    cpp = Path(tmp) / 'hooks.cpp'
    cpp.write_text((here / 'hooks.h').read_text() + '\n' +
                   function(hooks, 'sl::Result StreamlineHooks::hkslSetConstants(') + '\n' +
                   function(hooks, 'bool StreamlineHooks::hkslSetConstants_sl1(') + '\n' +
                   (here / 'hook_cases.h').read_text())
    binary = Path(tmp) / 'hooks'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread',
                    '-I', str(root / 'retoque'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Streamline 1/2 production hook bodies: forwarding, NR-off, invalid/false, FG input isolation passed')

renderer = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
bridge = (root / 'retoque/dlssnr/DlssNr_PresentHost.cpp').read_text()
guides = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp').read_text()
probe = (root / 'retoque/dlssnr/DlssNr_SlBoundaryProbe.cpp').read_text()
present = function(renderer, 'void RunPresentPass(')
assert present.index('PresentReset::Pending::Apply(gameResetTicket, frame);') < present.index('recorded = EvaluateAtPresent(')
assert present.index('SubmitPresentList(queue, slot)') < present.index('PresentReset::Instance().Confirm(gameResetTicket, recorded, executed);')
assert present.count('PresentReset::Instance().Confirm(') == 1
assert 'frame = clone.frame;\n            // The cut was latched before capture admission.' in present
assert present.index('frame.Reset = false;', present.index('frame = clone.frame;')) < present.index('PresentReset::Pending::Apply(')
upscale = function(renderer, 'void EvaluateAfterUpscale(')
assert upscale.index('if (params != nullptr && (dlssNrStage != 1 || preUpscaleDeclined) && captureForPresent)') < upscale.index('params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success') < upscale.index('PresentReset::Instance().Observe(true, gameReset != 0);') < upscale.index('if (target == nullptr || depth == nullptr || motion == nullptr)')
assert upscale.index('PictureRole(params)') < upscale.index('PresentReset::Instance().Observe(') < upscale.index('if (cmdList == nullptr || params == nullptr)')
transport = function(guides, 'bool CaptureBridgedGuides(')
assert transport.index('PresentReset::Instance().Observe(') < transport.index('return CaptureGuides(')
assert 'PresentReset::' not in function(bridge, 'void PresentHost::AbandonRecording(')
assert 'PresentReset::Instance().Confirm(_gameResetTicket, true, true);' in function(bridge, 'void PresentHost::ConfirmExecuted(')
assert bridge.index('PresentReset::Pending::Apply(_gameResetTicket, frame);') < bridge.index('const bool passed = EvaluateAtPresent(')
ngx = function(probe, 'void DlssNr::SlBoundaryProbe::NoteDlssgEvaluate(')
assert ngx.index('DlssNrEnabled.value_or_default() && params != nullptr') < ngx.index('params->Get("DLSSG.Reset"') < ngx.index('if (!g_installed.load())')
assert 'multiFrameIndex <= 1 && params->Get("DLSSG.Reset", &reset) == NVSDK_NGX_Result_Success' in ngx
assert 'PresentReset::Instance().Observe(true, reset != 0);' in ngx
assert 'if (IsSL1AndFGActive() || Config::Instance()->DlssNrEnabled.value_or_default())' in hooks
note = hooks[hooks.index('else if (nrOn)'):hooks.index('if (State::Instance().activeFgInput == FGInput::DLSSG)', hooks.index('else if (nrOn)'))]
assert 'AttachInterposerHook(&(PVOID&) o_slSetConstants, (PVOID) hkslSetConstants);' in note
# Existing production reset-rate accounting receives the game's non-policy flag; no second counter is added.
assert 'NoteResetSent(frameReset, frameResetIsPolicy, frameResetIsSceneCut);' in renderer
print('present reset: capture-before-refusal, submission acknowledgment, NGX first-frame and cause wiring passed')
