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
guides = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp').read_text(encoding='utf-8-sig')
chain = (ROOT / 'retoque/dlssnr/DlssNr_Chain.h').read_text(encoding='utf-8-sig')
common = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Common.h').read_text(encoding='utf-8-sig')
wrapped = (ROOT / 'OptiScaler/wrapped/wrapped_swapchain.cpp').read_text(encoding='utf-8-sig')
fg_hooks = (ROOT / 'OptiScaler/hooks/FG_Hooks.cpp').read_text(encoding='utf-8-sig')
boundary = (ROOT / 'retoque/dlssnr/DlssNr_SlBoundaryProbe.cpp').read_text(encoding='utf-8-sig')
bridge = (ROOT / 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp').read_text(encoding='utf-8-sig')
vcxproj = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (ROOT / 'OptiScaler/OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')

# 1. Compile and execute the pure decision rule + verbatim lifetime slices under ASan/UBSan.
park_generation = body(lifetime, 'static void ParkUntrackedGeneration(bool migrating = true)')
slice_text = (
    body(internal, 'struct NrRetired\n') + ';\n'
    + 'std::vector<NrRetired> g_nrRetired;\n'
    + 'std::vector<NrRetired> s_leftBehind;\n'
    + 'void TickNrRetired(bool shutdown = false);\n'
    + body(lifetime, 'void ParkNrFeature(void*& feature)') + '\n'
    + body(lifetime, 'void ParkNrResource(ID3D12Resource*& res)') + '\n'
    + body(lifetime, 'void ParkNrClone(ID3D12Resource*& res, const DlssNr::Submission::Usage& usage, bool tracked)') + '\n'
    + body(lifetime, 'void TickNrRetired(bool shutdown)') + '\n'
    + body(lifetime, 'bool RetiredCapacity()') + '\n'
    + body(guides, 'static bool PresentGuideRetirementCapacity()') + '\n'
    + body(lifetime, 'static void LeaveUntrackedBehind()') + '\n'
)
chain_slice = 'namespace DlssNr::Chain\n{\n' + body(chain, 'inline bool RetirementAllowed(size_t parked)') + '\n}\n'

with tempfile.TemporaryDirectory(prefix='retoque-nr-untracked-retire-') as folder:
    folder = Path(folder)
    (folder / 'artifacts').mkdir()
    (folder / 'artifacts/chain_slice.inc').write_text(chain_slice, encoding='utf-8')
    binary = folder / 'cases'
    build = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-I' + str(ROOT / 'retoque'), '-I' + str(folder),
             str(HERE / 'cases.cpp'), '-o', str(binary)]
    (folder / 'artifacts/lifetime_slice.inc').write_text(slice_text, encoding='utf-8')
    subprocess.run(build, check=True)
    subprocess.run([str(binary)], check=True)
    # What a live opt-in leaves behind is out of every collector's reach; guide clones and tracked entries stay.
    lifetime_mutations = (
        ('left-behind generation stays where the collector frees it',
         '        s_leftBehind.push_back(std::move(g_nrRetired[i]));\n        g_nrRetired[i] = std::move(g_nrRetired.back());\n        g_nrRetired.pop_back();',
         '        ++i;'),
        ('guide clones left behind with the generation', ' || g_nrRetired[i].cloneBytes != 0)', ')'),
        ('tracked entries left behind with the generation', 'if (g_nrRetired[i].tracked || ', 'if ('),
    )
    for label, old, new in lifetime_mutations:
        assert slice_text.count(old) == 1, label
        (folder / 'artifacts/lifetime_slice.inc').write_text(slice_text.replace(old, new), encoding='utf-8')
        subprocess.run(build, check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert result.returncode != 0, 'surviving mutation: ' + label
    print(f'PASS: {len(lifetime_mutations)} unsafe left-behind generation mutations rejected')

# The real admission state machine, with COM/NGX replaced by observable host fakes.
migration_slice = body(internal, 'enum class NrRecordingRoute : uint8_t\n') + ';\n'
migration_slice += lifetime[lifetime.index('namespace\n{\n// Under g_nrMutex.'):lifetime.index('// Detach the entire NR generation')]
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
        # Live opt-in from the upscaler's output (chain-in-flight.md): the one-pass path is never enrolled, the
        # generation is left behind once, only on that route, only when a second one fits and can be followed.
        ('Passes=1 enrolled in tracking', 'requested.Count > 1 || requested.Individual ||', 'requested.Count > 0 || requested.Individual ||'),
        ('budget check skipped', 'if (!SecondGenerationFits(cmd))', 'if (false && !SecondGenerationFits(cmd))'),
        ('tracker check skipped', 'else if (!DlssNr::Submission::Track(cmd, probe, false))',
         'else if (false && !DlssNr::Submission::Track(cmd, probe, false))'),
        ('budget asked on every frame', 's_retainRefusal != nullptr && g_nrTicks - s_retainRefusedAt < kRetainRetryTicks', 'false'),
        ('bridge or before-upscale history allowed', '!s_legacyPrivatePresent && !s_legacyOtherRoute &&', '!s_legacyPrivatePresent &&'),
        ('private present history allowed', '!s_legacyPrivatePresent && !s_legacyOtherRoute &&', '!s_legacyOtherRoute &&'),
        ('opt-in on another route allowed', 'route == NrRecordingRoute::AfterUpscale && s_legacyAfterUpscale &&', 's_legacyAfterUpscale &&'),
        ('mandatory scene stage allowed', '!s_trackingTransition && !mandatory && route ==', '!s_trackingTransition && route =='),
        ('driver proxy allowed', 'else if (DriverProxyRequested())', 'else if (false)'),
        ('generation retained twice', ' && !s_generationRetained)', ')'),
        ('tracking not armed with the detach', '            g_tracked = true; // Sticky from the same call', '            // g_tracked = true; // Sticky from the same call'),
        ('detach omitted from the live opt-in', '            RetainUntrackedGeneration(cmd);', '            /* omitted */'),
        ('recorded on the detached generation',
         'return false; // Never record with the generation this call detached', 'return true; // mutated'),
        ('one-pass fallback dropped on refusal',
         '                ChainStatus(kept);\n                NoteUntrackedRecording(cmd, route);\n                return true;',
         '                ChainStatus(kept);\n                NoteUntrackedRecording(cmd, route);\n                return false;'),
    )
    for label, old, new in mutations:
        assert old in migration_slice, label
        (folder / 'migration.inc').write_text(migration_slice.replace(old, new))
        subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(folder),
                        str(HERE / 'migration_cases.cpp'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        assert result.returncode != 0, 'surviving mutation: ' + label
    print(f'PASS: {len(mutations)} unsafe migration and live opt-in mutations rejected')

park = park_generation
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

# Live opt-in from the upscaler's output: the detach is the migration's own inventory without its fence gate, nothing
# in it is destroyed, and what it leaves behind is in a list nothing else reads -- no collector, cap or shutdown.
retain = body(lifetime, 'void RetainUntrackedGeneration(ID3D12GraphicsCommandList* cmd)')
assert retain.index('ParkUntrackedGeneration(false);') < retain.index('LeaveUntrackedBehind();') < retain.index('RetainedLine(')
assert 'delete ' not in retain and '->Release()' not in retain.replace('device->Release()', '') and 'g_tracked' not in retain
assert 'if (migrating)\n        for (auto& retired : g_nrRetired)\n            if (!retired.tracked)\n                retired.requiresPresentFence = true;' in park
leave = body(lifetime, 'static void LeaveUntrackedBehind()')
assert 'delete ' not in leave and 'Release' not in leave
units = {path.name: path.read_text(encoding='utf-8-sig') for path in sorted((ROOT / 'retoque').rglob('*')) if path.suffix in ('.cpp', '.h')}
assert [name for name, text in units.items() if 's_leftBehind' in text] == ['DlssNr_Dx12_Lifetime.cpp']
uses = [line.strip() for line in lifetime.splitlines() if 's_leftBehind' in line and not line.lstrip().startswith('//')]
assert uses == ['std::vector<NrRetired> s_leftBehind;',
                's_leftBehind.push_back(std::move(g_nrRetired[i]));',
                'const size_t before = s_leftBehind.size();',
                'for (size_t i = before; i < s_leftBehind.size(); ++i)',
                'const auto& retired = s_leftBehind[i];'], uses
assert lifetime.count('LeaveUntrackedBehind()') == 2 and lifetime.count('RetainUntrackedGeneration(cmd)') == 1
assert lifetime.count('ParkUntrackedGeneration(') == 3  # definition, the migration, the live opt-in
fits = body(lifetime, 'bool SecondGenerationFits(ID3D12GraphicsCommandList* cmd)')
assert 'VideoMemory(device, budget)' in fits and 'DlssNr::Chain::AdmitBesideRetained(' in fits
assert 'DlssNr::Chain::GenerationSurfaceBytes(g_nr.width, g_nr.height, g_nr.workWidth, g_nr.workHeight, gridPixels)' in fits
for write in ('g_nr.', 'g_nrRetired', 'g_tracked'):
    assert not re.search(re.escape(write) + r'[\w.\[\]]*\s*=[^=]', fits), write  # asks, changes nothing
# The same surface count Dispatch uses for the generation the tracked path builds next.
assert ('const uint64_t surfaceBytes = uint64_t(workWidth) * workHeight * 48 + uint64_t(width) * height * 48 +\n'
        '                                      (peripheryActive ? uint64_t(gridWidth) * gridHeight * 16 : 0);') in dx12
assert 'return workWidth * workHeight * 48 + width * height * 48 + gridPixels * 16;' in chain

# The default one-pass path gains nothing from this: no tracker call, lock, wait, allocation or log. The executable
# cases count them on the production body; these pin the two places a recording on that path passes through.
track = body(lifetime, 'bool TrackNrRecording(')
assert ('    if (!g_tracked)\n    {\n        g_legacyRecorded = true;\n        NoteUntrackedRecording(cmd, route);\n'
        '        return true;\n    }\n') in track
gate = track.index('if (!g_tracked && g_legacyRecorded && (optIn || s_trackingTransition))')
for inside in ('SecondGenerationFits(cmd)', 'RetainUntrackedGeneration(cmd)', 'DriverProxyRequested()',
               'DlssNr::Submission::Track(cmd, probe, false)'):
    assert track.count(inside) == 1 and gate < track.index(inside) < track.index('if (!g_tracked && optIn && !g_legacyRecorded)'), inside
note_untracked = body(lifetime, 'static void NoteUntrackedRecording(ID3D12GraphicsCommandList* cmd, NrRecordingRoute route)')
for forbidden in ('Submission::', 'AddRef', 'new ', 'make_', 'lock', 'Wait', 'Signal', 'CreateFence', 'LOG_', 'push_back'):
    assert forbidden not in note_untracked, forbidden
# Each route names itself; the mark is set in one place, after the capture that copies the frame for the present.
after = body(dx12, 'void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,')
assert dx12.count('frame.AfterUpscaleRecording = true;') == 1
assert after.index('CaptureTemporal(cmdList, params, frame)') < after.index('frame.AfterUpscaleRecording = true;') < after.index('compose->Dispatch(')
for source in sorted((ROOT / 'retoque').rglob('*.cpp')) + sorted((ROOT / 'OptiScaler').rglob('*.cpp')):
    if source.name != 'DlssNr_Dx12.cpp':
        assert 'AfterUpscaleRecording' not in source.read_text(encoding='utf-8-sig', errors='replace'), source
assert 'bool AfterUpscaleRecording = false;' in common
submit = body(dx12, 'static bool SubmitPresentList(')
assert submit.index('queue->ExecuteCommandLists(') < submit.index('queue->Signal(') < submit.index('std::lock_guard<std::mutex> nrLock(g_nrMutex);') < submit.index('EndUntrackedPresent(')
assert 'EndUntrackedPresent(g_presentList.list[slot], nullptr, nullptr, 0)' in body(dx12, 'static void DropPresentList(')
assert dx12.count('frame.PrivatePresentRecording = true;') == 1
present = body(dx12, 'void RunPresentPass(')
assert present.index('frame.PrivatePresentRecording = true;') < present.index('recorded = EvaluateAtPresent(')
assert 'frame.PrivatePresentRecording' in body(dx12, 'bool DlssNr_Dx12::Dispatch(')
dispatch = body(dx12, 'bool DlssNr_Dx12::Dispatch(')
assert dispatch.index('this != g_compose.get()') < dispatch.index('TrackNrRecording(')
assert ('const NrRecordingRoute recordingRoute = frame.PrivatePresentRecording ? NrRecordingRoute::PrivatePresent\n'
        '                                            : frame.AfterUpscaleRecording ? NrRecordingRoute::AfterUpscale\n'
        '                                                                          : NrRecordingRoute::Other;') in dispatch
assert 'TrackNrRecording(cmdList, passSnapshot, recording, frame.NativeDx11Scene, recordingRoute)' in dispatch
# The before-upscale scope never names a route: its recordings, and the scratch they write, stay a restart.
scope = body(dx12, 'ScopedPreUpscale::ScopedPreUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool applies,')
assert 'TrackNrRecording(cmdList, requested, _recording.get())' in scope
assert scope.index('TrackNrRecording(cmdList, requested, _recording.get())') < scope.index('g_pre.scratch = CreatePreUpscaleScratch(')
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
