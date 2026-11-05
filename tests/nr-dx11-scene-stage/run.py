#!/usr/bin/env python3
"""Native D3D11 scene route: compiled policy and integration contracts, each mutant must fail."""
from pathlib import Path
import os
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
OUT.mkdir(exist_ok=True)
FILES = ['retoque/dlssnr/DlssNr_Dx11ScenePolicy.h',
         'retoque/dlssnr/DlssNr_Dx11SceneTransport.h', 'retoque/dlssnr/DlssNr_Dx11Scene.cpp',
         'OptiScaler/Config.h', 'OptiScaler/Config.cpp', 'OptiScaler.ini',
         'OptiScaler/upscalers/dlss/DLSSFeature_Dx11.cpp', 'retoque/dlssnr/DlssNr_Menu.cpp',
         'retoque/shaders/dlssnr/DlssNr_Dx12.cpp', 'retoque/shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp']
base = {name: (ROOT / name).read_text() for name in FILES}


def contracts(files):
    policy, transport, scene, config, io, ini, seam, menu, renderer, lifetime = [files[f] for f in FILES]
    code = re.sub(r'//[^\n]*', '', transport)
    for banned in ('WaitForSingleObject', 'SetEventOnCompletion', 'AcquireSync', 'Sleep(', 'Map(', 'SyncDx12ToDx11'):
        assert banned not in code, 'CPU wait / alternate synchronization'
    run = code[code.index('Result Run('):code.index('bool Retire(')]
    steps = ['if (!Ready())', 'Textures(inputs)', 'allocator->Reset()', 'list->Reset(', 'record(list.Get()',
             'list->Close()', 'context->CopyResource(shared[i].Get()', 'context->Signal(p11.Get()',
             'context->Flush()', 'queue->Wait(p12.Get()', 'queue->ExecuteCommandLists(',
             'queue->Signal(n12.Get()', 'context->Wait(n11.Get()', 'context->CopyResource(inputs[0]',
             'context->Signal(c11.Get()']
    indexes = [run.index(s) for s in steps]
    assert indexes == sorted(indexes), 'P -> NR -> N -> consumer -> C / bounded reuse ordering'
    assert 'if (recorded)' in run, 'declined model must not replace the game output'
    assert 'poisoned = true;' in run and 'poisoned = false;' in run, 'retain uncertain submission'
    assert 'Complete(c12->GetCompletedValue(), serial)' in code, 'consumer retirement proof'
    assert 'bytes > MaxBytes / 2' in code, 'bounded memory'
    assert 'GetVulkanQueueInfo(present, &display' in code and 'GetSubmissionQueue(&game' in code
    assert 'QueueRefusal(' in code, 'VkQueue equality enforced'
    assert 'SharedFencesUnavailable' in code and 'Fence(true, c11, c12)' in code
    body = scene[scene.index('void EvaluateNativeDx11Scene('):scene.index('const char* NativeDx11SceneStatus()')]
    assert body.index('if (!Eligible()') < body.index('Pictures::Ask(') < body.index('std::unique_lock') < body.index('PrepareD3D12ForD3D11(')
    assert '.role == Pictures::Role::Aside' in body
    assert 'std::try_to_lock' in body and 'if (!lock.owns_lock() || refused)' in body
    assert 'frame.NativeDx11Scene = true' in body and 'GatherGuideFrame(params)' in body
    assert 'Dx11Upscaler.has_value()' in scene and 'Upscaler::DLSS' in scene
    assert 'EffectiveMethod(boundary)' in scene
    assert 'if (!transport->Retire())' in scene and scene.index('if (!transport->Retire())') < scene.index('delete transport;')
    assert 'Submission::Track(list, probe)' in scene and 'SubmissionTrackingUnavailable' in scene
    assert 'CustomOptional<bool> DlssNrDx11SceneStage { false }' in config
    assert io.count('_dlssNrDx11SceneStageAtStart = DlssNrDx11SceneStage.value_or_default()') == 1
    assert 'DlssNrDx11SceneStage.set_from_config(readBool("DlssNr", "Dx11SceneStage"))' in io
    assert 'Instance()->DlssNrDx11SceneStage.value_for_config()' in io
    assert 'Dx11SceneStage=false' in ini
    evaluate = seam[seam.index('bool DLSSFeatureDx11::EvaluateInternal('):]
    assert evaluate.index('if (nvResult != NVSDK_NGX_Result_Success)') < evaluate.index('EvaluateNativeDx11Scene(') < evaluate.index('LOG_TRACE')
    assert 'routeNotice = scene;' in menu and 'in.dx11Route = scene;' in menu
    assert 'WantsTrackedRecording(passSnapshot, frame.NativeDx11Scene)' in renderer
    assert 'TrackNrRecording(cmdList, passSnapshot, recording, frame.NativeDx11Scene)' in renderer
    assert 'mandatory && g_legacyRecorded' in lifetime and 'optIn = mandatory ||' in lifetime
    assert renderer.index('ShutdownNativeDx11Scene();') < renderer.index('ShutdownNativeDx11Guides();')


