#!/usr/bin/env python3
"""Host checks for untracked D3D12 NR retirement's present-queue fence gate (DlssNr_UntrackedRetire.h).

Compiles the pure header and verbatim production ParkNr*/TickNrRetired slices under ASan/UBSan, and checks
source wiring across DlssNr_Dx12.cpp, DlssNr_Dx12_Lifetime.cpp and all four D3D12 Present sites. No GPU.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def body(text: str, signature: str) -> str:
    assert text.count(signature) == 1, f'expected one definition: {signature!r}'
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated: ' + signature)


internal = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text(encoding='utf-8-sig')
lifetime = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp').read_text(encoding='utf-8-sig')
dx12 = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8-sig')
wrapped = (ROOT / 'OptiScaler/wrapped/wrapped_swapchain.cpp').read_text(encoding='utf-8-sig')
fg_hooks = (ROOT / 'OptiScaler/hooks/FG_Hooks.cpp').read_text(encoding='utf-8-sig')
boundary = (ROOT / 'retoque/dlssnr/DlssNr_SlBoundaryProbe.cpp').read_text(encoding='utf-8-sig')
bridge = (ROOT / 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp').read_text(encoding='utf-8-sig')
vcxproj = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (ROOT / 'OptiScaler/OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')

# 1. Compile and execute the pure decision rule + verbatim lifetime slices under ASan/UBSan.
slice_text = (
    body(internal, 'struct NrRetired\n') + ';\n'
    + 'std::vector<NrRetired> g_nrRetired;\n'
    + body(lifetime, 'void ParkNrFeature(void*& feature)') + '\n'
    + body(lifetime, 'void ParkNrResource(ID3D12Resource*& res)') + '\n'
    + body(lifetime, 'void ParkNrClone(ID3D12Resource*& res, const DlssNr::Submission::Usage& usage, bool tracked)') + '\n'
    + body(lifetime, 'void TickNrRetired(bool shutdown)') + '\n'
)

with tempfile.TemporaryDirectory(prefix='retoque-nr-untracked-retire-') as folder:
    folder = Path(folder)
    (folder / 'artifacts').mkdir()
    (folder / 'artifacts/lifetime_slice.inc').write_text(slice_text, encoding='utf-8')
    binary = folder / 'cases'
    subprocess.run(
        ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-I' + str(ROOT / 'retoque'), '-I' + str(folder),
         str(HERE / 'cases.cpp'), '-o', str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)

# 2. Source guards: every ParkNr* helper stamps presentSerial on untracked entries, NR-disabled path does no
#    present-fence work, and all four D3D12 Present sites notify on SUCCEEDED non-test presents.
park_signatures = (
    'void ParkNrFeature(void*& feature)',
    'void ParkNrResource(ID3D12Resource*& res)',
    'void ParkNrScaler(OS_Dx12*& scaler)',
    'void ParkNrStabilizer(DlssNr_Stabilizer_Dx12*& stabilizer)',
    'void ParkNrUiMask(DlssNr_UiMask_Dx12*& uiMask)',
    'void ParkNrCadence(DlssNr_Cadence_Dx12*& cadence)',
    'void ParkNrEnlarger(NrEnlarger*& enlarger)',
    'void ParkNrDetailStats(DlssNr_DetailStats_Dx12*& detailStats)',
    'void ParkNrPulse(DlssNr_Pulse_Dx12*& pulse)',
    'void ParkNrMotionProbe(DlssNr_MotionProbe_Dx12*& probe)',
    'void ParkNrClone(ID3D12Resource*& res, const DlssNr::Submission::Usage& usage, bool tracked)',
)
for sig in park_signatures:
    fn = body(lifetime, sig)
    assert 'r.presentSerial = SnapshotUntrackedPresentSerial();' in fn, f'{sig} does not stamp presentSerial'

note = body(dx12, 'void NoteSwapchainPresented(ID3D12CommandQueue* queue, PresentSignalSource source)')
first_com = note.index('NrRealObject(')
assert note.index('!cfg.DlssNrEnabled.value_or_default()') < first_com, 'NR-off check must precede any COM/hook work'
assert note.index('State::Instance().isShuttingDown') < first_com, 'shutdown check must precede any COM/hook work'
assert note.index('Consumers::HandOverIfCompeting()') < first_com, 'competing-consumer check must precede COM work'

assert wrapped.count('DlssNr::NoteSwapchainPresented(cq, DlssNr::PresentSignalSource::WrappedSwapchain);') == 1
assert fg_hooks.count('DlssNr::NoteSwapchainPresented(nrQueue, DlssNr::PresentSignalSource::FgSwapchain);') == 1
assert boundary.count('DlssNr::NoteSwapchainPresented(queue, DlssNr::PresentSignalSource::Boundary);') == 1
assert bridge.count('DlssNr::NoteSwapchainPresented(presentQueue, DlssNr::PresentSignalSource::Dx11Bridge);') == 1

entry = '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_UntrackedRetire.h"'
assert entry in vcxproj, 'OptiScaler.vcxproj does not list DlssNr_UntrackedRetire.h'
assert entry in filters, 'OptiScaler.vcxproj.filters does not list DlssNr_UntrackedRetire.h'

print('PASS: DlssNr_UntrackedRetire decision rules, verbatim ParkNr*/TickNrRetired slices, and D3D12 Present wiring')
sys.exit(0)
