#!/usr/bin/env python3
"""Exercise DlssNr::DecidePresentSync: what [DlssNr] PresentSync=auto means.

1. The rule's cases, compiled from the production header under ASan/UBSan.
2. Mutation controls: each condition of the FSR-FG exception taken out must fail a case.
3. Source guards on what the exception rests on and is not executed here: RunPresentPass gathering the inputs, the
   frame generation hook handing the pass the FG object's game queue before the swapchain's Present, and the two
   swapchains signalling a fence on their game queue and making the queue that reads the frame wait for it.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
sys.path.insert(0, str(here.parent / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

header = (root / 'retoque/dlssnr/DlssNr_PresentSync.h').read_text(encoding='utf-8')
condition = '    if (in.fgHook && in.fsrFgOutput && !in.fsrFgCustomProvider && in.onFgGameQueue)\n'
serial = ('        if (in.serialRecordings)\n'
          '            return { true, "auto: FSR-FG, with multi-pass recordings admitted one at a time" };\n')
ordered = ('        return { false, "auto: FSR-FG\'s swapchain orders the game queue\'s work ahead of its own on the '
           'GPU" };\n')
dlssg = '    if (in.dlssgSeen || in.dlssgLoaded)\n        return { true, "auto: DLSS-G was seen or is loaded" };\n'
other = '    if (in.fgHook || in.fgOutput)\n'
assert all(header.count(text) == 1 for text in (condition, serial, ordered, dlssg, other))
assert header.index(dlssg) < header.index(condition) < header.index(serial) < header.index(ordered), \
    'DLSS-G is asked before the FSR-FG exception, and serial recordings inside it'


def mutate(old: str, new: str) -> str:
    assert header.count(old) == 1, f'mutation anchor is not unique: {old!r}'
    return header.replace(old, new)


MUTANTS = {
    'the exception away from the hook': mutate('in.fgHook && in.fsrFgOutput &&', 'in.fsrFgOutput &&'),
    'any output taken for FSR-FG': mutate('in.fgHook && in.fsrFgOutput &&', 'in.fgHook &&'),
    'a custom provider taken for FFX': mutate(' && !in.fsrFgCustomProvider', ''),
    'any queue taken for the swapchain\'s': mutate(' && in.onFgGameQueue)', ')'),
    'the exception ahead of DLSS-G': mutate(dlssg, '').replace(other, dlssg + '\n' + other),
    'serial recordings left to skip frames': mutate(serial, ''),
    'no exception at all': mutate(ordered, '        return { true, "auto: FSR-FG\'s swapchain orders" };\n'),
}


def passes(directory: Path, header_text: str | None) -> bool:
    directory.mkdir()
    includes = []
    if header_text is not None:
        (directory / 'dlssnr').mkdir()
        (directory / 'dlssnr/DlssNr_PresentSync.h').write_text(header_text, encoding='utf-8')
        includes = ['-I', str(directory)]
    binary = directory / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *includes,
                    '-I', str(root / 'retoque'), '-I', str(root / 'OptiScaler'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=header_text is not None, text=True)
    return result.returncode == 0


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-present-sync-') as directory:
    if not passes(Path(directory) / 'production', None):
        sys.exit(1)
    for index, (name, text) in enumerate(MUTANTS.items()):
        if passes(Path(directory) / f'mutant-{index}', text):
            print(f'FAIL: no case notices {name}')
            sys.exit(1)

# --- What the exception rests on ------------------------------------------------------------------------------------
failures = []


def expect(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


renderer = renderer_source()
present = renderer[renderer.index('void RunPresentPass('):renderer.index('// ReportSpatialContract, GatherGuideFrame')]
for line in ('syncInputs.fsrFgOutput = state.activeFgOutput == FGOutput::FSRFG;',
             'syncInputs.fsrFgCustomProvider = !state.activeFsrFgProvider.empty();',
             'syncInputs.onFgGameQueue = fgHook && state.currentFG != nullptr && '
             'state.currentFG->GetCommandQueue() == queue;',
             'syncInputs.serialRecordings = tracked && DlssNr::Submission::Serialized();',
             'DlssNr::DecidePresentSync(cfg.DlssNrPresentSync, syncInputs);'):
    expect(present.count(line) == 1, f'RunPresentPass: expected one {line}')
# The wait is the last thing the pass does: after the submit, and decided once.
expect(present.index('SubmitPresentList(queue, slot)') < present.index('DlssNr::DecidePresentSync(') <
       present.index('WaitForSingleObject(g_presentList.fenceEvent, 1000)'), 'the wait no longer follows the submit')
expect(present.count('if (presentSync && ') == 1, 'the wait is not gated by the decision alone')

# "Admitted one at a time" is the admission rule's own two conditions (DlssNr_SubmissionModel.h, Admit): strict mode
# refuses with Replay, no established single queue with Serial, both while anything earlier is unfinished.
submission = (root / 'retoque/dlssnr/DlssNr_Submission.cpp').read_text(encoding='utf-8')
serialized = submission[submission.index('bool Serialized()'):]
serialized = serialized[:serialized.index('\n}\n')]
expect('std::lock_guard lock(stateMutex);' in serialized and
       'return model.Strict() || !model.Overlap();' in serialized, 'Submission::Serialized is not the admission rule')
expect('Detail::Admit(usage, FenceValue, model.Clock(), model.Strict(), model.Overlap(), key);' in submission,
       'Submission::Admit no longer decides on Strict and Overlap')
model = (root / 'retoque/dlssnr/DlssNr_SubmissionModel.h').read_text(encoding='utf-8')
admit = model[model.index('Admission Admit(const Usage& usage, Completed completed'):]
admit = admit[:admit.index('\n}\n')]
expect('if (strict)\n            return Admission::Replay;' in admit and
       'if (!overlap)\n            return Admission::Serial;' in admit,
       'the admission model no longer refuses on strict or on no overlap')

# The hook: the pass on the FG object's game queue, before the swapchain's own Present. Present1 is the same body.
hooks = (root / 'OptiScaler/hooks/FG_Hooks.cpp').read_text(encoding='utf-8-sig')
fg_present = hooks[hooks.index('HRESULT FGHooks::FGPresent('):]
queue = ('ID3D12CommandQueue* nrQueue = (fgDx12 != nullptr && fgDx12->GetCommandQueue() != nullptr)\n'
         '                                      ? fgDx12->GetCommandQueue()\n'
         '                                      : state.currentCommandQueue;')
run = 'DlssNr::RunPresentPass((IDXGISwapChain3*) This, nrQueue, true);'
forward = 'result = o_FGSCPresent(This, SyncInterval, Flags);'
expect(all(fg_present.count(text) == 1 for text in (queue, run, forward)), 'the FG hook\'s pass or its queue moved')
if not failures:
    expect(fg_present.index(queue) < fg_present.index(run) < fg_present.index(forward),
           'the FG hook no longer runs the pass on the FG object\'s queue before the swapchain\'s Present')

# FSR-FG keeps as its game queue the one it created the swapchain on, FFX's and the fork's alike.
fsrfg = (root / 'OptiScaler/framegen/ffx/FSRFG_Dx12.cpp').read_text(encoding='utf-8-sig')
for create in ('bool FSRFG_Dx12::CreateSwapchainInternal(', 'bool FSRFG_Dx12::CreateSwapchain1Internal('):
    body = fsrfg[fsrfg.index(create):]
    body = body[:body.index('\n}\n')]
    expect('createSwapChainDesc.gameQueue = realQueue;' in body and '_gameCommandQueue = realQueue;' in body and
           ('Lsfg::Presenter::CreateSwapChain(realFactory, realQueue,' in body or
            'Lsfg::Presenter::CreateSwapChain1(realFactory, realQueue,' in body),
           f'{create} no longer keeps the queue the swapchain was created on')

# Both swapchains, in both present paths: a fence signalled on the game queue, and the queue that reads the frame
# made to wait for it. The interpolation queue's own signal is what the present queue waits for in that path.
ORDERING = (
    ('presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);\n'
     '    presentInfo.interpolationQueue->Wait(presentInfo.gameFence, gameFenceValue);'),
    ('presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);\n'
     '        presentInfo.presentQueue->Wait(presentInfo.gameFence, gameFenceValue);'),
)
fork = (root / 'retoque/lsfg/presenter/MfgSwapChain_Dx12.cpp').read_text(encoding='utf-8-sig')
for text in ORDERING:
    expect(fork.count(text) == 1, 'the fork\'s presenter no longer orders its game queue ahead of what reads the frame')
expect('presenter->presentQueue->Wait(presenter->interpolationFence,' in fork,
       'the fork\'s presenter thread no longer waits for the interpolation queue')

# FFX's source is a submodule host CI does not check out: held where it is present, said where it is not.
ffx = root / ('external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/dx12/'
              'FrameInterpolationSwapchainDX12.cpp')
ffx_checked = ffx.is_file()
if ffx_checked:
    ffx_text = ffx.read_text(encoding='utf-8-sig', errors='replace')
    for text in ORDERING:
        expect(ffx_text.count(text) == 1,
               'FFX\'s swapchain no longer orders its game queue ahead of what reads the frame')
    expect('presenter->presentQueue->Wait(presenter->interpolationFence,' in ffx_text,
           'FFX\'s presenter thread no longer waits for the interpolation queue')

if failures:
    for failure in failures:
        print(f'FAIL: {failure}')
    sys.exit(1)

ffx_note = "FFX's vendored swapchain too" if ffx_checked else "FFX's vendored source is not checked out: not checked"
print(f"PASS: {len(MUTANTS)} mutants rejected; RunPresentPass gathers the FSR-FG inputs, the FG hook runs the pass on "
      f"the FG object's queue before the swapchain's Present, and the fork's presenter orders that queue ({ffx_note})")