def compiled(header, name, expected):
    path = OUT / name
    path.mkdir(exist_ok=True)
    (path / 'DlssNr_Dx11ScenePolicy.h').write_text(header)
    exe = path / 'policy'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(path), str(HERE / 'policy.cpp'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert (result.returncode == 0) == expected, name


contracts(base)
compiled(base[FILES[0]], 'baseline', True)
mutants = [
    (0, 'startup && enabled &&', 'enabled && (startup || !startup) &&', 'opt-in'),
    (0, 'explicitNative &&', '(explicitNative || !explicitNative) &&', 'native-only'),
    (0, 'hook == 1', '(hook == 1 || hook == 2)', 'hook-method'),
    (0, 'stage == 0', '(stage == 0 || stage == 1)', 'post-only'),
    (0, 'completed != UINT64_MAX &&', '', 'device-lost'),
    (0, 'nr == consumer || nr == present', 'nr == consumer', 'present-alias'),
    (0, 'nr == consumer || nr == present', 'nr == present', 'consumer-alias'),
    (0, '!nr || !consumer || !present', '!nr || !consumer', 'missing-queue'),
    (1, 'Complete(c12->GetCompletedValue(), serial)', 'Complete(n12->GetCompletedValue(), serial)', 'retirement'),
    (1, 'bytes > MaxBytes / 2', 'bytes > MaxBytes * 2', 'memory-cap'),
    (1, 'if (recorded)', 'if (true)', 'declined-copyback'),
    (1, 'context->Wait(n11.Get(), serial)', 'context->Signal(n11.Get(), serial)', 'consumer-edge'),
    (1, 'queue->Wait(p12.Get(), serial)', 'queue->Signal(p12.Get(), serial)', 'producer-edge'),
    (2, '.role == Pictures::Role::Aside', '.role != Pictures::Role::Aside', 'main-picture'),
    (2, 'Submission::Track(list, probe)', 'true', 'tracking'),
    (3, 'DlssNrDx11SceneStage { false }', 'DlssNrDx11SceneStage { true }', 'default-off'),
    (4, 'readBool("DlssNr", "Dx11SceneStage")', 'readBool("DlssNr", "WrongKey")', 'read-key'),
    (4, 'Instance()->DlssNrDx11SceneStage.value_for_config()', 'Instance()->DlssNrEnabled.value_for_config()', 'save-key'),
    (9, 'mandatory && g_legacyRecorded', 'false && g_legacyRecorded', 'legacy-lifetime'),
]
for index, old, new, name in mutants:
    changed = dict(base)
    assert old in changed[FILES[index]], (name, old)
    changed[FILES[index]] = changed[FILES[index]].replace(old, new, 1)
    if index == 0:
        compiled(changed[FILES[0]], name, False)
    else:
        try:
            contracts(changed)
        except (AssertionError, ValueError):
            pass
        else:
            raise AssertionError('surviving mutant: ' + name)
    print('KILLED:', name)
print(f'PASS: compiled policy, integration contracts, {len(mutants)} individually rejected mutants; no GPU/NGX claim')
