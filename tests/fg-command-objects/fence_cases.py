#!/usr/bin/env python3
"""Production SC allocator getters/waits/signals plus FSR allocator reset under scripted fences."""
from pathlib import Path
import subprocess
import tempfile
import re
ROOT = Path(__file__).resolve().parents[2]
base = (ROOT / 'OptiScaler/framegen/IFGFeature_Dx12.cpp').read_text()
fsr = (ROOT / 'OptiScaler/framegen/ffx/FSRFG_Dx12.cpp').read_text()
def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
unit = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <iostream>
#include <algorithm>
using UINT64 = uint64_t; using UINT = unsigned; using DWORD = unsigned; using HANDLE = void*;
using HRESULT = int; constexpr int S_OK=0, WAIT_OBJECT_0=0, WAIT_TIMEOUT=258, WAIT_FAILED=-1;
#define FAILED(x) ((x)<0)
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define BUFFER_COUNT 4
static UINT64 tick=0; static int waitResult=WAIT_OBJECT_0; static UINT64 wakeCompleted=0;
UINT64 GetTickCount64(){ return tick; }
struct ID3D12Fence { UINT64 completed=0; int registration=0;
 UINT64 GetCompletedValue(){ return completed; }
 int SetEventOnCompletion(UINT64,HANDLE){ return registration; }
};
static ID3D12Fence* waiting=nullptr;
DWORD WaitForSingleObject(HANDLE,DWORD ms){ tick += std::min(ms,1000u); if(waiting) waiting->completed=wakeCompleted; return waitResult; }
struct ID3D12Device {};
struct Allocator { unsigned resets=0; int Reset(){++resets;return 0;} };
struct ID3D12GraphicsCommandList { int Close(){return 0;} int Reset(Allocator*,void*){return 0;} };
using ID3D12CommandList=ID3D12GraphicsCommandList;
struct Queue { int signal=0; void ExecuteCommandLists(int,ID3D12CommandList**){ } int Signal(ID3D12Fence*,UINT64){return signal;} };
struct State { ID3D12Device* currentD3D12Device=nullptr; static State& Instance(){static State s;return s;} };
class IFGFeature_Dx12 { public:
 ID3D12Device* _device=nullptr; Queue* _gameCommandQueue=nullptr;
 ID3D12Fence* _scFence=nullptr; HANDLE _scFenceEvent=(void*)1; UINT64 _scFenceValue=0;
 std::mutex _scFenceMutex; Allocator* _scCommandAllocator[4]{};
 ID3D12GraphicsCommandList* _scCommandList[4]{}; bool _scCommandListResetted[4]{};
 UINT64 _scAllocatorFenceValues[4]{};
 Allocator* _fgCommandAllocator[4]{}; ID3D12GraphicsCommandList* _fgCommandList[4]{};
 UINT64 _fgAllocatorFenceValues[4]{};
 int GetIndex(){return 0;} void CreateObjects(ID3D12Device*){}
 bool WaitForFenceValue(ID3D12Fence*,HANDLE,UINT64);
 UINT64 SignalSCFence();
 ID3D12GraphicsCommandList* GetSCCommandList(int index);
 bool PrepareFSR(int fIndex);
};
'''
for signature in ['bool IFGFeature_Dx12::WaitForFenceValue(', 'UINT64 IFGFeature_Dx12::SignalSCFence(',
                  'ID3D12GraphicsCommandList* IFGFeature_Dx12::GetSCCommandList(']:
    unit += body(base,signature)+'\n'
a=fsr.index('        auto allocator = _fgCommandAllocator[fIndex];')
b=fsr.index('        dfgPrepare.commandList',a)
unit+='bool IFGFeature_Dx12::PrepareFSR(int fIndex) {\n'+fsr[a:b]+'return true; }\n'
unit+=r'''
int main(){
 IFGFeature_Dx12 fg; ID3D12Fence fence; Queue queue; Allocator allocator; ID3D12GraphicsCommandList list;
 fg._scFence=&fence; fg._gameCommandQueue=&queue; fg._scCommandAllocator[0]=&allocator;
 fg._scCommandList[0]=&list; fg._fgCommandAllocator[0]=&allocator; fg._fgCommandList[0]=&list;
 assert(fg.WaitForFenceValue(nullptr,nullptr,0));
 assert(!fg.WaitForFenceValue(nullptr,(void*)1,3));
 fence.completed=UINT64_MAX; assert(!fg.WaitForFenceValue(&fence,(void*)1,3));
 fence.completed=0; waiting=&fence; wakeCompleted=0; tick=0;
 assert(!fg.WaitForFenceValue(&fence,(void*)1,3)); // stale wake cannot prove completion
 fence.completed=0; wakeCompleted=3; tick=0;
 assert(fg.WaitForFenceValue(&fence,(void*)1,3));
 for(int scenario=0; scenario<4; ++scenario){
   fence.completed=scenario==1 ? UINT64_MAX : 0; wakeCompleted=fence.completed;
   queue.signal=scenario==3 ? -1 : 0; waitResult=scenario==2 ? WAIT_FAILED : WAIT_TIMEOUT; tick=0;
   UINT64 value=scenario==3 ? fg.SignalSCFence() : 3;
   fg._scAllocatorFenceValues[0]=value; fg._fgAllocatorFenceValues[0]=value;
   assert(fg.GetSCCommandList(0)==nullptr); assert(allocator.resets==0);
   tick=0; assert(!fg.PrepareFSR(0)); assert(allocator.resets==0);
 }
 fence.completed=3; fg._scAllocatorFenceValues[0]=3; fg._fgAllocatorFenceValues[0]=3;
 assert(fg.GetSCCommandList(0)==&list); assert(fg.PrepareFSR(0)); assert(allocator.resets==2);
 std::cout<<"PASS: actual SC/FSR reset refuses timeout, lost device, failed wait and failed Signal; stale wakes rechecked\n";
}
'''
with tempfile.TemporaryDirectory(prefix='retoque-fg-fence-') as folder:
    cpp=Path(folder)/'case.cpp'; exe=Path(folder)/'case'; cpp.write_text(unit)
    subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined','-g',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
