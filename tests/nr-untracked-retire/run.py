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

# The real admission state machine, with COM/NGX replaced by observable host fakes.
migration_slice = lifetime[lifetime.index('namespace\n{\n// Under g_nrMutex.'):lifetime.index('// Detach the entire NR generation')]
migration_slice += body(lifetime, 'bool WantsTrackedRecording(') + '\n'
migration_slice += body(lifetime, 'bool TrackNrRecording(') + '\n'
with tempfile.TemporaryDirectory(prefix='retoque-nr-migration-') as folder:
    folder = Path(folder)
    (folder / 'pch.h').write_text('// Host fixture: no Windows PCH.\n')
    (folder / 'migration.inc').write_text(migration_slice)
    binary = folder / 'migration'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(folder),
                    str(HERE / 'migration_cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    mutations = (
        ('detach omitted', '            ParkUntrackedGeneration();', '            /* omitted */'),
        ('pending CPU list ignored',
         'if (std::any_of(s_legacyPending.begin(), s_legacyPending.end(), [](auto* list) { return list != nullptr; }))',
         'if (false && std::any_of(s_legacyPending.begin(), s_legacyPending.end(), [](auto* list) { return list != nullptr; }))'),
        ('device removal accepted', 'completed != UINT64_MAX && ', ''),
        ('second queue accepted', 'queue != s_legacyQueue || ', ''),
        ('individual opt-in ignored', 'requested.Count > 1 || requested.Individual ||', 'requested.Count > 1 ||'),
        ('undrained generation accepted', 'return r.requiresPresentFence;', 'return r.requiresPresentFence && false;'),
    )
    for label, old, new in mutations:
        assert old in migration_slice, label
        (folder / 'migration.inc').write_text(migration_slice.replace(old, new))
        subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(folder),
                        str(HERE / 'migration_cases.cpp'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert result.returncode != 0, 'surviving mutation: ' + label
    print(f'PASS: {len(mutations)} unsafe migration mutations rejected')

park = body(lifetime, 'static void ParkUntrackedGeneration()')
# Inventory all NrState GPU-resource members, including arrays and the alternate-format set.
state = internal[internal.index('struct NrState'):internal.index('struct PreUpscaleState')]
import re
# Explicitly check the non-alt members; an alternate-set helper must not hide an omitted main resource.
state_no_alt = state[:state.index('    struct AltSurfaces')] + state[state.index('    } alt;') + len('    } alt;'):]
for member in re.findall(r'ID3D12Resource\* (\w+)', state_no_alt):
    assert 'g_nr.' + member in park, member
for member in ('feature', 'passFeature', 'superUp', 'superDown', 'stabilizer', 'uiMask', 'cadence', 'guideMatch', 'periphery'):
    assert 'g_nr.' + member in park, member
assert 'std::move(g_compose)' in park and 'ParkTrackingDiagnostics();' in park and 'ParkNrEnlarger(g_enlarge.live)' in park
assert 'retired.requiresPresentFence = true;' in park and 'g_tracked = true' not in park
assert 'delete ' not in park and '->Release()' not in park
shutdown = body(dx12, 'void Shutdown()')
assert shutdown.index('TrackingTransitionPending()') < shutdown.index('TickNrRetired(true)')
submit = body(dx12, 'static bool SubmitPresentList(')
assert submit.index('queue->ExecuteCommandLists(') < submit.index('queue->Signal(') < submit.index('std::lock_guard<std::mutex> nrLock(g_nrMutex);') < submit.index('EndUntrackedPresent(')
assert 'EndUntrackedPresent(g_presentList.list[slot], nullptr, nullptr, 0)' in body(dx12, 'static void DropPresentList(')
assert dx12.count('frame.PrivatePresentRecording = true;') == 1
present = body(dx12, 'void RunPresentPass(')
assert present.index('frame.PrivatePresentRecording = true;') < present.index('recorded = EvaluateAtPresent(')
assert 'frame.PrivatePresentRecording' in body(dx12, 'bool DlssNr_Dx12::Dispatch(')
dispatch = body(dx12, 'bool DlssNr_Dx12::Dispatch(')
assert dispatch.index('this != g_compose.get()') < dispatch.index('TrackNrRecording(')
assert 'std::lock_guard<std::mutex> nrLock(g_nrMutex);' in body(dx12, 'static std::shared_ptr<DlssNr_Dx12> AcquireCompose()')
assert dx12.count('compose->Dispatch(') == 3 and 'g_compose->Dispatch(' not in dx12
assert 'std::shared_ptr<DlssNr_Dx12>* compose = nullptr;' in internal  # abandoned entries do not destroy on exit
match_queue = body(dx12, 'bool Dx12Internal::UntrackedPresentFenceMatches(')
assert 'PresentComIdentity(queue)' in match_queue and 'slot.queueIdentity == queueIdentity' in match_queue
assert '!UntrackedPresentFenceMatches(s_legacyQueue)' in body(lifetime, 'void TickNrRetired(bool shutdown)')

print('PASS: untracked -> tracked production admission, pending-list/queue evidence and complete generation inventory')

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
    assert 'r.requiresPresentFence = !r.tracked && s_trackingTransition;' in fn, sig

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
