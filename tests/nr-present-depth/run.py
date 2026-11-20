#!/usr/bin/env python3
"""Production CPU rules with sanitizers, runtime wiring guards and optional MinGW SDK syntax check. No GPU."""
from pathlib import Path
import os
import re
import shutil
import sys
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ARTIFACTS = HERE / 'artifacts'
ARTIFACTS.mkdir(exist_ok=True)
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-pthread']
RULE = 'retoque/dlssnr/DlssNr_PresentDepth.h'
TABLE = 'retoque/dlssnr/DlssNr_PresentDepthTables.h'
GLUE = 'retoque/dlssnr/DlssNr_PresentDepthDx12.cpp'

def read(p): return (ROOT / p).read_text(encoding='utf-8-sig')
def block(text, start):
    a=text.index(start); b=text.index('{',a); depth=1; end=b+1
    while depth:
        if text[end]=='{': depth+=1
        elif text[end]=='}': depth-=1
        end+=1
    return text[a:end]

def guards():
    glue=read(GLUE); core=read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
    device=read('OptiScaler/hooks/D3D12_Hooks.cpp'); track=read('OptiScaler/resource_tracking/ResTrack_dx12.cpp')
    capture=read('retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp')
    install=block(glue,'void Install(')
    assert install.index('Enabled(')<install.index('make_unique<Owner>')<install.index('DetourTransactionBegin')
    assert 'static const bool requested' in install and 'swapchainInteropApi != SwapchainInteropApi::None' in install
    assert 'if (!ok)' in install and 'DetourTransactionAbort()' in install
    assert install.index('DetourTransactionCommit()')<install.index('running.store(true')
    assert 'ScopedInternalResourceCreation::Active()' in block(glue,'void Created(')
    # Lifetime proof is the observer's own destruction notice. It registers nothing in ResTrack's index and no release
    # reaches HUDfix through it; ResTrack's two lifetime functions do not name NR (tests/restrack-resource-references).
    notice=block(glue,'bool NoticeRelease('); released=block(glue,'void __stdcall Released(')
    assert 'RegisterDestructionCallback(&Released, resource, &callback)' in notice
    assert notice.index('GetPrivateData(kReleaseNotice')<notice.index('RegisterDestructionCallback(')<notice.index('SetPrivateData(kReleaseNotice')
    assert 'isShuttingDown' in notice and 'isShuttingDown' in released
    assert 'model.Forget(' in released and 'resources.Put(key, {})' in released
    assert 'const bool lifetime = NoticeRelease(resource); // outside observer lock' in glue
    assert glue.count('NoticeRelease(') == 2 and 'r && r->lifetime ? id : 0' in glue
    for forbidden in ['TrackResourceRelease', '_trackedResources', 'Hudfix']:
        assert forbidden not in glue, forbidden
    assert sorted(set(re.findall(r'ResTrack_Dx12::(\w+)', glue))) == ['EnableNrSubmissionTracking', 'NrRealObject']
    for signature in ['void __stdcall ResTrack_Dx12::ResourceDestroyed(', 'bool ResTrack_Dx12::TrackResourceRelease(']:
        assert 'DlssNr' not in block(track, signature), signature
    assert 'DeviceDestroyed(device)' in device and 'PresentDepthDx12::Destroyed' not in track
    assert 'tokens[i] == kInternal' in glue and 'r->Usable(tokens[i])' in glue
    submit=block(track,'void ResTrack_Dx12::hkExecuteCommandLists(')
    assert submit.index('PresentDepthDx12::Submission')<submit.index('o_ExecuteCommandLists(')
    # Observer source cannot record GPU work, mutate a model input, alter the ring or add a fence/wait.
    for forbidden in ['CopyResource(', 'CopyTextureRegion(', 'ExecuteCommandLists(', 'CreateFence(', '->Signal(', '->Wait(', 'EvaluateAtPresent(', 'CommitRecord(', 'tracker.AtPresent(']:
        assert forbidden not in glue, forbidden
    observe=block(glue,'Observation::Observation(')
    assert 'model.Present(' in observe and 'intervalUnknown' in observe and 'inflight' in observe
    present=block(core,'void RunPresentPass(')
    assert present.index('PresentDepthDx12::Observation')<present.index('if (g_nr.failed')
    assert '(boundary || !BoundaryLive())' in present
    assert 'kFgHookTimeout' in present.split('PresentDepthDx12::InternalScope')[0]
    assert present.index('const size_t usableSlot = guides.decision.slot;')<present.index('PresentDepthDx12::Eligible(usableSlot)')
    assert capture.index('g_presentGuideRing.CommitRecord(')<capture.index('PresentDepthDx12::Captured(')
    assert 'frame.DepthSubrectBaseX == 0' in capture and 'frame.RenderSubrectWidth == depthDesc.Width' in capture
    # Original pairing clock stays gated on nonempty ring; the new observer does not advance it.
    ask=block(capture,'static GuideAsk AskGuideIdentity(')
    assert ask.index('NewestRecordedSequence() == 0')<ask.index('return ask;')<ask.index('tracker.AtPresent(')
    for p in ['OptiScaler/Config.h','OptiScaler/Config.cpp','OptiScaler.ini','Config.md']:
        assert 'PresentDepth' in read(p)
    cfg=read('OptiScaler/Config.cpp')
    assert 'DlssNrPresentDepth.set_from_config(readString("DlssNr", "PresentDepth", true))' in cfg
    assert '"PresentDepth", Instance()->DlssNrPresentDepth.value_for_config_or("auto").c_str()' in cfg
    for p in ['OptiScaler.ini','Config.md']:
        assert re.search(r'^PresentDepth=auto$',read(p),re.M) and 'constant or log' in read(p)
    names=['CreateList','CreateList1','View','Copy','CopySimple','Bind','Draw','Indexed','Clear','Barriers','Reset','Close','Bundle','Indirect','BeginPass','EndPass','Mesh','Enhanced']
    for name in names:
        assert glue.count(f'original{name}(') == 1, f'original forwarding count: {name}'
        assert f'ATTACH({name})' in glue and f'DETACH({name})' in glue and f'VALIDATE_MEMBER_HOOK(Hook{name}, {name}Fn)' in glue
    print('PASS: startup gate, 18 hook signatures/attach/detach, forwarding seam, own release notice, no GPU command, independent clock and config wiring')

guards()
with tempfile.TemporaryDirectory(prefix='host-', dir=ARTIFACTS) as temp:
    temp=Path(temp); (temp/'pch.h').write_text('#pragma once\n')
    if '--syntax-only' not in sys.argv:
        binary=temp/'cases'
        subprocess.run(FLAGS+['-I'+str(temp),'-I'+str(ROOT),str(HERE/'cases.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        mutants={
            'R1 chooses oldest': ('r.bind > f.r1.pick.bind','r.bind < f.r1.pick.bind'),
            'R2 loses margin': ('f.r2.runner.draws > f.r2.pick.draws / 2','f.r2.runner.draws > f.r2.pick.draws'),
            'MSAA accepted': ('r.shape.samples != 1','r.shape.samples == 0'),
            'extent ignored': ('if (!size)\n                continue;', 'if (false)\n                continue;'),
            'unknown state retained': ('r.source = StateSource::Unknown;\n                r.direction = -1;', 'r.source = StateSource::Creation;\n                r.direction = -1;'),
            'stale generation accepted': ('r.id == id ? &r : nullptr','r.id != 0 ? &r : nullptr'),
            'no debounce': ('elapsed < 2000000000LL','elapsed < 0'),
            'no heartbeat': ('elapsed < 30000000000LL','elapsed < 300000000000LL'),
            'cleared accepted': ('r.clear > r.use','r.clear < r.use'),
            'cross queue accepted': ('r.otherQueue || r.queue != queue','false && (r.otherQueue || r.queue != queue)'),
        }
        source=read(RULE)
        for index,(name,(old,new)) in enumerate(mutants.items()):
            assert source.count(old)==1,(name,'mutation anchor drift')
            shadow=temp/f'm{index}'; path=shadow/RULE; path.parent.mkdir(parents=True)
            path.write_text(source.replace(old,new)); (shadow/TABLE).write_text(read(TABLE)); target=shadow/'cases'
            subprocess.run(FLAGS+['-I'+str(shadow),'-I'+str(temp),'-I'+str(ROOT),str(HERE/'cases.cpp'),'-o',str(target)],check=True,stdout=subprocess.DEVNULL)
            result=subprocess.run([str(target)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            assert result.returncode != 0, f'mutant survived: {name}'
        print(f'PASS: {len(mutants)} behavioral mutants rejected')
    # Full adapter syntax/type-check against real Windows interfaces, without Wine/MSVC or linking a DLL.
    compiler=shutil.which('x86_64-w64-mingw32-g++')
    if compiler:
        stubs=temp/'windows'; stubs.mkdir()
        files={
            'pch.h':'#pragma once\n#include <windows.h>\n#include <d3d12.h>\n#include <dxgi1_4.h>\n',
            'detours/detours.h':'#pragma once\nLONG WINAPI DetourTransactionBegin();\nLONG WINAPI DetourTransactionAbort();\nLONG WINAPI DetourTransactionCommit();\nLONG WINAPI DetourUpdateThread(HANDLE);\nLONG WINAPI DetourAttach(PVOID*,PVOID);\nLONG WINAPI DetourDetach(PVOID*,PVOID);\n',
            'Logger.h':'#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n',
            'Config.h':'''#pragma once
#include <string>
template<class T> struct Option { T value_or_default() const { return {}; } };
struct Config { static Config* Instance(); Option<bool> DlssNrEnabled; Option<unsigned> DlssNrHookMethod; Option<std::string> DlssNrPresentDepth; };
''',
            'State.h':'''#pragma once
enum class SwapchainInteropApi { None };
struct State { static State& Instance(); SwapchainInteropApi swapchainInteropApi; bool isShuttingDown; };
struct ScopedInternalResourceCreation { static bool Active(); };
''',
            'resource_tracking/ResTrack_Dx12.h':'''#pragma once
struct ResTrack_Dx12 { static IUnknown* NrRealObject(IUnknown*); static bool EnableNrSubmissionTracking(ID3D12Device*); };
''',
        }
        for name,content in files.items():
            path=stubs/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(content)
        subprocess.run([compiler,'-std=c++20','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unknown-pragmas','-I'+str(stubs),'-I'+str(ROOT/'OptiScaler'),'-I'+str(ROOT/'OptiScaler/include'),'-I'+str(ROOT/'retoque'),str(ROOT/GLUE)],check=True)
        print('PASS: complete observer adapter syntax and hook signatures against MinGW Windows SDK (no DLL build/runtime)')
    else:
        print('NOT_RUN: optional MinGW SDK syntax check (compiler unavailable); portable host contracts still executed')
